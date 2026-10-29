// The LiquidSurface table (SXC/Water/Surface.h).
#include "SXC/Water/Surface.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include "Engine/Source/Tempest/CMath.h"
#include "SXC/Data/Data.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"

namespace SXC::Water {
    namespace {
        const Look kDefaults;

        Data::Schema MakeSchema() {
            Data::Schema s("LiquidSurface", "DBFilesClient\\LiquidSurface.cdbc");
            s.Description("Our water's look per water: a row per LiquidType (0: every water) and place. The most specific row wins: the "
                          "water's own type before the every-water row, a tile before a map before everywhere. The Water panel's "
                          "\"Write to this water's row\" fills one; save here.");
            s.Id();
            s.Uint("LiquidType").Group("Place").Tooltip("LiquidType.dbc's ID (1 Water, 2 Ocean, 17 WMO Water - Interior, ...); 0: every water.");
            s.String("Scope").Group("Place").Tooltip("Empty: everywhere. A map's folder name (\"Kalimdor\"): that map. An ADT's name "
                                                     "(\"Kalimdor_23_30\"): that tile. Any case. Read where the player is.");
            s.Float("ClarityClear").Group("Water").Default(kDefaults.clarityClear).Range(0, 0.5)
                .Tooltip("Absorption per yd where the stock tint's alpha is 0 (clear). Red goes first: the deep turns to the water's colour.");
            s.Float("ClarityMurky").Group("Water").Default(kDefaults.clarityMurky).Range(0, 1).Tooltip("Absorption per yd where the stock alpha is 1.");
            s.Float("OwnColour").Group("Water").Default(kDefaults.scatter).Range(0, 2).Tooltip("The light the water itself sends back, x its colour.");
            s.Float("Refraction").Group("Water").Default(kDefaults.refraction).Range(0, 4).Tooltip("How far the waves bend what lies behind.");
            s.Enum("ColourMode").Group("Colour").Value(0, "Stock").Value(1, "Hue").Value(2, "Replace")
                .Tooltip("The water's colour. Stock: the light data's shallow and deep colour, through the day. Hue: their brightness "
                         "through the day, our hue. Replace: our colours.");
            s.Color("Shallow").Group("Colour").Default(0x4DB3B3).Tooltip("At the shore (Hue, Replace).");
            s.Color("Deep").Group("Colour").Default(0x0D3359).Tooltip("A few yards down (Hue, Replace).");
            s.Float("Reflectivity").Group("Surface").Default(kDefaults.reflectivity).Range(0, 1).Tooltip("Looking straight down (real water 0.02).");
            s.Float("Reflection").Group("Surface").Default(kDefaults.reflection).Range(0, 8).Tooltip("x the reflection.");
            s.Float("Window").Group("Surface").Default(kDefaults.window).Range(0, 0.95).Tooltip("Snell's window from below, as a cos (real water 0.66; "
                                                                                              "lower: wider, for a third-person camera).");
            s.Float("Glint").Group("Surface").Default(kDefaults.glint).Range(0, 10).Tooltip("The sky bodies' highlight.");
            s.Float("GlintSharpness").Group("Surface").Default(kDefaults.glintSharpness).Range(10, 5000);
            s.Float("WaveScale").Group("Waves").Default(kDefaults.waveScale).Range(0, 4).Tooltip("x the shared sea, this water (its shading and movement).");
            s.Float("WaveHeight").Group("Waves").Default(kDefaults.amplitude).Range(0, 4)
                .Tooltip("The shared sea's height, from the row of the water the player is in.");
            s.Float("Choppiness").Group("Waves").Default(kDefaults.chop).Range(0, 4).Tooltip("The shared sea's: sharper crests.");
            s.Float("Spread").Group("Waves").Default(kDefaults.spread).Range(0, 1).Tooltip("The shared sea's: 0 all with the wind .. 1 every way.");
            s.Float("Foam").Group("Waves").Default(kDefaults.foam).Range(0, 4);
            s.Float("FoamThreshold").Group("Waves").Default(kDefaults.foamThreshold).Range(0, 2).Tooltip("The shared sea's: crests foam below this Jacobian.");
            s.Float("SurfaceMovement").Group("Waves").Default(kDefaults.move).Range(0, 3).Tooltip("The surface's displacement (0 flat).");
            s.Text("Comment").Tooltip("Free text for the editor; never read.");
            return s;
        }

        std::string Lower(const std::string& text) {
            std::string out = text;

            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            return out;
        }

        enum class Kind { Global, Map, Tile };

        struct Entry {
            uint32_t liquidType = 0;
            Kind kind = Kind::Global;
            std::string map;
            int32_t x = 0, y = 0;
            Look look;
        };

