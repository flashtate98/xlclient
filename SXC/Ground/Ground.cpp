#include "SXC/Ground/Ground.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "SXC/Log.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    constexpr float kTile = 533.33333f;          // an ADT tile, yd
    constexpr float kChunk = kTile / 16.0f;      // a map chunk
    constexpr float kHalf = 32.0f * kTile;       // the map's corner from its middle

    struct Known {
        SXC::Ground::Note note;
        int32_t cells[2][2] = {};   // (i, j) of its map chunks
        uint32_t colours[2][SXC::Ground::kColours] = {};   // their vertex colours, copied
        float heights[2][SXC::Ground::kColours] = {};      // their vertex heights (world z), copied
        uint32_t normals[2][SXC::Ground::kColours] = {};   // their vertex normals, packed (Frame::normals)
        uint32_t holes[2] = {};
    };

    std::unordered_map<const CMapRenderChunk*, Known> s_known;
    float s_light[16] = {};   // the terrain's light at its last draw (Frame::light)
    char s_status[160] = "no terrain noted yet";

    int32_t Cell(float world) {
        return static_cast<int32_t>(std::floor((kHalf - world) / kChunk));
    }
}

namespace SXC::Ground {
    void NoteChunk(const CMapRenderChunk* chunk, const Note& note) {
        Known& k = s_known[chunk];
        k.note = note;

        for (uint32_t c = 0; c < note.centreCount && c < 2; c++) {
            k.cells[c][0] = Cell(note.centres[c][0]);
            k.cells[c][1] = Cell(note.centres[c][1]);

            for (uint32_t v = 0; v < kColours; v++) {
                k.colours[c][v] = note.shading[c] ? note.shading[c][v] : 0xFF7F7F7Fu;
                // Without heights the chunk counts as all holes for the proximity (far below anything).
                k.heights[c][v] = note.heights[c] ? note.baseZ[c] + note.heights[c][v] : -1.0e9f;
                // Up where there are none.
                const int8_t* n = note.normals[c] ? note.normals[c] + 3 * v : nullptr;
                k.normals[c][v] = n ? static_cast<uint8_t>(n[0]) | (static_cast<uint32_t>(static_cast<uint8_t>(n[1])) << 8) |
                                          (static_cast<uint32_t>(static_cast<uint8_t>(n[2])) << 16)
                                    : 127u << 16;
            }

            k.holes[c] = note.heights[c] ? note.holes[c] : 0xFFFFu;
        }

        k.note.shading[0] = k.note.shading[1] = nullptr;   // not kept: the chunk owns them
        k.note.heights[0] = k.note.heights[1] = nullptr;
        k.note.normals[0] = k.note.normals[1] = nullptr;

        // The light, its direction turned into the world with this frame's view (the WMOs drawn next frame turn it
        // back with theirs: no lag when the camera turns).
        if (const CM2Scene* scene = World::s_m2Scene) {
            const C44Matrix& vi = scene->m_viewInv;
            const float* d = note.sunDir;
            s_light[0] = vi.a0 * d[0] + vi.b0 * d[1] + vi.c0 * d[2];
            s_light[1] = vi.a1 * d[0] + vi.b1 * d[1] + vi.c1 * d[2];
            s_light[2] = vi.a2 * d[0] + vi.b2 * d[1] + vi.c2 * d[2];
        }

        for (int i = 0; i < 3; i++) {
            s_light[4 + i] = note.ambient[i];
            s_light[8 + i] = note.diffuse[i];
        }

        for (int i = 0; i < 4; i++) {
            s_light[12 + i] = note.specular[i];
        }
    }

    void ForgetChunk(const CMapRenderChunk* chunk) {
        s_known.erase(chunk);
    }

