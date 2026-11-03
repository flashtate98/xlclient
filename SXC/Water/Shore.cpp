// The shore field (SXC/Water/Shore.h).
#include "SXC/Water/Shore.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CMath.h"
#include "SXC/Profiler/Profiler.h"
#include "SXC/Water/Surface.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include "WoW/Source/WorldClient/CChunkLiquid.h"
#include "WoW/Source/WorldClient/CMapBaseObjLink.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapDoodadDef.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/World.h"

namespace SXC::Water {
    namespace {
        constexpr float kHalfWorld = 32.0f * 533.33333f;
        constexpr int32_t kChunks = kShoreSize / 16;     // chunks per side
        constexpr int32_t kResamplePerFrame = 16;        // changed chunks sampled per frame (a row: the window's step)
        constexpr float kMaxDistance = 64.0f;            // yd: farther shores count as this far
        constexpr float kMaxDepth = 50.0f;               // yd: holes and the deep

        // What a chunk slot was sampled from: when any of it changes, the chunk is sampled again (a chunk
        // object is reused for other places; its liquid may come after it).
        struct Slot {
            int32_t cx = -1, cy = -1;
            const void* chunk = nullptr;
            const void* liquid = nullptr;
            float cornerX = 0.0f, cornerY = 0.0f;
            uint32_t models = 0;   // ModelSignature: models load after their chunk
        };

        struct State {
            ShoreMap map;
            Slot slots[kChunks * kChunks];
            std::vector<uint8_t> water = std::vector<uint8_t>(kShoreSize * kShoreSize, 0);
            std::vector<float> depth = std::vector<float>(kShoreSize * kShoreSize, 0.0f);
            std::vector<float> surface = std::vector<float>(kShoreSize * kShoreSize, 0.0f);
            std::vector<float> ground = std::vector<float>(kShoreSize * kShoreSize, -1e6f);
            std::vector<uint16_t> type = std::vector<uint16_t>(kShoreSize * kShoreSize, 0);
            std::vector<float> coord = std::vector<float>(kShoreSize * kShoreSize, 0.0f);   // the stock depth coordinate
            std::vector<uint8_t> model = std::vector<uint8_t>(kShoreSize * kShoreSize, 0);  // a model stands in the water here
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

        // Lava and slime (the LiquidType's Type column 2 and 3): no wet sand at their shores. Looked up once
        // per type.
        bool NotWater(uint16_t type) {
            static uint8_t s_known[4096] = {};   // 0 not looked up, 1 water, 2 not

            if (type >= 4096) {
                return false;
            }

            if (!s_known[type]) {
                const LiquidTypeRec* rec = g_liquidTypeDB.GetRecord(static_cast<int32_t>(type));
                s_known[type] = rec && rec->m_soundBank >= 2 ? 2 : 1;
            }

            return s_known[type] == 2;
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

        // The depth coordinate the stock mesh gives this point (CChunkLiquid::BuildVertices: the vertices'
        // depth bytes through the type's table, which for the sea is authored, not measured), between the
        // tile's corners as GetHeight reads the heights. The near mesh's tint, so it matches the stock water.
        float DepthCoord(const CChunkLiquid* liquid, const C2iVector& tile, const C2Vector& frac) {
            DepthCoordTable* table = CMap::GetDepthCoordTable(static_cast<uint32_t>(liquid->m_type));

            if (!table || !liquid->m_vertexData) {
                return 0.0f;
            }

            const int32_t cols = liquid->m_tileEnd.y - liquid->m_tileBegin.y + 1;
            const uint32_t index = static_cast<uint32_t>((tile.x - liquid->m_tileBegin.y) + cols * (tile.y - liquid->m_tileBegin.x));
            const uint32_t next = index + static_cast<uint32_t>(cols);
            Liquid::IVertexData* data = liquid->m_vertexData;
            const float d00 = (*table)[data->GetDepth(index)], d01 = (*table)[data->GetDepth(index + 1)];
            const float d10 = (*table)[data->GetDepth(next)], d11 = (*table)[data->GetDepth(next + 1)];
            const float top = d00 + (d01 - d00) * frac.x;
            return top + (d10 + (d11 - d10) * frac.x - top) * frac.y;
        }

        // The models a chunk holds that can block the water (doodads placed and loaded, WMOs with their
        // groups loaded), as a number that changes when one comes or goes: the chunk is sampled again.
        uint32_t ModelSignature(CMapChunk* chunk) {
            if (!chunk) {
                return 0;
            }

            uint32_t sig = 0;

            for (CMapBaseObjLink* link = chunk->m_doodadDefLinkList.Head(); link; link = chunk->m_doodadDefLinkList.Next(link)) {
                const CMapDoodadDef* def = static_cast<const CMapDoodadDef*>(link->m_owner);

                if (def && (static_cast<uint32_t>(def->m_flags) & 0x80) && def->m_model) {
                    sig = sig * 31u + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(def) >> 4);
                }
            }

            for (CMapBaseObjLink* link = chunk->m_mapObjDefLinkList.Head(); link; link = chunk->m_mapObjDefLinkList.Next(link)) {
                const CMapObjDef* def = static_cast<const CMapObjDef*>(link->m_owner);

                if (def && def->m_owner && def->m_owner->m_isGroupLoaded) {
                    sig = sig * 31u + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(def) >> 4) + 1u;
                }
            }

