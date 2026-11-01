// The LiquidSurface table and the look grid (SXC/Water/Surface.h).
#include "SXC/Water/Surface.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "SXC/Data/Data.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/CChunkLiquid.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"

namespace SXC::Water {
    namespace {
        const Look kDefaults;

        // The client's world grid: 64 tiles of 16 chunks; chunk x runs along -y, chunk y along -x.
        constexpr float kHalfWorld = 32.0f * 533.33333f;
        constexpr float kChunk = 533.33333f / 16.0f;

        Data::Schema MakeSchema() {
            Data::Schema s("LiquidSurface", "DBFilesClient\\LiquidSurface.cdbc");
            s.Description("Our water's look per water: a row per LiquidType (0: every water) and place. The most specific row wins: the "
                          "water's own type before the every-water row, then a tile before an area (a sub-area before its zone) before a "
                          "map before everywhere. Looks fade into each other over a terrain chunk (~33 yd) where places meet. The Water "
                          "panel's \"Write\" fills one; save here.");
            s.Id();
            s.Uint("LiquidType").Group("Place").Tooltip("LiquidType.dbc's ID (1 Water, 2 Ocean, 17 WMO Water - Interior, ...); 0: every water.");
            s.String("Scope").Group("Place").Tooltip("Empty: everywhere. A map's folder name (\"Kalimdor\"): that map. An area's name "
                                                     "(\"Elwynn Forest\", \"Crystal Lake\"; every area of that name) or \"area:<AreaTable ID>\": "
                                                     "that area and its sub-areas. An ADT's name (\"Kalimdor_23_30\"): that tile. Any case.");
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
            // After the comment: added 2026-10-30, and older files are read by position (they lack these).
            s.Float("ShoreFoam").Group("Shore").Default(kDefaults.shoreFoam).Range(0, 2)
                .Tooltip("The shore's foam (the waterline, breaking crests, the swash). The row of the water the player is in.");
            s.Float("ShoreWaveHeight").Group("Shore").Default(kDefaults.shoreWaveHeight).Range(0, 0.6).Tooltip("yd: the shore waves' height.");
            s.Float("ShoreWavelength").Group("Shore").Default(kDefaults.shoreWavelength).Range(2, 20).Tooltip("yd between the shore waves.");
            s.Float("ShoreReach").Group("Shore").Default(kDefaults.shoreReach).Range(4, 60).Tooltip("yd: how far out the shore waves begin.");
            s.Float("Swash").Group("Shore").Default(kDefaults.swash).Range(0, 2)
                .Tooltip("yd: how high the water runs up the beach as each wave arrives (a flat beach far, a steep one hardly). 0 off.");
            // Added 2026-10-31, after the rest for the same reason.
            s.Float("Caustics").Group("Underwater").Default(kDefaults.caustics).Range(0, 3)
                .Tooltip("The sunlight's caustics on the floor under the water (0 none: murky water). The row of the water the player is in.");
            s.Float("CausticScale").Group("Underwater").Default(kDefaults.causticScale).Range(0.5, 10).Tooltip("yd: the size of the caustics' cells.");
            s.Float("CausticDepth").Group("Underwater").Default(kDefaults.causticDepth).Range(0.5, 40).Tooltip("yd: how deep the caustics reach before they fade out.");
            return s;
        }

        std::string Lower(const std::string& text) {
            std::string out = text;

            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            return out;
        }

        std::string Trimmed(const std::string& text) {
            const size_t begin = text.find_first_not_of(" \t");
            return begin == std::string::npos ? std::string() : text.substr(begin, text.find_last_not_of(" \t") + 1 - begin);
        }

        bool Digits(const std::string& t) {
            return !t.empty() && t.find_first_not_of("0123456789") == std::string::npos;
        }

        // The AreaTable rows by name (lower case), made once the table is there.
        std::unordered_map<std::string, std::vector<uint32_t>> s_areasByName;