    bool Build(Frame& out) {
        const CM2Scene* scene = World::s_m2Scene;

        if (!scene) {
            std::snprintf(s_status, sizeof s_status, "no world scene");
            return false;
        }

        // The view back to the world (row vectors: world = view x the view's inverse), a row per world axis; the
        // camera is its translation.
        const C44Matrix& vi = scene->m_viewInv;
        const float rows[12] = { vi.a0, vi.b0, vi.c0, vi.d0, vi.a1, vi.b1, vi.c1, vi.d1, vi.a2, vi.b2, vi.c2, vi.d2 };

        for (int i = 0; i < 12; i++) {
            out.viewInv[i] = rows[i];
        }

        for (int i = 0; i < 16; i++) {
            out.light[i] = s_light[i];
        }

        out.firstI = Cell(vi.d0) - static_cast<int32_t>(kGrid / 2);
        out.firstJ = Cell(vi.d1) - static_cast<int32_t>(kGrid / 2);

        for (uint32_t& g : out.grid) {
            g = 0;
        }

        out.entryCount = 0;
        out.textureCount = 0;
        static std::unordered_map<const CGxTex*, uint32_t> slots;   // texture -> slot + 1 (kept for its buckets)
        slots.clear();
        uint32_t full = 0;

        auto slotOf = [&](CGxTex* gx, bool clamp) -> uint32_t {
            if (!gx) {
                return 0;
            }

            auto found = slots.find(gx);

            if (found != slots.end()) {
                return found->second;
            }

            if (out.textureCount >= kMaxTextures) {
                full++;
                return 0;
            }

            out.textures[out.textureCount] = Texture{ gx, clamp };
            const uint32_t slot = ++out.textureCount;
            slots.emplace(gx, slot);
            return slot;
        };

        for (const auto& known : s_known) {
            const Known& k = known.second;
            const Note& n = k.note;
            bool inside = false;

            for (uint32_t c = 0; c < n.centreCount && c < 2; c++) {
                const int32_t i = k.cells[c][0] - out.firstI, j = k.cells[c][1] - out.firstJ;
                inside = inside || (i >= 0 && j >= 0 && i < static_cast<int32_t>(kGrid) && j < static_cast<int32_t>(kGrid));
            }

            if (!inside) {
                continue;
            }

            if (out.entryCount >= kMaxEntries) {
                full++;
                break;
            }

            Entry& e = out.entries[out.entryCount];
            e = Entry();
            e.place[0] = n.origin[0];
            e.place[1] = n.origin[1];
            e.place[2] = n.alphaScale[0];
            e.place[3] = n.alphaScale[1];

            for (int l = 0; l < 4; l++) {
                e.layerScale[l] = n.layerScale[l];
                e.heightScale[l] = n.heightScale[l];
                e.heightOffset[l] = n.heightOffset[l];
                e.layerTex[l] = slotOf(n.layer[l], false);
                e.heightTex[l] = slotOf(n.height[l], false);
            }

            e.alphaTex = slotOf(n.alpha, true);
            e.layers = n.layers;
            e.flags = n.flags;
            e.coverPainted = n.coverPainted;
            e.coverLooks = n.coverLooks;
            e.liquid = n.liquid;
            out.entryCount++;

            for (uint32_t c = 0; c < n.centreCount && c < 2; c++) {
                const int32_t i = k.cells[c][0] - out.firstI, j = k.cells[c][1] - out.firstJ;

                if (i >= 0 && j >= 0 && i < static_cast<int32_t>(kGrid) && j < static_cast<int32_t>(kGrid)) {
                    out.grid[j * kGrid + i] = out.entryCount;
                    std::memcpy(&out.colours[(j * kGrid + i) * kColours], k.colours[c], sizeof k.colours[c]);
                    std::memcpy(&out.heights[(j * kGrid + i) * kColours], k.heights[c], sizeof k.heights[c]);
                    std::memcpy(&out.normals[(j * kGrid + i) * kColours], k.normals[c], sizeof k.normals[c]);
                    out.holes[j * kGrid + i] = k.holes[c];
                }
            }
        }

        uint32_t noAlpha = 0, byLayers[5] = {};

        for (uint32_t e = 0; e < out.entryCount; e++) {
            noAlpha += out.entries[e].alphaTex == 0 ? 1u : 0u;
            byLayers[out.entries[e].layers < 4 ? out.entries[e].layers : 4]++;
        }

        std::snprintf(s_status, sizeof s_status, "%u chunks known, %u in the window (%u without an alpha map; 1/2/3/4 layers: %u/%u/%u/%u), %u textures%s",
                      static_cast<uint32_t>(s_known.size()), out.entryCount, noAlpha, byLayers[1], byLayers[2], byLayers[3], byLayers[4], out.textureCount,
                      full ? " (the table is full: some left out)" : "");

        if (out.entryCount) {
            SXC::LogOnce("ground-first", "terrain table: %s", s_status);
        }

        return true;
    }

    const char* Status() {
        return s_status;
    }
}