            return sig;
        }

        // Models standing in a chunk's water (Seyri, 2026-10-30: rocks, cliffs, piers and hulls had no
        // shore): the game's own collision, as it places things on the ground (doodads, game objects,
        // WMOs; not the terrain), through the water cells under the models that reach the water's
        // height. Per cell, two rays: down from high above, and up from just under the surface. A
        // model counts where the first thing above the water is within 2.5 yd of it (a low rock, a
        // hull, a low pier) or is the top itself (the ray rose inside a solid: a tall rock); a bridge
        // leaves a gap under it and doesn't count. Open sea has no models: no rays.
        void SampleModels(int32_t cx, int32_t cy, CMapChunk* chunk) {
            if (!chunk) {
                return;
            }

            float low = 1e9f, high = -1e9f;

            for (int32_t j = 0; j < 16; j++) {
                for (int32_t i = 0; i < 16; i++) {
                    const int32_t k = Stored(cy * 16 + j) * kShoreSize + Stored(cx * 16 + i);

                    if (s.water[k] && s.type[k]) {
                        low = s.surface[k] < low ? s.surface[k] : low;
                        high = s.surface[k] > high ? s.surface[k] : high;
                    }
                }
            }

            if (low > high) {
                return;
            }

            struct Area {
                float x0, y0, x1, y1;
            };
            Area areas[64];
            uint32_t count = 0;
            auto consider = [&](const CAaBox& box) {
                if (count < 64 && box.b.z < high + 0.5f && box.t.z > low - 0.5f) {
                    areas[count++] = { box.b.x - kShoreCell, box.b.y - kShoreCell, box.t.x + kShoreCell, box.t.y + kShoreCell };
                }
            };

            for (CMapBaseObjLink* link = chunk->m_doodadDefLinkList.Head(); link; link = chunk->m_doodadDefLinkList.Next(link)) {
                const CMapDoodadDef* def = static_cast<const CMapDoodadDef*>(link->m_owner);

                if (def && !(static_cast<uint32_t>(def->m_flags) & 0x100) && (static_cast<uint32_t>(def->m_flags) & 0x80) && def->m_model) {
                    consider(def->m_bbox);
                }
            }

            for (CMapBaseObjLink* link = chunk->m_mapObjDefLinkList.Head(); link; link = chunk->m_mapObjDefLinkList.Next(link)) {
                const CMapObjDef* def = static_cast<const CMapObjDef*>(link->m_owner);

                if (def && def->m_owner && def->m_owner->m_isGroupLoaded) {
                    consider(def->m_bbox);
                }
            }

            constexpr uint32_t kFlags = 0x100011;   // doodads, game objects, WMOs: the ground mask without the terrain
            constexpr float kAbove = 30.0f, kGap = 2.5f;

            for (int32_t j = 0; j < 16 && count; j++) {
                for (int32_t i = 0; i < 16; i++) {
                    const int32_t gx = cx * 16 + i, gy = cy * 16 + j;
                    const int32_t k = Stored(gy) * kShoreSize + Stored(gx);

                    if (!s.water[k] || !s.type[k]) {
                        continue;
                    }

                    const float px = (static_cast<float>(gx) + 0.5f) * 0.5f, py = (static_cast<float>(gy) + 0.5f) * 0.5f;
                    const float x = kHalfWorld - py / 0.24f, y = kHalfWorld - px / 0.24f;
                    bool under = false;

                    for (uint32_t a = 0; a < count && !under; a++) {
                        under = x >= areas[a].x0 && x <= areas[a].x1 && y >= areas[a].y0 && y <= areas[a].y1;
                    }

                    if (!under) {
                        continue;
                    }

                    const float surface = s.surface[k];
                    C3Vector top(x, y, surface + kAbove), bottom(x, y, surface - 0.5f), hitDown, hitUp;
                    float t = 1.0f;

                    if (!::World::Intersect(top, bottom, &hitDown, &t, kFlags, nullptr)) {
                        continue;
                    }

                    float u = 1.0f;
                    const bool roof = ::World::Intersect(bottom, top, &hitUp, &u, kFlags, nullptr) != 0;
                    const float firstAbove = roof ? hitUp.z : hitDown.z;

                    if (firstAbove - surface < kGap || std::fabs(firstAbove - hitDown.z) < 0.05f) {
                        s.model[k] = 1;
                    }
                }
            }
        }

