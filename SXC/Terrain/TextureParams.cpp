#include "SXC/Terrain/TextureParams.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "SXC/Data/Data.h"
#include "SXC/Settings/Settings.h"
#include "Engine/Source/Tempest/CMath.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"

namespace SXC::Terrain {
    namespace {
        // The MTXF bits the editor names (wowdev's SMTextureFlags).
        void MtxfBits(Data::Field& field) {
            field.Bit(0x1, "no specular / height (cube map)")
                .Bit(0x10, "scale bit 1 (x2)")
                .Bit(0x20, "scale bit 2 (x4)")
                .Bit(0x40, "scale bit 3 (x16)")
                .Bit(0x80, "scale bit 4 (x256)");
        }

        Data::Schema MakeSchema() {
            Data::Schema s("TerrainTextureParams", "DBFilesClient\\TerrainTextureParams.cdbc");
            s.Description("Per terrain texture: the MTXF flags and MTXP height values later ADTs carry, for 3.3.5 ADTs without them. "
                          "The most specific row for a texture wins: a tile, then a map, then everywhere.");
            s.Id();
            s.String("Scope").Group("Place").Tooltip("Empty: everywhere. A map's folder name (\"Kalimdor\"): that map. An ADT's name "
                                                     "(\"Kalimdor_23_30\"): that tile. Any case.");
            s.Path("Texture").Group("Place").Tooltip("The texture as the ADT names it (any case, / or \\). A name of the _s file matches its base.");
            MtxfBits(s.Flags("MTXF").Group("Flags").Tooltip("The values of the bits MTXFMask picks. Scale bits 4-7 hold n: the texture "
                                                            "spreads over 1 << n times the area (retail's large rock textures)."));
            MtxfBits(s.Flags("MTXFMask").Group("Flags").Tooltip("Which MTXF bits the row sets; the others stay as the ADT has them. "
                                                                "Take all four scale bits to set a scale."));
            s.Float("HeightScale").Group("Height").Default(0).Tooltip("MTXP: the _h texture's height is scaled to [0, this). 0 with offset 1 = off.");
            s.Float("HeightOffset").Group("Height").Default(1).Tooltip("MTXP: added to the scaled height. Blizzard's default 1.");
            s.Text("Comment").Tooltip("Free text for the editor; never read.");
            return s;
        }

