// The shore field (SXC/Water/Shore.h).
#include "SXC/Water/Shore.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CMath.h"
#include "SXC/Profiler/Profiler.h"
#include "SXC/Water/Surface.h"
#include "WoW/Source/WorldClient/CChunkLiquid.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"

namespace SXC::Water {
    namespace {
        constexpr float kHalfWorld = 32.0f * 533.33333f;
        constexpr int32_t kChunks = kShoreSize / 16;     // chunks per side
        constexpr int32_t kResamplePerFrame = 8;         // changed chunks sampled per frame
        constexpr float kMaxDistance = 64.0f;            // yd: farther shores count as this far
        constexpr float kMaxDepth = 50.0f;               // yd: holes and the deep

        // What a chunk slot was sampled from: when any of it changes, the chunk is sampled again (a chunk
        // object is reused for other places; its liquid may come after it).
        struct Slot {
            int32_t cx = -1, cy = -1;
            const void* chunk = nullptr;
            const void* liquid = nullptr;
            float cornerX = 0.0f, cornerY = 0.0f;
        };

        struct State {
            ShoreMap map;
            Slot slots[kChunks * kChunks];
            std::vector<uint8_t> water = std::vector<uint8_t>(kShoreSize * kShoreSize, 0);
            std::vector<float> depth = std::vector<float>(kShoreSize * kShoreSize, 0.0f);
            int32_t chunkX = -1, chunkY = -1;   // the window's first chunk
            bool dirty = false;
            std::chrono::steady_clock::time_point solved;
        };
        State s;

        uint16_t Half(float f) {
            uint32_t x;
            std::memcpy(&x, &f, 4);
            const uint32_t sign = (x >> 16) & 0x8000u;
            const int32_t exponent = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
            uint32_t mantissa = x & 0x7FFFFFu;

            if (exponent <= 0) {
                return static_cast<uint16_t>(sign);   // tiny: zero
            }

            if (exponent >= 31) {
                return static_cast<uint16_t>(sign | 0x7BFFu);   // huge: the largest
            }

            mantissa += 0x1000u;   // round

            if (mantissa & 0x800000u) {
                mantissa = 0;
                if (exponent + 1 >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
                return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent + 1) << 10));
            }