        // A chunk's 16 x 16 cells: water where its liquid stands above the ground (a hole under the
        // surface counts as deep). A chunk not loaded (yet, or no tile there: the open sea past a map's
        // tiles) counts as open water, which the near mesh doesn't cover: as land, it drew a shore with its
        // foam where the window met it (Seyri, 2026-10-30).
        void SampleChunk(int32_t cx, int32_t cy, CMapChunk* chunk) {
            for (int32_t j = 0; j < 16; j++) {
                for (int32_t i = 0; i < 16; i++) {
                    const int32_t gx = cx * 16 + i, gy = cy * 16 + j;
                    const int32_t k = Stored(gy) * kShoreSize + Stored(gx);
                    s.water[k] = 0;
                    s.depth[k] = 0.0f;
                    s.surface[k] = 0.0f;
                    s.ground[k] = -1e6f;
                    s.type[k] = 0;
                    s.coord[k] = 0.0f;
                    s.model[k] = 0;

                    if (!chunk) {
                        s.water[k] = 1;
                        s.depth[k] = kMaxDepth;
                        continue;
                    }

                    // The client's sub-chunk coordinates (0.24 per yd), as CMap::QueryHeight passes them.
                    const float px = (static_cast<float>(gx) + 0.5f) * 0.5f, py = (static_cast<float>(gy) + 0.5f) * 0.5f;
                    C3Vector world(kHalfWorld - py / 0.24f, kHalfWorld - px / 0.24f, 0.0f);
                    float ground = 0.0f;
                    const bool solid = CMap::GetHeightTerrain(chunk, world, CMath::fint_(px - 0.5f), CMath::fint_(py - 0.5f), &ground) && std::isfinite(ground);
                    s.ground[k] = solid ? ground : -1e6f;

                    // The first liquid layer here, as CMapChunk::GetLiquidHeight finds it, with its type.
                    const int32_t lx = CMath::fint_(px - 0.5f), ly = CMath::fint_(py - 0.5f);
                    C2iVector tile;
                    tile.x = lx & 7;
                    tile.y = ly & 7;
                    const C2Vector frac(px - static_cast<float>(lx), py - static_cast<float>(ly));
                    float surface = 0.0f;
                    uint32_t liquidType = 0;

                    for (CChunkLiquid* liquid = chunk->m_liquidChunkLinkList.Head(); liquid; liquid = chunk->m_liquidChunkLinkList.Next(liquid)) {
                        if (liquid->TileExists(tile.x, tile.y) && liquid->GetHeight(frac, tile, surface) && std::isfinite(surface)) {
                            liquidType = static_cast<uint32_t>(liquid->m_type);
                            s.coord[k] = DepthCoord(liquid, tile, frac);
                            break;
                        }
                    }

                    if (!liquidType) {
                        continue;
                    }

                    const float d = solid ? surface - ground : kMaxDepth;
                    s.surface[k] = surface;
                    s.type[k] = static_cast<uint16_t>(liquidType);

                    if (d > 0.02f) {
                        s.water[k] = 1;
                        s.depth[k] = d < kMaxDepth ? d : kMaxDepth;
                    }
                }
            }

            SampleModels(cx, cy, chunk);
        }