        const std::unordered_map<std::string, std::vector<uint32_t>>& AreasByName() {
            if (s_areasByName.empty()) {
                for (int32_t i = 0; i < g_areaTableDB.m_numRecords; i++) {
                    const AreaTableRec* rec = g_areaTableDB.GetRecordByIndex(i);

                    if (rec && rec->m_areaName && rec->m_areaName[0]) {
                        s_areasByName[Lower(rec->m_areaName)].push_back(static_cast<uint32_t>(rec->m_id));
                    }
                }
            }

            return s_areasByName;
        }

        bool IsMapName(const std::string& lower) {
            for (int32_t i = 0; i < g_mapDB.m_numRecords; i++) {
                const MapRec* rec = g_mapDB.GetRecordByIndex(i);

                if (rec && rec->m_directory && Lower(rec->m_directory) == lower) {
                    return true;
                }
            }

            return false;
        }

        // An area and its parents (sub-area -> zone), the area first.
        const std::vector<uint32_t>& AreaChain(uint32_t area) {
            static std::unordered_map<uint32_t, std::vector<uint32_t>> s_chains;
            const auto found = s_chains.find(area);

            if (found != s_chains.end()) {
                return found->second;
            }

            std::vector<uint32_t> chain;

            // The cap guards against a malformed (cyclic) table.
            for (uint32_t id = area; id && chain.size() < 8;) {
                chain.push_back(id);
                const AreaTableRec* rec = g_areaTableDB.GetRecord(static_cast<int32_t>(id));

                if (!rec || static_cast<uint32_t>(rec->m_parentAreaID) == id) {
                    break;
                }

                id = static_cast<uint32_t>(rec->m_parentAreaID);
            }

            return s_chains.emplace(area, std::move(chain)).first->second;
        }

        enum class Kind { Global, Map, Area, Tile };

        struct Entry {
            uint32_t liquidType = 0;
            Kind kind = Kind::Global;
            std::string map;
            int32_t x = 0, y = 0;
            std::vector<uint32_t> areas;   // Kind::Area: every area the Scope names
            Look look;
        };

        Entry ParseScope(const std::string& scope) {
            Entry e;
            const std::string trimmed = Lower(Trimmed(scope));

            if (trimmed.empty()) {
                return e;
            }

            if (trimmed.rfind("area", 0) == 0 && trimmed.size() > 5 && (trimmed[4] == ':' || trimmed[4] == ' ') && Digits(Trimmed(trimmed.substr(5)))) {
                e.kind = Kind::Area;
                e.areas.push_back(static_cast<uint32_t>(std::atoi(Trimmed(trimmed.substr(5)).c_str())));
                return e;
            }

            const size_t second = trimmed.rfind('_');
            const size_t first = second == std::string::npos || second == 0 ? std::string::npos : trimmed.rfind('_', second - 1);

            if (first != std::string::npos && Digits(trimmed.substr(first + 1, second - first - 1)) && Digits(trimmed.substr(second + 1))) {
                e.kind = Kind::Tile;
                e.map = trimmed.substr(0, first);
                e.x = std::atoi(trimmed.c_str() + first + 1);
                e.y = std::atoi(trimmed.c_str() + second + 1);
                return e;
            }

            // A map's folder name first; else an area's name; else a map we don't know (it never applies).
            const auto& byName = AreasByName();
            const auto area = byName.find(trimmed);

            if (!IsMapName(trimmed) && area != byName.end()) {
                e.kind = Kind::Area;
                e.areas = area->second;
                return e;
            }

            e.kind = Kind::Map;
            e.map = trimmed;
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

        // Where the player is: the map's folder name (lower case), the chunk, its tile and area.
        std::string s_map;
        int32_t s_chunkX = -1, s_chunkY = -1;
        int32_t s_tileX = -1, s_tileY = -1;
        uint32_t s_area = 0;
        std::chrono::steady_clock::time_point s_placeChecked;

        // Which row a (LiquidType, area, tile) takes, for this generation and map: an index into s_entries, -1 none.
        std::unordered_map<uint64_t, int32_t> s_resolved;
        uint32_t s_resolvedGeneration = ~0u;
        std::string s_resolvedMap;

        CGPlayer_C* Player() {
            const WOWGUID guid = ClntObjMgrGetActivePlayer();
            return guid ? static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(guid, TYPE_PLAYER, __FILE__, __LINE__)) : nullptr;
        }