        Entry ParseScope(const std::string& scope) {
            Entry e;
            const size_t begin = scope.find_first_not_of(" \t");

            if (begin == std::string::npos) {
                return e;
            }

            const std::string trimmed = Lower(scope.substr(begin, scope.find_last_not_of(" \t") + 1 - begin));
            const size_t second = trimmed.rfind('_');
            const size_t first = second == std::string::npos || second == 0 ? std::string::npos : trimmed.rfind('_', second - 1);
            auto digits = [](const std::string& t) { return !t.empty() && t.find_first_not_of("0123456789") == std::string::npos; };

            if (first != std::string::npos && digits(trimmed.substr(first + 1, second - first - 1)) && digits(trimmed.substr(second + 1))) {
                e.kind = Kind::Tile;
                e.map = trimmed.substr(0, first);
                e.x = std::atoi(trimmed.c_str() + first + 1);
                e.y = std::atoi(trimmed.c_str() + second + 1);
            } else {
                e.kind = Kind::Map;
                e.map = trimmed;
            }

            return e;
        }

        void Rgb(uint32_t rgb, float out[3]) {
            out[0] = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
            out[1] = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
            out[2] = static_cast<float>(rgb & 0xFF) / 255.0f;
        }

        uint32_t Rgb(const float in[3]) {
            auto byte = [](float v) { return static_cast<uint32_t>((v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) * 255.0f + 0.5f); };
            return byte(in[0]) << 16 | byte(in[1]) << 8 | byte(in[2]);
        }

        Data::Table* s_table = nullptr;
        uint32_t s_generation = ~0u;
        std::vector<Entry> s_entries;

        // Where the player is: the map's folder name (lower case), the ADT; refreshed four times a second.
        std::string s_map;
        int32_t s_tileX = -1, s_tileY = -1;
        std::chrono::steady_clock::time_point s_placeChecked;
        uint32_t s_placeVersion = 0;

        // The resolved looks for this generation and place.
        std::unordered_map<uint32_t, Look> s_resolved;
        uint32_t s_resolvedGeneration = ~0u, s_resolvedPlace = ~0u;

        CGPlayer_C* Player() {
            const WOWGUID guid = ClntObjMgrGetActivePlayer();
            return guid ? static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(guid, TYPE_PLAYER, __FILE__, __LINE__)) : nullptr;
        }

        void UpdatePlace() {
            const auto now = std::chrono::steady_clock::now();

            if (now - s_placeChecked < std::chrono::milliseconds(250)) {
                return;
            }

            s_placeChecked = now;
            std::string map = Lower(CMap::mapName);
            int32_t x = -1, y = -1;

            if (CGPlayer_C* player = Player()) {
                // As CMap finds the area of a point (SXC/Terrain's CurrentTile).
                const float half = 32.0f * 533.33333f;
                const C3Vector p = player->GetPosition();
                const float fx = -(p.x - half), fy = -(p.y - half);

                if (fx >= 0.0f && fy >= 0.0f && fx < 2.0f * half && fy < 2.0f * half) {
                    const int32_t cellX = CMath::fint_(fy * 0.24f - 0.5f), cellY = CMath::fint_(fx * 0.24f - 0.5f);

                    if (const CMapArea* area = CMap::m_areaTable[64 * ((cellY >> 7) & 0x3F) + ((cellX >> 7) & 0x3F)]) {
                        x = area->m_index.x;
                        y = area->m_index.y;
                    }
                }
            }

            if (map != s_map || x != s_tileX || y != s_tileY) {
                s_map = map;
                s_tileX = x;
                s_tileY = y;
                s_placeVersion++;
            }
        }

        void Rebuild() {
            if (!s_table || s_table->Generation() == s_generation) {
                return;
            }

            s_generation = s_table->Generation();
            s_entries.clear();
            const Data::Schema& schema = s_table->GetSchema();
            const int type = schema.IndexOf("LiquidType"), scope = schema.IndexOf("Scope"), clear = schema.IndexOf("ClarityClear"),
                      murky = schema.IndexOf("ClarityMurky"), own = schema.IndexOf("OwnColour"), refraction = schema.IndexOf("Refraction"),
                      mode = schema.IndexOf("ColourMode"), shallow = schema.IndexOf("Shallow"), deep = schema.IndexOf("Deep"),
                      reflectivity = schema.IndexOf("Reflectivity"), reflection = schema.IndexOf("Reflection"), window = schema.IndexOf("Window"),
                      glint = schema.IndexOf("Glint"), sharpness = schema.IndexOf("GlintSharpness"), waveScale = schema.IndexOf("WaveScale"),
                      height = schema.IndexOf("WaveHeight"), chop = schema.IndexOf("Choppiness"), spread = schema.IndexOf("Spread"),
                      foam = schema.IndexOf("Foam"), threshold = schema.IndexOf("FoamThreshold"), move = schema.IndexOf("SurfaceMovement");

            for (uint32_t i = 0; i < s_table->RowCount(); i++) {
                const Data::Row row = s_table->At(i);
                Entry e = ParseScope(row.String(scope));
                e.liquidType = row.Uint(type);
                Look& l = e.look;
                l.clarityClear = row.Float(clear);
                l.clarityMurky = row.Float(murky);
                l.scatter = row.Float(own);
                l.refraction = row.Float(refraction);
                const int32_t m = row.Int(mode);
                l.colourMode = m == 1 ? ColourMode::Hue : (m == 2 ? ColourMode::Replace : ColourMode::Stock);
                Rgb(row.Uint(shallow), l.shallow);
                Rgb(row.Uint(deep), l.deep);
                l.reflectivity = row.Float(reflectivity);
                l.reflection = row.Float(reflection);
                l.window = row.Float(window);
                l.glint = row.Float(glint);
                l.glintSharpness = row.Float(sharpness);
                l.waveScale = row.Float(waveScale);
                l.amplitude = row.Float(height);
                l.chop = row.Float(chop);
                l.spread = row.Float(spread);
                l.foam = row.Float(foam);
                l.foamThreshold = row.Float(threshold);
                l.move = row.Float(move);
                s_entries.push_back(e);
            }
        }