            return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
        }

        int32_t Stored(int32_t g) {
            return g & (kShoreSize - 1);
        }

        CMapChunk* ChunkAt(int32_t cx, int32_t cy, CMapArea** outTile = nullptr) {
            if (cx < 0 || cy < 0 || cx >= 1024 || cy >= 1024) {
                return nullptr;
            }

            CMapArea* tile = CMap::m_areaTable[64 * (cy >> 4) + (cx >> 4)];

            if (!tile || tile->m_asyncObject) {
                return nullptr;
            }

            if (outTile) *outTile = tile;
            CMapChunk* chunk = tile->m_mapChunks[(cx & 0xF) + 16 * (cy & 0xF)];
            return chunk && chunk->m_header && chunk->m_height ? chunk : nullptr;
        }

        // A chunk's 16 x 16 cells: water where its liquid stands above the ground (a hole under the
        // surface counts as deep).
        void SampleChunk(int32_t cx, int32_t cy, CMapChunk* chunk) {
            for (int32_t j = 0; j < 16; j++) {
                for (int32_t i = 0; i < 16; i++) {
                    const int32_t gx = cx * 16 + i, gy = cy * 16 + j;
                    const int32_t k = Stored(gy) * kShoreSize + Stored(gx);
                    s.water[k] = 0;
                    s.depth[k] = 0.0f;

                    if (!chunk) {
                        continue;
                    }

                    // The client's sub-chunk coordinates (0.24 per yd), as CMap::QueryHeight passes them.
                    const float px = (static_cast<float>(gx) + 0.5f) * 0.5f, py = (static_cast<float>(gy) + 0.5f) * 0.5f;
                    float surface = 0.0f;

                    if (!chunk->m_liquidChunkLinkList.Head() || !chunk->GetLiquidHeight(C2Vector(px, py), &surface) || !std::isfinite(surface)) {
                        continue;
                    }

                    C3Vector world(kHalfWorld - py / 0.24f, kHalfWorld - px / 0.24f, 0.0f);
                    float ground = 0.0f;
                    const bool solid = CMap::GetHeightTerrain(chunk, world, CMath::fint_(px - 0.5f), CMath::fint_(py - 0.5f), &ground) && std::isfinite(ground);
                    const float d = solid ? surface - ground : kMaxDepth;

                    if (d > 0.02f) {
                        s.water[k] = 1;
                        s.depth[k] = d < kMaxDepth ? d : kMaxDepth;
                    }
                }
            }
        }

        // Each window cell's nearest cell of the other kind (water to land, land to water), as an offset, by
        // two sweeps (8SSEDT); then the texels.
        void Solve() {
            constexpr int16_t kFar = 30000;
            const int32_t n = kShoreSize;
            const int32_t ox = s.chunkX * 16, oy = s.chunkY * 16;
            std::vector<int16_t> offX(n * n), offY(n * n);
            auto at = [&](int32_t wi, int32_t wj) { return Stored(oy + wj) * n + Stored(ox + wi); };
            auto isWater = [&](int32_t wi, int32_t wj) { return s.water[at(wi, wj)] != 0; };

            // A cell next to the other kind sits a cell from it; the rest far.
            for (int32_t wj = 0; wj < n; wj++) {
                for (int32_t wi = 0; wi < n; wi++) {
                    const int32_t w = wj * n + wi;
                    offX[w] = kFar;
                    offY[w] = kFar;
                }
            }

            auto test = [&](int32_t wi, int32_t wj, int32_t di, int32_t dj) {
                const int32_t ni = wi + di, nj = wj + dj;

                if (ni < 0 || nj < 0 || ni >= n || nj >= n) {
                    return;
                }

                const int32_t w = wj * n + wi, nw = nj * n + ni;
                int32_t cx, cy;

                if (isWater(ni, nj) != isWater(wi, wj)) {
                    cx = di;
                    cy = dj;
                } else if (offX[nw] != kFar) {
                    cx = offX[nw] + di;
                    cy = offY[nw] + dj;
                } else {
                    return;
                }

                const int32_t here = offX[w] == kFar ? 0x7FFFFFFF : offX[w] * offX[w] + offY[w] * offY[w];

                if (cx * cx + cy * cy < here) {
                    offX[w] = static_cast<int16_t>(cx);
                    offY[w] = static_cast<int16_t>(cy);
                }
            };

            for (int32_t wj = 0; wj < n; wj++) {
                for (int32_t wi = 0; wi < n; wi++) {
                    test(wi, wj, -1, 0);
                    test(wi, wj, 0, -1);
                    test(wi, wj, -1, -1);
                    test(wi, wj, 1, -1);
                }

                for (int32_t wi = n - 1; wi >= 0; wi--) {
                    test(wi, wj, 1, 0);
                }
            }

            for (int32_t wj = n - 1; wj >= 0; wj--) {
                for (int32_t wi = n - 1; wi >= 0; wi--) {
                    test(wi, wj, 1, 0);
                    test(wi, wj, 0, 1);
                    test(wi, wj, 1, 1);
                    test(wi, wj, -1, 1);
                }

                for (int32_t wi = 0; wi < n; wi++) {
                    test(wi, wj, -1, 0);
                }
            }

            for (int32_t wj = 0; wj < n; wj++) {
                for (int32_t wi = 0; wi < n; wi++) {
                    const int32_t w = wj * n + wi, k = at(wi, wj);
                    const bool water = s.water[k] != 0;
                    float distance = kMaxDistance, dirX = 0.0f, dirY = 0.0f;

                    if (offX[w] != kFar) {
                        // The shore lies halfway to the other kind's cell.
                        const float gx = static_cast<float>(offX[w]), gy = static_cast<float>(offY[w]);
                        const float cells = std::sqrt(gx * gx + gy * gy);
                        distance = (cells - 0.5f) * kShoreCell;
                        distance = distance < kMaxDistance ? distance : kMaxDistance;
                        // Cell x runs along world -y, cell y along world -x.
                        dirX = -gy / cells;
                        dirY = -gx / cells;
                    }

                    uint16_t* t = &s.map.texels[static_cast<size_t>(k) * 4];
                    t[0] = Half(water ? distance : -distance);
                    t[1] = Half(s.depth[k]);
                    t[2] = Half(dirX);
                    t[3] = Half(dirY);
                }
            }

            s.map.originX = ox;
            s.map.originY = oy;
            s.map.valid = true;
            s.map.version++;
        }
    }

    const ShoreMap& ShoreField() {
        // Asked at every water draw: brought up to date once per frame (within a few ms).
        static auto s_updated = std::chrono::steady_clock::time_point();
        const auto entered = std::chrono::steady_clock::now();

        if (entered - s_updated < std::chrono::milliseconds(4)) {
            return s.map;
        }

        s_updated = entered;
        static const int s_category = Profiler::Register("water: shore field", Profiler::kCpuOnly);
        Profiler::Scope scope(s_category);
        float x = 0.0f, y = 0.0f;

        if (!PlayerPosition(x, y)) {
            s.map.valid = false;
            return s.map;
        }

        const float fx = (kHalfWorld - y) / (533.33333f / 16.0f), fy = (kHalfWorld - x) / (533.33333f / 16.0f);

        if (!(fx >= 0.0f && fy >= 0.0f && fx < 1024.0f && fy < 1024.0f)) {
            s.map.valid = false;
            return s.map;
        }

        // The window: 16 x 16 chunks with the player's in the middle.
        const int32_t firstX = static_cast<int32_t>(fx) - kChunks / 2, firstY = static_cast<int32_t>(fy) - kChunks / 2;

        if (firstX != s.chunkX || firstY != s.chunkY) {
            s.chunkX = firstX;
            s.chunkY = firstY;
            s.dirty = true;
        }

        // The chunks whose source changed, a few per frame.
        int32_t sampled = 0;

        for (int32_t j = 0; j < kChunks && sampled < kResamplePerFrame; j++) {
            for (int32_t i = 0; i < kChunks && sampled < kResamplePerFrame; i++) {
                const int32_t cx = firstX + i, cy = firstY + j;
                Slot& slot = s.slots[(cy & (kChunks - 1)) * kChunks + (cx & (kChunks - 1))];
                CMapChunk* chunk = ChunkAt(cx, cy);
                const void* liquid = chunk ? chunk->m_liquidChunkLinkList.Head() : nullptr;
                const float cornerX = chunk ? chunk->m_topLeftCoords.x : 0.0f, cornerY = chunk ? chunk->m_topLeftCoords.y : 0.0f;

                if (slot.cx == cx && slot.cy == cy && slot.chunk == chunk && slot.liquid == liquid && slot.cornerX == cornerX && slot.cornerY == cornerY) {
                    continue;
                }

                SampleChunk(cx, cy, chunk);
                slot = Slot{ cx, cy, chunk, liquid, cornerX, cornerY };
                s.dirty = true;
                sampled++;
            }
        }

        const auto now = std::chrono::steady_clock::now();

        if (s.dirty && (now - s.solved > std::chrono::milliseconds(200) || !s.map.valid)) {
            s.solved = now;
            s.dirty = false;
            Solve();
        }

        return s.map;
    }
}