        // The chunk (client counting) a world point lies in; false outside the world.
        bool ChunkOf(float x, float y, int32_t& cx, int32_t& cy) {
            const float fx = (kHalfWorld - y) / kChunk, fy = (kHalfWorld - x) / kChunk;

            if (!(fx >= 0.0f && fy >= 0.0f && fx < 1024.0f && fy < 1024.0f)) {
                return false;
            }

            cx = static_cast<int32_t>(fx);
            cy = static_cast<int32_t>(fy);
            return true;
        }

        CMapArea* TileOf(int32_t cx, int32_t cy) {
            if (cx < 0 || cy < 0 || cx >= 1024 || cy >= 1024) {
                return nullptr;
            }

            CMapArea* area = CMap::m_areaTable[64 * (cy >> 4) + (cx >> 4)];
            return area && !area->m_asyncObject ? area : nullptr;
        }

        CMapChunk* ChunkAt(int32_t cx, int32_t cy) {
            CMapArea* tile = TileOf(cx, cy);
            return tile ? tile->m_mapChunks[(cx & 0xF) + 16 * (cy & 0xF)] : nullptr;
        }

        void UpdatePlace() {
            const auto now = std::chrono::steady_clock::now();

            if (now - s_placeChecked < std::chrono::milliseconds(250)) {
                return;
            }

            s_placeChecked = now;
            s_map = Lower(CMap::mapName);
            s_chunkX = s_chunkY = s_tileX = s_tileY = -1;
            s_area = 0;
            float x = 0.0f, y = 0.0f;

            if (PlayerPosition(x, y) && ChunkOf(x, y, s_chunkX, s_chunkY)) {
                if (CMapArea* tile = TileOf(s_chunkX, s_chunkY)) {
                    s_tileX = tile->m_index.x;
                    s_tileY = tile->m_index.y;
                }

                if (CMapChunk* chunk = ChunkAt(s_chunkX, s_chunkY)) {
                    s_area = static_cast<uint32_t>(chunk->m_areaId);
                }
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
                      foam = schema.IndexOf("Foam"), threshold = schema.IndexOf("FoamThreshold"), move = schema.IndexOf("SurfaceMovement"),
                      shoreFoam = schema.IndexOf("ShoreFoam"), shoreHeight = schema.IndexOf("ShoreWaveHeight"),
                      shoreLength = schema.IndexOf("ShoreWavelength"), shoreReach = schema.IndexOf("ShoreReach"), swash = schema.IndexOf("Swash"),
                      caustics = schema.IndexOf("Caustics"), causticScale = schema.IndexOf("CausticScale"), causticDepth = schema.IndexOf("CausticDepth");

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
                l.shoreFoam = row.Float(shoreFoam);
                l.shoreWaveHeight = row.Float(shoreHeight);
                l.shoreWavelength = row.Float(shoreLength);
                l.shoreReach = row.Float(shoreReach);
                l.swash = row.Float(swash);
                l.caustics = row.Float(caustics);
                l.causticScale = row.Float(causticScale);
                l.causticDepth = row.Float(causticDepth);
                s_entries.push_back(std::move(e));
            }
        }