        // The best row of one LiquidType here (a tile over a map over everywhere), or null.
        const Entry* Best(uint32_t liquidType) {
            const Entry* best = nullptr;

            for (const Entry& e : s_entries) {
                if (e.liquidType != liquidType) {
                    continue;
                }

                const bool applies = e.kind == Kind::Global || (e.map == s_map && (e.kind == Kind::Map || (e.x == s_tileX && e.y == s_tileY)));

                if (applies && (!best || static_cast<int>(e.kind) > static_cast<int>(best->kind))) {
                    best = &e;
                }
            }

            return best;
        }
    }

    void RegisterSurface() {
        if (!s_table) {
            s_table = Data::Register(MakeSchema());
        }
    }

    const Look& SurfaceLook(uint32_t liquidType) {
        UpdatePlace();
        Rebuild();

        if (s_resolvedGeneration != s_generation || s_resolvedPlace != s_placeVersion) {
            s_resolved.clear();
            s_resolvedGeneration = s_generation;
            s_resolvedPlace = s_placeVersion;
        }

        const auto found = s_resolved.find(liquidType);

        if (found != s_resolved.end()) {
            return found->second;
        }

        const Entry* e = Best(liquidType);

        if (!e && liquidType != 0) {
            e = Best(0);
        }

        return s_resolved.emplace(liquidType, e ? e->look : kDefaults).first->second;
    }

    uint32_t LiquidTypeOf(const void* settings) {
        TSGrowableArray<Liquid::CMaterialSettings*>& bank = Liquid::CSettingsBank::s_settings;

        for (uint32_t i = 0; settings && i < bank.Count(); i++) {
            if (bank.m_data[i] == settings) {
                return i;
            }
        }

        return 0;
    }

    uint32_t LiquidTypeHere() {
        CGPlayer_C* player = Player();

        if (!player) {
            return 0;
        }

        C3Vector p = player->GetPosition();
        int32_t type = 0, unused = 0;
        float height = 0.0f;
        return CMap::QueryLiquidStatus(&p, &type, &height, &unused, 1) && type > 0 ? static_cast<uint32_t>(type) : 0u;
    }

    bool WriteSurfaceRow(uint32_t liquidType, const Look& l) {
        if (!s_table) {
            return false;
        }

        const Data::Schema& schema = s_table->GetSchema();
        const int type = schema.IndexOf("LiquidType"), scope = schema.IndexOf("Scope");
        Data::Row row;

        for (uint32_t i = 0; i < s_table->RowCount() && !row; i++) {
            Data::Row candidate = s_table->At(i);
            const std::string& place = candidate.String(scope);

            if (candidate.Uint(type) == liquidType && place.find_first_not_of(" \t") == std::string::npos) {
                row = candidate;
            }
        }

        if (!row) {
            row = s_table->Add();
            row.SetUint(type, 0, liquidType);
        }

        auto set = [&](const char* name, float value) { row.SetFloat(schema.IndexOf(name), 0, value); };
        set("ClarityClear", l.clarityClear);
        set("ClarityMurky", l.clarityMurky);
        set("OwnColour", l.scatter);
        set("Refraction", l.refraction);
        row.SetInt(schema.IndexOf("ColourMode"), 0, static_cast<int32_t>(l.colourMode));
        row.SetUint(schema.IndexOf("Shallow"), 0, Rgb(l.shallow));
        row.SetUint(schema.IndexOf("Deep"), 0, Rgb(l.deep));
        set("Reflectivity", l.reflectivity);
        set("Reflection", l.reflection);
        set("Window", l.window);
        set("Glint", l.glint);
        set("GlintSharpness", l.glintSharpness);
        set("WaveScale", l.waveScale);
        set("WaveHeight", l.amplitude);
        set("Choppiness", l.chop);
        set("Spread", l.spread);
        set("Foam", l.foam);
        set("FoamThreshold", l.foamThreshold);
        set("SurfaceMovement", l.move);
        return true;
    }
}