        std::string Lower(const char* text) {
            std::string out = text ? text : "";

            for (char& c : out) {
                c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            return out;
        }

        // The texture's key: lower case, '/', without ".blp" and a trailing "_s".
        std::string TextureKey(const char* texture) {
            std::string key = Lower(texture);

            if (key.size() > 4 && key.compare(key.size() - 4, 4, ".blp") == 0) {
                key.resize(key.size() - 4);
            }

            if (key.size() > 2 && key.compare(key.size() - 2, 2, "_s") == 0) {
                key.resize(key.size() - 2);
            }

            return key;
        }

        enum class Kind { Global, Map, Tile };

        struct Entry {
            Kind kind = Kind::Global;
            std::string map;  // Map and Tile
            int32_t x = 0, y = 0;
            uint32_t mtxf = 0, mask = 0;
            float heightScale = 0.0f, heightOffset = 1.0f;
        };

        // "Name_12_34" -> a tile; "Name" -> a map; "" -> everywhere.
        Entry ParseScope(const std::string& scope) {
            Entry e;
            const size_t begin = scope.find_first_not_of(" \t");

            if (begin == std::string::npos) {
                return e;
            }

            const std::string trimmed = Lower(scope.substr(begin, scope.find_last_not_of(" \t") + 1 - begin).c_str());
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

        // The ADT the player stands on, as "<map>_<x>_<y>"; false outside the world or on no tile.
        bool CurrentTile(std::string& out) {
            const WOWGUID guid = ClntObjMgrGetActivePlayer();
            CGPlayer_C* player = guid ? static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(guid, TYPE_PLAYER, __FILE__, __LINE__)) : nullptr;

            if (!player) {
                return false;
            }

            // As CMap finds the area of a point (SXC/World's FindSpot).
            const float half = 32.0f * 533.33333f;
            const C3Vector p = player->GetPosition();
            const float fx = -(p.x - half), fy = -(p.y - half);

            if (!(fx >= 0.0f && fy >= 0.0f && fx < 2.0f * half && fy < 2.0f * half)) {
                return false;
            }

            const int32_t cellX = CMath::fint_(fy * 0.24f - 0.5f), cellY = CMath::fint_(fx * 0.24f - 0.5f);
            const CMapArea* area = CMap::m_areaTable[64 * ((cellY >> 7) & 0x3F) + ((cellX >> 7) & 0x3F)];

            if (!area) {
                return false;
            }

            char name[300];
            std::snprintf(name, sizeof(name), "%s_%d_%d", CMap::mapName, area->m_index.x, area->m_index.y);
            out = name;
            return true;
        }

        Data::Row CloneToCurrentTile(Data::Table& table, Data::Row selected, std::string& note) {
            std::string tile;

            if (!CurrentTile(tile)) {
                note = "not on a loaded tile";
                return Data::Row();
            }

            const int scope = table.GetSchema().IndexOf("Scope"), texture = table.GetSchema().IndexOf("Texture");
            const std::string key = TextureKey(selected.String(texture).c_str());
            const std::string tileKey = Lower(tile.c_str());

            for (uint32_t i = 0; i < table.RowCount(); i++) {
                const Data::Row row = table.At(i);

                if (Lower(row.String(scope).c_str()) == tileKey && TextureKey(row.String(texture).c_str()) == key) {
                    note = "already there (row " + std::to_string(row.Id()) + ")";
                    return row;
                }
            }

            Data::Row row = table.Clone(selected.Index());
            row.SetString(scope, 0, tile.c_str());
            note = "cloned for " + tile;
            return row;
        }

        Data::Table* s_table;
        std::mutex s_mutex;
        uint32_t s_generation = ~0u;
        std::unordered_map<std::string, std::vector<Entry>> s_byTexture;

        // Under s_mutex.
        void Rebuild() {
            if (!s_table || s_table->Generation() == s_generation) {
                return;
            }

            s_generation = s_table->Generation();
            s_byTexture.clear();
            const Data::Schema& schema = s_table->GetSchema();
            const int scope = schema.IndexOf("Scope"), texture = schema.IndexOf("Texture"), mtxf = schema.IndexOf("MTXF"),
                      mask = schema.IndexOf("MTXFMask"), heightScale = schema.IndexOf("HeightScale"), heightOffset = schema.IndexOf("HeightOffset");

            for (uint32_t i = 0; i < s_table->RowCount(); i++) {
                const Data::Row row = s_table->At(i);
                const std::string& name = row.String(texture);

                if (name.empty()) {
                    continue;
                }

                Entry e = ParseScope(row.String(scope));
                e.mtxf = row.Uint(mtxf);
                e.mask = row.Uint(mask);
                e.heightScale = row.Float(heightScale);
                e.heightOffset = row.Float(heightOffset);
                s_byTexture[TextureKey(name.c_str())].push_back(e);
            }
        }
    }

    Settings::Bool& Enabled() {
        static Settings::Bool s_setting("Terrain", "TextureParams", true, "Apply TerrainTextureParams",
                                        "The table's MTXF flags and texture scales over the ADTs' own; off: the ADTs as they are. Scales change at "
                                        "once, flags (no specular) when a tile loads again.");
        return s_setting;
    }

    void Register() {
        Enabled();

        if (!s_table) {
            s_table = Data::Register(MakeSchema());
            Data::AddRowAction("TerrainTextureParams", { "Clone to current tile", "a copy of the selected row for the tile you stand on (its ADT's name as Scope)", CloneToCurrentTile });
        }
    }

    uint32_t Generation() {
        return ((s_table ? s_table->Generation() : 0u) << 1) | (Enabled().Get() ? 1u : 0u);
    }

    TextureParams Resolve(const char* mapName, int32_t tileX, int32_t tileY, const char* texture, uint32_t mtxf) {
        TextureParams out;
        out.mtxf = mtxf;

        if (!texture || !Enabled().Get()) {
            return out;
        }

        std::lock_guard<std::mutex> lock(s_mutex);
        Rebuild();
        const auto found = s_byTexture.find(TextureKey(texture));

        if (found == s_byTexture.end()) {
            return out;
        }

        const std::string map = Lower(mapName);
        const Entry* best = nullptr;

        for (const Entry& e : found->second) {
            const bool applies = e.kind == Kind::Global || (e.map == map && (e.kind == Kind::Map || (e.x == tileX && e.y == tileY)));

            if (applies && (!best || static_cast<int>(e.kind) > static_cast<int>(best->kind))) {
                best = &e;
            }
        }

        if (best) {
            out.mtxf = (mtxf & ~best->mask) | (best->mtxf & best->mask);
            out.heightScale = best->heightScale;
            out.heightOffset = best->heightOffset;
            out.matched = true;
        }

        return out;
    }
}