        // How specific a row is at a place (higher wins), -1 when it doesn't apply there.
        int32_t Rank(const Entry& e, const std::vector<uint32_t>& chain, int32_t tileX, int32_t tileY) {
            switch (e.kind) {
                case Kind::Global:
                    return 0;
                case Kind::Map:
                    return e.map == s_map ? 1 : -1;
                case Kind::Tile:
                    return e.map == s_map && e.x == tileX && e.y == tileY ? 100 : -1;
                case Kind::Area:
                    // The deeper in the chain (a sub-area below its zone), the more specific.
                    for (size_t k = 0; k < chain.size(); k++) {
                        for (uint32_t id : e.areas) {
                            if (id == chain[k]) {
                                return 2 + static_cast<int32_t>(chain.size() - 1 - k);
                            }
                        }
                    }

                    return -1;
            }

            return -1;
        }

        int32_t Best(uint32_t liquidType, const std::vector<uint32_t>& chain, int32_t tileX, int32_t tileY) {
            int32_t best = -1, bestRank = -1;

            for (size_t i = 0; i < s_entries.size(); i++) {
                const Entry& e = s_entries[i];

                if (e.liquidType != liquidType) {
                    continue;
                }

                const int32_t rank = Rank(e, chain, tileX, tileY);

                if (rank > bestRank) {
                    best = static_cast<int32_t>(i);
                    bestRank = rank;
                }
            }

            return best;
        }

        // The row a water takes at a place: its type's, else the every-water row's; -1: the defaults.
        int32_t Resolve(uint32_t liquidType, uint32_t area, int32_t tileX, int32_t tileY) {
            Rebuild();

            if (s_resolvedGeneration != s_generation || s_resolvedMap != s_map) {
                s_resolved.clear();
                s_resolvedGeneration = s_generation;
                s_resolvedMap = s_map;
            }

            const uint64_t key = static_cast<uint64_t>(liquidType & 0xFFFF) << 48 | static_cast<uint64_t>(area & 0xFFFFFFu) << 16 |
                                 static_cast<uint64_t>(tileX & 0xFF) << 8 | static_cast<uint64_t>(tileY & 0xFF);
            const auto found = s_resolved.find(key);

            if (found != s_resolved.end()) {
                return found->second;
            }

            const std::vector<uint32_t>& chain = AreaChain(area);
            int32_t e = Best(liquidType, chain, tileX, tileY);

            if (e < 0 && liquidType != 0) {
                e = Best(0, chain, tileX, tileY);
            }

            return s_resolved.emplace(key, e).first->second;
        }

        const Look& LookOf(int32_t entry) {
            return entry >= 0 && entry < static_cast<int32_t>(s_entries.size()) ? s_entries[entry].look : kDefaults;
        }

        // --- the look grid ---------------------------------------------------------------------------

        struct Cell {
            bool known = false, wet = false;
            uint32_t type = 0, area = 0;
            int32_t tileX = -1, tileY = -1;
        };

        LookGrid s_grid;
        std::vector<Cell> s_cells(kGridSize * kGridSize);
        std::chrono::steady_clock::time_point s_gridBuilt;
        uint32_t s_gridGeneration = ~0u;
        int32_t s_gridChunkX = -1, s_gridChunkY = -1;

        // The terrain liquid a chunk holds (its first water layer's LiquidType), 0 when dry.
        uint32_t ChunkWater(CMapChunk* chunk) {
            for (CChunkLiquid* liquid = chunk->m_liquidChunkLinkList.Head(); liquid; liquid = chunk->m_liquidChunkLinkList.Next(liquid)) {
                if (liquid->m_type > 0) {
                    return static_cast<uint32_t>(liquid->m_type);
                }
            }

            return 0;
        }