        // Each window cell's nearest cell of the other kind (water to land, land to water), as an offset, by
        // two sweeps (8SSEDT); then the texels.
        void Solve() {
            constexpr int16_t kFar = 30000;
            const int32_t n = kShoreSize;
            const int32_t ox = s.chunkX * 16, oy = s.chunkY * 16;
            std::vector<int16_t> offX(n * n), offY(n * n);
            auto at = [&](int32_t wi, int32_t wj) { return Stored(oy + wj) * n + Stored(ox + wi); };

            // Water, less the models standing in it; but not specks of a cell or two (a post, a buoy):
            // a model cell counts with at least three solid neighbours, so small things foam (the
            // pixel's own depth does that) without starting rings of shore waves.
            std::vector<uint8_t> wet(n * n, 0);

            for (int32_t wj = 0; wj < n; wj++) {
                for (int32_t wi = 0; wi < n; wi++) {
                    const int32_t k = at(wi, wj);
                    wet[wj * n + wi] = s.water[k];

                    if (!s.water[k] || !s.model[k]) {
                        continue;
                    }

                    int32_t solid = 0;

                    for (int32_t dj = -1; dj <= 1; dj++) {
                        for (int32_t di = -1; di <= 1; di++) {
                            const int32_t ni = wi + di, nj = wj + dj;

                            if ((di || dj) && ni >= 0 && nj >= 0 && ni < n && nj < n) {
                                const int32_t nk = at(ni, nj);
                                solid += !s.water[nk] || s.model[nk];
                            }
                        }
                    }

                    if (solid >= 3) {
                        wet[wj * n + wi] = 0;
                    }
                }
            }

            auto isWater = [&](int32_t wi, int32_t wj) { return wet[wj * n + wi] != 0; };

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

            // The near mesh's cells: water (under the models standing in it too: the shore field takes them as
            // land, for their foam and waves, but left out of the mesh the stock water filled them, still by its
            // easing toward a shore, and the foam ring around a rock rode apart from the sea: Seyri,
            // 2026-11-02), and land within reach of its nearest water (that water's surface and type), then not
            // where two levels meet.
            constexpr int32_t kReachAbove = 3;    // cells onto land above the surface
            constexpr int32_t kReachBelow = 12;   // cells onto land below it (water a map cut short)
            // yd: land higher than this above the surface: never. Cliffs too: stopped 2 yd up, the mesh ended
            // a cell short of a cliff in whole-cell steps, and the deep water there lifted that edge out of the
            // sea (Seyri, 2026-10-31); carried under the rock, the cliff's face cuts the waves.
            constexpr float kAbove = 60.0f;
            constexpr float kBelow = 1.5f;        // yd: land lower than this under it: never (the valley below a
                                                  // raised lake or a waterfall's lip, Seyri 2026-10-31)
            constexpr float kStep = 0.4f;         // yd: neighbours further apart are two levels
            // yd: but two cells both under the liquid are one sloped surface up to this (a river running down;
            // held to 0.4 every steeper cell went to the stock mesh, in patches of other waves: Seyri
            // 2026-11-03). Steeper (about 45 degrees per cell) stays two levels.
            constexpr float kSlope = 2.0f;
            std::vector<float> surfaceOf(n * n, 0.0f);
            std::vector<uint16_t> typeOf(n * n, 0);
            std::vector<uint8_t> covered(n * n, 0);
            // Cells whose surface is the liquid's own there (water, or land the liquid's tile runs over), not
            // borrowed from the nearest water: on a slope the borrowed one is off by the slope over the reach,
            // so the waterline broke into steps beside the shore and those cells fell out of the mesh, still
            // and in other waves (Seyri, 2026-11-03: a river where the ground rises through the surface).
            std::vector<uint8_t> real(n * n, 0);

            for (int32_t wj = 0; wj < n; wj++) {
                for (int32_t wi = 0; wi < n; wi++) {
                    const int32_t w = wj * n + wi, k = at(wi, wj);

                    if (s.water[k]) {
                        surfaceOf[w] = s.surface[k];
                        typeOf[w] = s.type[k];
                        covered[w] = s.type[k] != 0 ? 1 : 0;   // not a chunk not loaded
                        real[w] = covered[w];
                        continue;
                    }

                    if (offX[w] == kFar) {
                        continue;
                    }

                    const int32_t ni = wi + offX[w], nj = wj + offY[w];

                    if (ni < 0 || nj < 0 || ni >= n || nj >= n) {
                        continue;
                    }

                    const int32_t nk = at(ni, nj);
                    const int32_t reach = std::abs(offX[w]) > std::abs(offY[w]) ? std::abs(offX[w]) : std::abs(offY[w]);
                    real[w] = s.type[k] != 0 && s.type[k] == s.type[nk] ? 1 : 0;
                    const float surface = real[w] ? s.surface[k] : s.surface[nk];
                    const float above = s.ground[k] - surface;
                    surfaceOf[w] = surface;
                    typeOf[w] = s.type[nk];
                    covered[w] = s.type[nk] != 0 && ((above < 0.0f && above > -kBelow && reach <= kReachBelow) || (above >= 0.0f && above < kAbove && reach <= kReachAbove)) ? 1 : 0;
                }
            }

            // Land only where it joins its water through covered cells: past a ridge, a lake's surface isn't
            // the dip behind it.
            {
                std::vector<int32_t> queue;
                std::vector<uint8_t> joined(n * n, 0);
                queue.reserve(n * n);

                for (int32_t w = 0; w < n * n; w++) {
                    if (covered[w] && wet[w]) {
                        joined[w] = 1;
                        queue.push_back(w);
                    }
                }

                for (size_t q = 0; q < queue.size(); q++) {
                    const int32_t w = queue[q], wi = w % n, wj = w / n;
                    static const int32_t kNext[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

                    for (const auto& d : kNext) {
                        const int32_t ni = wi + d[0], nj = wj + d[1];

                        if (ni < 0 || nj < 0 || ni >= n || nj >= n) {
                            continue;
                        }

                        const int32_t nw = nj * n + ni;

                        // Land under the surface only from water or other such land: not over a rise the mesh
                        // runs beneath into the hollow behind it (Seyri, 2026-10-31: a path behind a lake's rim).
                        const auto under = [&](int32_t c) {
                            return !wet[c] && s.ground[at(c % n, c / n)] < surfaceOf[c];
                        };
                        const bool from = wet[w] || under(w);

                        const float step = real[w] && real[nw] ? kSlope : kStep;

                        if (covered[nw] && !joined[nw] && std::fabs(surfaceOf[nw] - surfaceOf[w]) <= step && (from || !under(nw))) {
                            joined[nw] = 1;
                            queue.push_back(nw);
                        }
                    }
                }

                for (int32_t w = 0; w < n * n; w++) {
                    covered[w] &= joined[w];
                }
            }

            std::vector<uint8_t> own(n * n, 0);
            s.map.typeCount = 0;

            for (int32_t wj = 0; wj < n; wj++) {
                for (int32_t wi = 0; wi < n; wi++) {
                    const int32_t w = wj * n + wi;

                    if (!covered[w]) {
                        continue;
                    }

                    bool level = true;
                    static const int32_t kSides[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

                    for (const auto& d : kSides) {
                        const int32_t ni = wi + d[0], nj = wj + d[1];

                        if (ni < 0 || nj < 0 || ni >= n || nj >= n || !covered[nj * n + ni]) {
                            continue;
                        }

                        const bool realHere = real[w] != 0, realThere = real[nj * n + ni] != 0;

                        // The liquid's own surface against land carrying a borrowed height: the land leaves the
                        // mesh (its own test), not the water.
                        if (realHere && !realThere && typeOf[nj * n + ni] == typeOf[w]) {
                            continue;
                        }

                        const float step = realHere && realThere ? kSlope : kStep;

                        if (std::fabs(surfaceOf[nj * n + ni] - surfaceOf[w]) > step || typeOf[nj * n + ni] != typeOf[w]) {
                            level = false;
                        }
                    }

                    if (!level) {
                        continue;
                    }

                    own[w] = 1;
                    bool known = false;

                    for (uint32_t t = 0; t < s.map.typeCount; t++) {
                        known |= s.map.types[t] == typeOf[w];
                    }

                    if (!known && s.map.typeCount < ShoreMap::kMaxTypes) {
                        s.map.types[s.map.typeCount++] = typeOf[w];
                    } else if (!known) {
                        own[w] = 0;   // more waters than the mesh draws: the stock mesh keeps this one
                    }
                }
            }

            for (int32_t wj = 0; wj < n; wj++) {
                for (int32_t wi = 0; wi < n; wi++) {
                    const int32_t w = wj * n + wi, k = at(wi, wj);
                    float* c = &s.map.cells[static_cast<size_t>(k) * 4];
                    c[0] = surfaceOf[w];
                    c[1] = s.ground[k] > -1e5f ? s.ground[k] : surfaceOf[w] - kMaxDepth;
                    c[2] = static_cast<float>(typeOf[w]) + (NotWater(typeOf[w]) ? 0.25f : 0.0f);   // + 0.25: lava, slime
                    // Covered: 1 plus the stock depth coordinate (land the mesh reaches over: 0, the waterline's).
                    c[3] = own[w] ? 1.0f + (s.water[k] ? s.coord[k] : 0.0f) : 0.0f;
                }
            }

            for (int32_t wj = 0; wj < n; wj++) {
                for (int32_t wi = 0; wi < n; wi++) {
                    const int32_t w = wj * n + wi, k = at(wi, wj);
                    const bool water = wet[w] != 0;
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
                    t[1] = Half(water ? s.depth[k] : 0.0f);
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
        bool pending = false;

        for (int32_t j = 0; j < kChunks && !pending; j++) {
            for (int32_t i = 0; i < kChunks && !pending; i++) {
                const int32_t cx = firstX + i, cy = firstY + j;
                Slot& slot = s.slots[(cy & (kChunks - 1)) * kChunks + (cx & (kChunks - 1))];
                CMapChunk* chunk = ChunkAt(cx, cy);
                const void* liquid = chunk ? chunk->m_liquidChunkLinkList.Head() : nullptr;
                const float cornerX = chunk ? chunk->m_topLeftCoords.x : 0.0f, cornerY = chunk ? chunk->m_topLeftCoords.y : 0.0f;
                const uint32_t models = liquid ? ModelSignature(chunk) : 0;   // only water can be blocked

                if (slot.cx == cx && slot.cy == cy && slot.chunk == chunk && slot.liquid == liquid && slot.cornerX == cornerX && slot.cornerY == cornerY && slot.models == models) {
                    continue;
                }

                if (sampled == kResamplePerFrame) {
                    pending = true;
                    break;
                }

                SampleChunk(cx, cy, chunk);
                slot = Slot{ cx, cy, chunk, liquid, cornerX, cornerY, models };
                s.dirty = true;
                sampled++;
            }
        }

        const auto now = std::chrono::steady_clock::now();

        // Not while chunks wait: after the window moves, their cells still hold the window's far side (the
        // cells are stored toroidally) - the coast behind the player, solved as a shore ahead.
        if (s.dirty && !pending && (now - s.solved > std::chrono::milliseconds(200) || !s.map.valid)) {
            s.solved = now;
            s.dirty = false;
            Solve();
        }

        return s.map;
    }
}