        void BuildGrid() {
            UpdatePlace();
            Rebuild();
            const auto now = std::chrono::steady_clock::now();
            const bool moved = s_chunkX != s_gridChunkX || s_chunkY != s_gridChunkY;

            if (!moved && s_gridGeneration == s_generation && now - s_gridBuilt < std::chrono::milliseconds(500)) {
                return;
            }

            s_gridBuilt = now;
            s_gridGeneration = s_generation;
            s_gridChunkX = s_chunkX;
            s_gridChunkY = s_chunkY;

            if (s_chunkX < 0) {
                s_grid.valid = false;
                return;
            }

            const int32_t ox = s_chunkX - kGridSize / 2, oy = s_chunkY - kGridSize / 2;

            for (int32_t j = 0; j < kGridSize; j++) {
                for (int32_t i = 0; i < kGridSize; i++) {
                    Cell& c = s_cells[j * kGridSize + i];
                    c = Cell();
                    CMapArea* tile = TileOf(ox + i, oy + j);
                    CMapChunk* chunk = tile ? tile->m_mapChunks[((ox + i) & 0xF) + 16 * ((oy + j) & 0xF)] : nullptr;

                    if (!chunk) {
                        continue;
                    }

                    c.known = true;
                    c.area = static_cast<uint32_t>(chunk->m_areaId);
                    c.tileX = tile->m_index.x;
                    c.tileY = tile->m_index.y;
                    c.type = ChunkWater(chunk);
                    c.wet = c.type != 0;
                }
            }

            // A dry chunk next to water takes that water's place, so the water's look doesn't fade toward
            // whatever the dry land would give (the blend runs between chunk centres).
            std::vector<Cell> filled = s_cells;
            static const int32_t kNear[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } };

            for (int32_t j = 0; j < kGridSize; j++) {
                for (int32_t i = 0; i < kGridSize; i++) {
                    if (s_cells[j * kGridSize + i].wet) {
                        continue;
                    }

                    for (const auto& d : kNear) {
                        const int32_t ni = i + d[0], nj = j + d[1];

                        if (ni >= 0 && nj >= 0 && ni < kGridSize && nj < kGridSize && s_cells[nj * kGridSize + ni].wet) {
                            filled[j * kGridSize + i] = s_cells[nj * kGridSize + ni];
                            break;
                        }
                    }
                }
            }

            // The palette: one slot per row taken (the defaults too), in the order met.
            std::unordered_map<int32_t, uint8_t> slots;
            s_grid.paletteCount = 0;

            for (int32_t k = 0; k < kGridSize * kGridSize; k++) {
                const Cell& c = filled[k];
                const int32_t entry = c.known ? Resolve(c.type, c.area, c.tileX, c.tileY) : -1;
                const auto found = slots.find(entry);
                uint8_t slot = 0;

                if (found != slots.end()) {
                    slot = found->second;
                } else if (s_grid.paletteCount < kPaletteSize) {
                    slot = static_cast<uint8_t>(s_grid.paletteCount++);
                    slots.emplace(entry, slot);
                    PackLook(LookOf(entry), s_grid.palette[slot]);
                }

                s_grid.index[k] = slot;
            }

            s_grid.originX = ox;
            s_grid.originY = oy;
            s_grid.valid = true;
        }
    }

    void PackLook(const Look& l, float out[kPackedLook]) {
        const float values[kPackedLook] = {
            l.clarityClear, l.clarityMurky, l.scatter, l.refraction,
            l.reflectivity, l.reflection, l.window, l.glint,
            l.shallow[0], l.shallow[1], l.shallow[2], l.colourMode == ColourMode::Hue ? 1.0f : 0.0f,
            l.deep[0], l.deep[1], l.deep[2], l.colourMode == ColourMode::Replace ? 1.0f : 0.0f,
            l.glintSharpness, l.waveScale, l.foam, l.move,
        };
        std::memcpy(out, values, sizeof(values));
    }

    void RegisterSurface() {
        if (!s_table) {
            s_table = Data::Register(MakeSchema());
        }
    }

    const LookGrid& SurfaceGrid() {
        BuildGrid();
        return s_grid;
    }

    const Look& SurfaceLookAt(uint32_t liquidType, float x, float y) {
        UpdatePlace();
        uint32_t area = s_area;
        int32_t tileX = s_tileX, tileY = s_tileY, cx = 0, cy = 0;

        if (ChunkOf(x, y, cx, cy)) {
            if (CMapArea* tile = TileOf(cx, cy)) {
                tileX = tile->m_index.x;
                tileY = tile->m_index.y;

                if (CMapChunk* chunk = tile->m_mapChunks[(cx & 0xF) + 16 * (cy & 0xF)]) {
                    area = static_cast<uint32_t>(chunk->m_areaId);
                }
            }
        }

        return LookOf(Resolve(liquidType, area, tileX, tileY));
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

    bool PlayerPosition(float& x, float& y) {
        CGPlayer_C* player = Player();

        if (!player) {
            return false;
        }

        const C3Vector p = player->GetPosition();
        x = p.x;
        y = p.y;
        return true;
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

    bool ScopeHere(ScopeKind kind, std::string& scope, std::string& label) {
        UpdatePlace();
        scope.clear();
        label.clear();
        char text[160];

        // An area by its name when the name is its own, else by its ID.
        auto areaScope = [&](uint32_t id, const char* what) {
            const AreaTableRec* rec = g_areaTableDB.GetRecord(static_cast<int32_t>(id));
            const char* name = rec && rec->m_areaName && rec->m_areaName[0] ? rec->m_areaName : nullptr;
            const auto& byName = AreasByName();
            const auto same = name ? byName.find(Lower(name)) : byName.end();

            if (name && same != byName.end() && same->second.size() == 1 && !IsMapName(Lower(name))) {
                scope = name;
            } else {
                std::snprintf(text, sizeof text, "area:%u", id);
                scope = text;
            }

            std::snprintf(text, sizeof text, "%s %s", what, name ? name : scope.c_str());
            label = text;
            return true;
        };

        switch (kind) {
            case ScopeKind::Everywhere:
                label = "everywhere";
                return true;
            case ScopeKind::Map:
                if (!CMap::mapName[0]) {
                    return false;
                }

                scope = CMap::mapName;
                label = "map " + scope;
                return true;
            case ScopeKind::Zone: {
                const std::vector<uint32_t>& chain = AreaChain(s_area);
                return !chain.empty() && areaScope(chain.back(), "zone");
            }
            case ScopeKind::SubArea: {
                const std::vector<uint32_t>& chain = AreaChain(s_area);
                return chain.size() > 1 && areaScope(chain.front(), "sub-area");
            }
            case ScopeKind::Tile:
                if (s_tileX < 0 || !CMap::mapName[0]) {
                    return false;
                }

                std::snprintf(text, sizeof text, "%s_%d_%d", CMap::mapName, s_tileX, s_tileY);
                scope = text;
                label = "tile " + scope;
                return true;
        }

        return false;
    }

    int32_t ScopeRankHere(const std::string& scope) {
        UpdatePlace();
        return Rank(ParseScope(scope), AreaChain(s_area), s_tileX, s_tileY);
    }

    bool WriteSurfaceRow(uint32_t liquidType, const std::string& place, const Look& l) {
        if (!s_table) {
            return false;
        }

        const Data::Schema& schema = s_table->GetSchema();
        const int type = schema.IndexOf("LiquidType"), scope = schema.IndexOf("Scope");
        const std::string wanted = Lower(Trimmed(place));
        Data::Row row;

        for (uint32_t i = 0; i < s_table->RowCount() && !row; i++) {
            Data::Row candidate = s_table->At(i);

            if (candidate.Uint(type) == liquidType && Lower(Trimmed(candidate.String(scope))) == wanted) {
                row = candidate;
            }
        }

        if (!row) {
            row = s_table->Add();
            row.SetUint(type, 0, liquidType);
            row.SetString(scope, 0, place.c_str());
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
        set("ShoreFoam", l.shoreFoam);
        set("ShoreWaveHeight", l.shoreWaveHeight);
        set("ShoreWavelength", l.shoreWavelength);
        set("ShoreReach", l.shoreReach);
        set("Swash", l.swash);
        set("Caustics", l.caustics);
        set("CausticScale", l.causticScale);
        set("CausticDepth", l.causticDepth);
        return true;
    }
}
