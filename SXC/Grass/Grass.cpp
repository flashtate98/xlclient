// The instanced grass (SXC/Grass/Grass.h).

#include "SXC/Grass/Grass.h"

#include <cmath>
#include <cstring>
#include <string>
#include "SXC/Api/sxc_grass.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "SXC/Cover/Cover.h"
#include "SXC/Log.h"
#include "SXC/Settings/Settings.h"
#include "SXC/World/World.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/GroundEffectTextureRec.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/CDetailDoodadData.h"
#include "WoW/Source/WorldClient/DetailDoodad.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/Types.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    const SXC::Grass::Device* s_device = nullptr;

    SXC::Settings::Bool& Setting() {
        static SXC::Settings::Bool s_on("Grass", "Instanced", true, "Grass: instanced",
                                        "The ground-effect plants drawn instanced on the GPU (each chunk's plants uploaded once) instead of "
                                        "the client re-baking them on the CPU every frame. Vulkan only; off: the stock grass.");
        return s_on;
    }

    SXC::Settings::Int& PlacementSetting() {
        static SXC::Settings::Int s_mode("Grass", "Placement", 2, 0, 2, "Grass: placement",
                                         "0: each cell's ground effect from the layer the map file marks dominant (stock; some maps mark a 0% "
                                         "base dominant: no grass there). 1: the dominant layer worked out from the paint (the largest share in "
                                         "the cell). 2: every layer's plants by how much of it is painted where they fall. Stock and instanced "
                                         "grass alike.");
        return s_mode;
    }

    SXC::Settings::Float& SteepestSetting() {
        static SXC::Settings::Float s_value("Grass", "SteepestGround", 0.4f, 0.1f, 1.0f, "Grass: steepest ground",
                                            "No ground-effect plants on terrain steeper than this, as the up part of the ground's normal "
                                            "(stock 0.4: about 66 degrees; 0.7: 45; 0.87: 30). Stock and instanced grass alike.");
        return s_value;
    }

    SXC::Settings::Float& SlopeSinkSetting() {
        static SXC::Settings::Float s_value("Grass", "SlopeSink", 0.0f, 0.0f, 1.5f, "Grass: sink on slopes",
                                            "Upright plants on a slope hang over the ground on their downhill side; this lowers each by that "
                                            "drop (its base's reach x the slope) times this factor (stock 0).");
        return s_value;
    }

    SXC::Settings::Float& PaintedMinSetting() {
        static SXC::Settings::Float s_min("Grass", "PaintedMin", 0.3f, 0.0f, 1.0f, "Grass: least paint",
                                          "Placement 2: no plants of a layer where less of it than this is painted (a trace of grass on rock).");
        return s_min;
    }

    // A plant model on the device (by GroundEffectDoodad id).
    struct Model {
        bool ok = false;
        uint32_t firstIndex = 0, indexCount = 0;
        int32_t vertexOffset = 0;
    };
    std::unordered_map<uint32_t, Model> s_models;

    // A chunk's plants in the pool: per batch (texture) and plant model, a contiguous run.
    struct Group {
        CTexture* texture;
        uint32_t doodadId;
        uint32_t offset;   // into the chunk's range
        uint32_t count;
        float copies;      // step 7d: the most copies and radius of its plants (which chunks draw copies)
        float radius;
    };
    struct Chunk {
        bool ok = false;
        uint32_t first = 0, count = 0;
        uint32_t adds = 0;   // the batches' adds when built (a change rebuilds)
        uint32_t generation = 0;   // the tables' when built (the motion's overrides, the density's provider)
        float cornerX = 0.0f, cornerY = 0.0f;   // its top-left corner (it spans 33.3 yd toward -x and -y)
        std::vector<Group> groups;
        std::vector<std::pair<int32_t, int32_t>> missing;   // step 7d: neighbouring chunks its edges wait for
    };
    std::unordered_map<const CDetailDoodadInst*, Chunk> s_chunks;

    // This frame's runs, per texture.
    struct TextureRuns {
        CTexture* texture;
        std::vector<SXC::Grass::Run> runs;
    };
    std::vector<TextureRuns> s_frame;
    bool s_collecting = false;

    struct Stats {
        uint32_t chunks = 0, plants = 0, runs = 0, draws = 0, built = 0, stock = 0, copies = 0, copyRuns = 0, spilled = 0, deferred = 0;
    } s_stats, s_last;
    char s_status[320] = "";

    // --- the motion (step 7b; WXL's features/GrassDoodads and GrassMotion) ------------------------
    // Per plant model: the bend from its root to its tip in texture V, found from its geometry (the mean
    // V of the lowest and of the highest 12% of its vertices), unless the mod's table overrides it;
    // models under 0.15 yd don't sway. Its plants carry {root V, 1 / (tip V - root V), wind scale, push
    // scale}; a model with no usable mapping gets WXL's fallback (root at V 1, tip at V 0).
    constexpr float kRootTipBand = 0.12f;
    constexpr float kFlatHeight = 0.15f;

    struct Doodad {
        bool analyzed = false, valid = false, flat = false;
        float height = 0.0f, rootV = 1.0f, tipV = 0.0f;
        float bottom = 0.0f;      // step 7d: its lowest point (model z)
        float footprint = 0.0f;   // how far its lowest quarter reaches out from its origin (yd, unscaled)
        std::string path;
    };
    std::unordered_map<uint32_t, Doodad> s_doodads;
    std::unordered_map<uint32_t, sxc_grass_doodad> s_overrides;
    uint32_t s_generation = 1;   // bumped when the overrides or the density's provider change: every chunk is placed anew

    void PlaceAnew() {
        if (++s_generation == 0) {
            s_generation = 1;
        }
    }

    Doodad& Analyze(uint32_t id) {
        Doodad& d = s_doodads[id];

        if (d.analyzed || id >= CDetailDoodad::s_doodadList.Count()) {
            return d;
        }

        const CDetailDoodadData* data = CDetailDoodad::s_doodadList[id];
        CM2Model* model = data ? data->m_model : nullptr;

        if (!model || !(model->m_fFlags & 1) || !model->m_shared || !model->m_shared->m_skinData || !model->m_shared->m_data) {
            return d;
        }

        d.analyzed = true;
        d.path = model->m_shared->m_fileName ? model->m_shared->m_fileName : "";
        const M2SkinProfile* skin = model->m_shared->m_skinData;
        const M2Data* m2 = model->m_shared->m_data;
        float minZ = 1e30f, maxZ = -1e30f;

        for (uint32_t v = 0; v < skin->m_vertices.m_count; v++) {
            const uint16_t g = skin->m_vertices[v];

            if (g < m2->m_vertices.m_count) {
                minZ = std::min(minZ, m2->m_vertices[g].m_position.z);
                maxZ = std::max(maxZ, m2->m_vertices[g].m_position.z);
            }
        }

        if (minZ > maxZ) {
            return d;
        }

        // Step 7d: the footprint (what hangs in the air on a slope's downhill side).
        d.bottom = minZ;

        for (uint32_t v = 0; v < skin->m_vertices.m_count; v++) {
            const uint16_t g = skin->m_vertices[v];

            if (g < m2->m_vertices.m_count && m2->m_vertices[g].m_position.z <= minZ + 0.25f * (maxZ - minZ)) {
                const C3Vector& p = m2->m_vertices[g].m_position;
                d.footprint = std::max(d.footprint, std::sqrt(p.x * p.x + p.y * p.y));
            }
        }

        const float band = (maxZ - minZ) * kRootTipBand;
        float rootSum = 0.0f, tipSum = 0.0f;
        uint32_t rootN = 0, tipN = 0;

        for (uint32_t v = 0; v < skin->m_vertices.m_count; v++) {
            const uint16_t g = skin->m_vertices[v];

            if (g >= m2->m_vertices.m_count) {
                continue;
            }

            const float z = m2->m_vertices[g].m_position.z, t = m2->m_vertices[g].m_texcoord[0].y;

            if (z <= minZ + band) {
                rootSum += t;
                rootN++;
            }

            if (z >= maxZ - band) {
                tipSum += t;
                tipN++;
            }
        }

        if (!rootN || !tipN) {
            return d;
        }

        d.height = maxZ - minZ;
        d.rootV = rootSum / rootN;
        d.tipV = tipSum / tipN;
        d.flat = d.height < kFlatHeight;
        d.valid = std::fabs(d.tipV - d.rootV) > 0.01f || d.flat;
        return d;
    }

    // What the plants of a model carry (false: its model isn't read yet).
    bool Entry(uint32_t id, float out[4]) {
        out[0] = 1.0f;
        out[1] = -1.0f;
        out[2] = 1.0f;
        out[3] = 1.0f;
        const Doodad& d = Analyze(id);

        if (!d.analyzed) {
            return false;
        }

        const auto o = s_overrides.find(id);
        float rootV = d.rootV, tipV = d.tipV, stiffness = 0.0f;
        bool wind = d.valid && !d.flat, push = d.valid && !d.flat, flip = false;

        if (o != s_overrides.end()) {
            const sxc_grass_doodad& r = o->second;
            rootV = r.root_v >= 0.0f ? r.root_v : rootV;
            tipV = r.tip_v >= 0.0f ? r.tip_v : tipV;
            const bool usable = d.valid || (r.root_v >= 0.0f && r.tip_v >= 0.0f);
            wind = usable && !(r.flags & SXC_GRASS_NO_WIND);
            push = usable && !(r.flags & SXC_GRASS_NO_PUSH);
            flip = (r.flags & SXC_GRASS_FLIP) != 0;
            stiffness = std::min(std::max(r.stiffness, 0.0f), 1.0f);
        } else if (!d.valid) {
            return true;   // the fallback
        }

        const float root = flip ? tipV : rootV, tip = flip ? rootV : tipV, span = tip - root, flex = 1.0f - stiffness;
        out[0] = root;
        out[1] = std::fabs(span) > 1e-4f ? 1.0f / span : 0.0f;
        out[2] = wind ? flex : 0.0f;
        out[3] = push ? flex : 0.0f;
        return true;
    }

    // The rows the motion reads from c64 on (Shaders/GrassMotion.glsl).
    constexpr uint32_t kMotionFirstRow = 64;
    constexpr uint32_t kMotionRows = 4 + SXC_GRASS_GRID * SXC_GRASS_GRID + SXC_GRASS_MAX_ACTORS;
    float s_motionRows[kMotionRows][4] = {};
    bool s_motionOn = false;

    void SetMotion(const sxc_grass_motion& m) {
        std::memset(s_motionRows, 0, sizeof(s_motionRows));
        s_motionOn = m.on != WXL_FALSE;

        if (!s_motionOn) {
            return;
        }

        const float anchor = std::min(std::max(m.anchor, 0.0f), 0.9f);
        const float inv = m.grid_spacing > 0.01f ? 1.0f / m.grid_spacing : 0.125f;
        const float h[4][4] = {
            { m.time, anchor, 1.0f / (1.0f - anchor), m.amplitude },
            { m.flutter, m.flutter_speed, m.push_strength, 1.0f },
            { inv, inv, m.grid_origin[0] * inv + 0.5f, m.grid_origin[1] * inv + 0.5f },
            { m.player[0], m.player[1], m.player[2], m.actor_range * m.actor_range },
        };
        std::memcpy(s_motionRows, h, sizeof(h));

        for (uint32_t c = 0; c < SXC_GRASS_GRID * SXC_GRASS_GRID; c++) {
            std::memcpy(s_motionRows[4 + c], m.wind[c], sizeof(m.wind[c]));
        }

        for (uint32_t a = 0; a < SXC_GRASS_MAX_ACTORS && a < m.actor_count; a++) {
            std::memcpy(s_motionRows[4 + SXC_GRASS_GRID * SXC_GRASS_GRID + a], m.actors[a], sizeof(m.actors[a]));
        }
    }

    // --- the density (step 7d; WXL's features/GrassDensity and the copies of GrassInstanced) ---------------
    // Each plant carries its copies (multiplier - 1), radius and spread (halves) and its spot in its terrain
    // cell (2 x triangle + s, t). The chunks near the player draw their runs once more per copy; the shader
    // (Shaders/GrassDensity.glsl, reading c148..c149) places, turns and grows each copy.
    constexpr float kCellSize = 4.1666665f;
    constexpr float kChunkSize = 8.0f * kCellSize;
    constexpr uint32_t kMaxCopies = 15;   // the device's copy entries (Vulkan/Grass kMaxCopies)
    constexpr uint32_t kDensityFirstRow = 148;

    // The ground marks (Shaders/GrassMarks.glsl, c150..c161): where the map lies (SXC/Cover: the mod's
    // set_marks) and what each channel does to plants (sxc.grass's set_marks).
    constexpr uint32_t kMarksFirstRow = 150;
    constexpr uint32_t kMarksRows = 12;
    sxc_grass_marks s_marks = {};
    constexpr uint32_t kPlantFloats = 24;   // a plant's record (Vulkan/Grass kPlantBytes)

    sxc_grass_density_fn s_densityFn = nullptr;
    void* s_densityUser = nullptr;
    sxc_grass_density_limits s_limits = {};
    float s_player[2] = {};
    bool s_playerKnown = false;

    // A float as a half (positive, small values: the density's).
    uint16_t Half(float f) {
        uint32_t x;
        std::memcpy(&x, &f, sizeof(x));
        const uint32_t sign = (x >> 16) & 0x8000u;
        const int32_t exponent = static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;

        if (exponent <= 0) {
            return static_cast<uint16_t>(sign);
        }

        if (exponent >= 31) {
            return static_cast<uint16_t>(sign | 0x7BFFu);
        }

        // Rounded; a carry out of the mantissa rightly raises the exponent.
        return static_cast<uint16_t>(sign | ((static_cast<uint32_t>(exponent) << 10) + (((x & 0x7FFFFFu) + 0x1000u) >> 13)));
    }

    // The provider's answers for one chunk build (per plant model and ground effect).
    struct DensityAnswer {
        uint32_t doodad, effect;
        sxc_grass_density value;
    };

    sxc_grass_density DensityOf(std::vector<DensityAnswer>& known, uint32_t area, int32_t map, uint32_t doodad, uint32_t effect) {
        for (const DensityAnswer& a : known) {
            if (a.doodad == doodad && a.effect == effect) {
                return a.value;
            }
        }

        sxc_grass_density v = { 1.0f, 0.0f, 0.0f };

        if (!s_densityFn || !s_densityFn(s_densityUser, area, map, doodad, effect, &v) || !(v.multiplier >= 1.0f) || !(v.radius > 0.0f)) {
            v = { 1.0f, 0.0f, 0.0f };
        }

        v.multiplier = std::min(v.multiplier, static_cast<float>(kMaxCopies + 1));
        v.spread = std::max(v.spread, 0.0f);
        known.push_back(DensityAnswer{ doodad, effect, v });
        return v;
    }

    // The ground effect painted where a plant grows: its cell's layer (as CMapChunk::CreateDetailDoodads
    // chose it).
    uint32_t EffectAt(const CMapChunk* chunk, uint32_t cell) {
        if (!chunk->m_lowQualityTexMap || !chunk->m_layers) {
            return 0;
        }

        const uint32_t x = cell & 7, y = (cell >> 3) & 7;
        const uint32_t layer = (reinterpret_cast<const uint16_t*>(chunk->m_lowQualityTexMap)[y] & CMap::s_2bitSplatMask[x]) >> CMap::s_2bitSplatShft[x];
        return layer < chunk->m_header->m_nLayers ? chunk->m_layers[layer].m_offectId : 0;
    }

    // A placed plant's spot in its cell: the chunk-space position runs from the cell's corner toward -y (s)
    // and -x (t).
    void SpotOf(const CDetailDoodadInstAdd& add, float& s, float& t) {
        const uint32_t cell = add.m_triIndex >> 2, x = cell & 7, y = (cell >> 3) & 7;
        s = std::min(std::max((-add.m_position.y - x * kCellSize) / kCellSize, 0.0f), 0.9999f);
        t = std::min(std::max((-add.m_position.x - y * kCellSize) / kCellSize, 0.0f), 0.9999f);
    }

    // Whether the ground marks leave a plant model alone (GroundEffectDoodadWind's "no marks": stones).
    bool Unmarked(uint32_t id) {
        const auto o = s_overrides.find(id);
        return o != s_overrides.end() && (o->second.flags & SXC_GRASS_NO_MARKS) != 0;
    }

    // A plant's density values, whether the marks pass it by (1: GrassMarks.glsl) and its spot into its
    // record (floats 20..23).
    void PlaceDensity(const sxc_grass_density& v, bool unmarked, float s, float t, uint32_t tri, float* out) {
        const uint16_t halves[4] = { Half(v.multiplier - 1.0f), Half(v.radius), Half(v.spread), Half(unmarked ? 1.0f : 0.0f) };
        std::memcpy(out, halves, sizeof(halves));
        out[2] = 2.0f * static_cast<float>(tri & 3) + s;
        out[3] = t;
    }

    // --- soft edges (step 7d, a test) ------------------------------------------------------------------
    // Where a cell with plants borders one without, the plants near the border shrink (toward `shrink` at
    // the border, over `band` yards), and some spill onto the bare ground next to it (up to `spill` yards,
    // smaller with the way out), each on its own terrain triangle. Cells are found by their global indices
    // (as CMap::GetChunkAtPosition: X along -y, Y along -x from the map's corner); a neighbour whose chunk
    // isn't loaded yet counts as unknown (no shrink, no spill toward it) and the chunk is placed again once
    // it loads.
    constexpr float kMapCorner = 17066.666f;
    constexpr int32_t kGridMargin = 3;   // cells around a chunk an edge may reach
    constexpr int32_t kGridSide = 8 + 2 * kGridMargin;

    sxc_grass_edges s_edges = {};
    uint32_t s_frameNumber = 0;
    constexpr double kBuildBudgetMs = 4.0;   // placing chunks per frame (at least one)
    std::chrono::steady_clock::time_point s_frameStart;

    struct CellInfo {
        bool known = false, ground = false, plants = false;
        const CTexture* texture = nullptr;
        const CMapChunk* chunk = nullptr;
        uint32_t x = 0, y = 0;
    };

    CMapArea* AreaOfChunk(int32_t chunkX, int32_t chunkY) {
        return chunkX < 0 || chunkY < 0 || chunkX >= 1024 || chunkY >= 1024 ? nullptr : CMap::m_areaTable[64 * (chunkY >> 4) + (chunkX >> 4)];
    }

    CellInfo CellAt(int32_t gx, int32_t gy) {
        CellInfo c;
        const int32_t chunkX = gx >> 3, chunkY = gy >> 3;
        CMapArea* area = gx < 0 || gy < 0 ? nullptr : AreaOfChunk(chunkX, chunkY);

        if (!area || area->m_asyncObject) {
            return c;
        }

        const CMapChunk* chunk = area->m_mapChunks[(chunkX & 0xF) + 16 * (chunkY & 0xF)];

        if (!chunk || !chunk->m_header || !chunk->m_height || !chunk->m_layers || !chunk->m_lowQualityTexMap || !chunk->m_predTexture) {
            return c;
        }

        c.known = true;
        c.chunk = chunk;
        c.x = static_cast<uint32_t>(gx & 7);
        c.y = static_cast<uint32_t>(gy & 7);

        if (chunk->m_header->m_holes & CMap::s_holeMask[4 * (c.y >> 1) + (c.x >> 1)]) {
            return c;   // a hole: no ground
        }

        c.ground = true;
        const uint32_t layer = (reinterpret_cast<const uint16_t*>(chunk->m_lowQualityTexMap)[c.y] & CMap::s_2bitSplatMask[c.x]) >> CMap::s_2bitSplatShft[c.x];

        if (layer >= chunk->m_header->m_nLayers) {
            return c;
        }

        const SMLayer& l = chunk->m_layers[layer];

        if (l.m_textureId < area->m_textures.Count()) {
            c.texture = area->m_textures[l.m_textureId].m_texture;
        }

        if ((chunk->m_predTexture[c.y] >> c.x) & 1) {
            return c;   // painted "no ground effects"
        }

        const GroundEffectTextureRec* rec = l.m_offectId ? g_groundEffectTextureDB.GetRecord(l.m_offectId) : nullptr;
        c.plants = rec && (rec->m_doodadID[0] || rec->m_doodadID[1] || rec->m_doodadID[2] || rec->m_doodadID[3]);
        return c;
    }

    // The ground at a spot of a cell (s, t in 0..1): its world height and its triangle (the cell's centre
    // and two corners, as CreateDetailDoodads makes the planes). False: too steep for plants (as stock).
    bool GroundAt(const CellInfo& c, float s, float t, float& z, C3Vector& normal, uint32_t& tri) {
        const float* h = c.chunk->m_height;
        auto height = [&](uint32_t i, uint32_t j) { return h[17 * (c.y + j) + c.x + i]; };
        tri = (s > t ? 1u : 0u) + (s + t > 1.0f ? 2u : 0u);
        const float as = tri == 3 ? 1.0f : 0.0f, at = tri == 2 ? 1.0f : 0.0f;
        const float bs = tri == 0 ? 0.0f : 1.0f, bt = tri == 1 ? 0.0f : 1.0f;

        // Chunk space: x runs along -t, y along -s.
        auto local = [&](float ps, float pt, float pz) { return C3Vector(-(c.y + pt) * kCellSize, -(c.x + ps) * kCellSize, pz); };
        const C3Vector pc = local(0.5f, 0.5f, h[17 * c.y + 9 + c.x]);
        const C3Vector pa = local(as, at, height(static_cast<uint32_t>(as), static_cast<uint32_t>(at)));
        const C3Vector pb = local(bs, bt, height(static_cast<uint32_t>(bs), static_cast<uint32_t>(bt)));
        const C3Vector e1 = pa - pc, e2 = pb - pc;
        C3Vector n(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);

        if (len < 1e-6f) {
            return false;
        }

        n = n * ((n.z < 0.0f ? -1.0f : 1.0f) / len);

        if (n.z < SteepestSetting().Get()) {
            return false;
        }

        const C3Vector p = local(s, t, 0.0f);
        z = c.chunk->m_topLeftCoords.z + pc.z - (n.x * (p.x - pc.x) + n.y * (p.y - pc.y)) / n.z;
        normal = n;
        return true;
    }

    uint32_t Hash(uint32_t x) {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    float Random(uint32_t& seed) {
        seed = Hash(seed + 0x9e3779b9u);
        return static_cast<float>(seed >> 8) * (1.0f / 16777216.0f);
    }

    // The cells around one chunk, looked up once per build.
    struct CellGrid {
        int32_t gx0 = 0, gy0 = 0;
        CellInfo cells[kGridSide * kGridSide];
        bool looked[kGridSide * kGridSide] = {};
        std::vector<std::pair<int32_t, int32_t>>* missing = nullptr;   // chunks not loaded yet

        const CellInfo& At(int32_t gx, int32_t gy) {
            static const CellInfo s_none;
            const int32_t i = gx - gx0 + kGridMargin, j = gy - gy0 + kGridMargin;

            if (i < 0 || j < 0 || i >= kGridSide || j >= kGridSide) {
                return s_none;
            }

            const int32_t k = j * kGridSide + i;

            if (!looked[k]) {
                looked[k] = true;
                cells[k] = CellAt(gx, gy);

                if (!cells[k].known && missing && AreaOfChunk(gx >> 3, gy >> 3) != nullptr) {
                    const std::pair<int32_t, int32_t> chunk(gx >> 3, gy >> 3);

                    if (std::find(missing->begin(), missing->end(), chunk) == missing->end()) {
                        missing->push_back(chunk);
                    }
                }
            }

            return cells[k];
        }
    };

    // Whether a bare cell may take spilled plants from a cell with `texture`.
    bool TakesSpill(const CellInfo& c, const CTexture* texture) {
        return c.known && c.ground && !c.plants && (!s_edges.same_texture || (c.texture && c.texture == texture));
    }

    // How many copies of a group a chunk draws this frame: all within half the radius of the player, none
    // past it (from the chunk's nearest point; the shader fades each plant by its own distance).
    uint32_t CopiesFor(const Group& g, float distance) {
        const float radius = std::min(g.radius, s_limits.max_radius);
        const float copies = std::min(g.copies, s_limits.max_multiplier - 1.0f);

        if (radius <= 0.0f || copies <= 0.0f || distance >= radius) {
            return 0;
        }

        const float t = std::min(std::max(2.0f - 2.0f * distance / radius, 0.0f), 1.0f);
        return std::min(static_cast<uint32_t>(std::ceil(copies * t)), kMaxCopies);
    }

    uint32_t AddsOf(CDetailDoodadInst* inst) {
        uint32_t n = 0;

        for (CDetailDoodadInstBatch& b : inst->m_batches) {
            n += b.m_texture ? b.m_adds.Count() : 0;
        }

        return n;
    }

    // The plant model's geometry on the device: every index of its skin, its skin's vertices (position,
    // first texture coordinate), as FillIndices / FillVertices take them.
    const Model& ModelFor(uint32_t id) {
        Model& m = s_models[id];

        if (m.ok || id >= CDetailDoodad::s_doodadList.Count()) {
            return m;
        }

        const CDetailDoodadData* data = CDetailDoodad::s_doodadList[id];
        CM2Model* model = data ? data->m_model : nullptr;

        if (!model || !(model->m_fFlags & 1) || !model->m_shared || !model->m_shared->m_skinData || !model->m_shared->m_data) {
            return m;
        }

        const M2SkinProfile* skin = model->m_shared->m_skinData;
        const M2Data* m2 = model->m_shared->m_data;
        static std::vector<float> vertices;
        static std::vector<uint16_t> indices;
        vertices.clear();
        indices.clear();

        for (uint32_t v = 0; v < skin->m_vertices.m_count; v++) {
            const uint16_t global = skin->m_vertices[v];
            const M2Vertex& src = global < m2->m_vertices.m_count ? m2->m_vertices[global] : m2->m_vertices[0];
            vertices.insert(vertices.end(), { src.m_position.x, src.m_position.y, src.m_position.z, src.m_texcoord[0].x, src.m_texcoord[0].y });
        }

        for (uint32_t i = 0; i < skin->m_indices.m_count; i++) {
            indices.push_back(skin->m_indices[i]);
        }

        m.ok = s_device->mesh(id, vertices.data(), skin->m_vertices.m_count, indices.data(), static_cast<uint32_t>(indices.size()), m.firstIndex, m.vertexOffset,
                              m.indexCount);
        return m;
    }

    void Release(Chunk& c) {
        if (c.count && s_device) {
            s_device->release(c.first, c.count);
        }

        c = Chunk{};
    }

    // One plant on its way into the pool.
    struct Item {
        uint32_t batch, doodad;
        float record[kPlantFloats];
        float copies, radius;
    };

    // Step 7d: a placed plant near a bare neighbour shrinks; some of it spills onto that ground.
    void Edge(const CDetailDoodadInstAdd& add, float s, float t, uint32_t batch, const sxc_grass_density& density, const CTexture* texture, CellGrid& grid,
              int32_t gx, int32_t gy, Item& item, std::vector<Item>& spilled) {
        struct Side {
            int32_t dx, dy;
        };
        Side sides[8];
        uint32_t sideCount = 0;
        float nearest = 1e30f;

        for (int32_t dy = -1; dy <= 1; dy++) {
            for (int32_t dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) {
                    continue;
                }

                const CellInfo& n = grid.At(gx + dx, gy + dy);

                if (!n.known || n.plants) {
                    continue;
                }

                const float ax = dx > 0 ? 1.0f - s : (dx < 0 ? s : 0.0f), ay = dy > 0 ? 1.0f - t : (dy < 0 ? t : 0.0f);
                const float d = kCellSize * std::sqrt(ax * ax + ay * ay);
                nearest = std::min(nearest, d);

                if (s_edges.spill > 0.0f && d <= s_edges.spill && TakesSpill(n, texture)) {
                    sides[sideCount++] = Side{ dx, dy };
                }
            }
        }

        // Shrink toward the border.
        if (s_edges.band > 0.0f && nearest < s_edges.band) {
            const float f = s_edges.shrink + (1.0f - s_edges.shrink) * (nearest / s_edges.band);

            for (int r = 0; r < 3; r++) {
                for (int k = 0; k < 3; k++) {
                    item.record[4 * r + k] *= f;
                }
            }
        }

        if (!sideCount || s_edges.share <= 0.0f) {
            return;
        }

        // Spill: from the plant, past the nearest point of a bare neighbour, a little way onto it.
        uint32_t seed = Hash(static_cast<uint32_t>(gx) * 73856093u ^ static_cast<uint32_t>(gy) * 19349663u ^ Hash(static_cast<uint32_t>(s * 65535.0f) << 16 | static_cast<uint32_t>(t * 65535.0f)));
        const float share = std::min(s_edges.share, 4.0f);
        uint32_t tries = static_cast<uint32_t>(share);
        tries += Random(seed) < share - static_cast<float>(tries) ? 1u : 0u;

        for (uint32_t k = 0; k < tries; k++) {
            const Side& side = sides[std::min(static_cast<uint32_t>(Random(seed) * sideCount), sideCount - 1)];
            const float px = static_cast<float>(gx) + s, py = static_cast<float>(gy) + t;
            const float nx = std::min(std::max(px, static_cast<float>(gx + side.dx)), static_cast<float>(gx + side.dx + 1));
            const float ny = std::min(std::max(py, static_cast<float>(gy + side.dy)), static_cast<float>(gy + side.dy + 1));
            float ox = nx - px, oy = ny - py;
            const float olen = std::sqrt(ox * ox + oy * oy);

            if (olen < 1e-4f) {
                continue;
            }

            ox /= olen;
            oy /= olen;
            const float depth = s_edges.spill * Random(seed) * Random(seed);   // most just past the border
            const float lateral = (Random(seed) - 0.5f) * 1.5f;
            const float tx = nx + (ox * (depth + 0.05f) - oy * lateral) / kCellSize;
            const float ty = ny + (oy * (depth + 0.05f) + ox * lateral) / kCellSize;
            const int32_t cx = static_cast<int32_t>(std::floor(tx)), cy = static_cast<int32_t>(std::floor(ty));
            const CellInfo& target = grid.At(cx, cy);

            if (!TakesSpill(target, texture)) {
                continue;
            }

            const float ts = std::min(std::max(tx - cx, 0.0f), 0.9999f), tt = std::min(std::max(ty - cy, 0.0f), 0.9999f);
            float z;
            C3Vector normal;
            uint32_t tri;

            if (!GroundAt(target, ts, tt, z, normal, tri)) {
                continue;
            }

            // Smaller the farther out (from the border size down to a third of it).
            const float f = s_edges.shrink * (1.0f - 0.67f * depth / s_edges.spill);
            CDetailDoodadInstAdd a = add;
            // From the target chunk's own corner (its height is that chunk's: a ported map's corners may sit
            // off the ideal grid).
            const C3Vector& tc = target.chunk->m_topLeftCoords;
            a.m_position = C3Vector(tc.x - (target.y + tt) * kCellSize, tc.y - (target.x + ts) * kCellSize, z);
            a.m_rotation = Random(seed) * 6.2831853f;
            a.m_scale = add.m_scale * f;
            a.m_position.z -= SXC::Grass::SlopeSink(add.m_doodadId, a.m_scale, normal);
            a.m_normal = normal;
            a.m_triIndex = static_cast<uint16_t>((target.x + 8 * target.y) * 4 + tri);

            Item spill;
            spill.batch = batch;
            spill.doodad = add.m_doodadId;
            SxcGrassPlant(a, C3Vector(0.0f, 0.0f, 0.0f), spill.record);
            Entry(add.m_doodadId, spill.record + 16);
            sxc_grass_density d = density;
            d.multiplier = 1.0f + (density.multiplier - 1.0f) * f;
            PlaceDensity(d, Unmarked(add.m_doodadId), ts, tt, tri, spill.record + 20);
            spill.copies = d.multiplier - 1.0f;
            spill.radius = d.multiplier > 1.0f ? d.radius : 0.0f;
            spilled.push_back(spill);
        }
    }

    // A chunk's plants into the pool: each batch's plants grouped by model (contiguous runs), placed in
    // world space (the chunk's corner added to the stock bake's chunk space); step 7d: their density, and
    // the soft edges' spilled plants with them.
    bool Build(CDetailDoodadInst* inst, Chunk& c) {
        const uint32_t adds = AddsOf(inst);
        c.adds = adds;

        if (!adds || !inst->m_chunk) {
            c.ok = adds == 0;
            return c.ok;
        }

        static std::vector<float> baked;
        static std::vector<Item> items;
        static std::vector<DensityAnswer> densities;
        items.clear();
        densities.clear();
        c.generation = s_generation;
        c.missing.clear();
        const C3Vector& corner = inst->m_chunk->m_topLeftCoords;
        c.cornerX = corner.x;
        c.cornerY = corner.y;
        const uint32_t area = static_cast<uint32_t>(inst->m_chunk->m_areaId);
        const int32_t map = ::World::QueryMapId();

        // The cells around the chunk (the soft edges).
        const bool edges = s_edges.on && (s_edges.band > 0.0f || s_edges.spill > 0.0f);
        static CellGrid grid;
        grid = CellGrid{};
        grid.gx0 = static_cast<int32_t>(std::lround((kMapCorner - corner.y) / kCellSize));
        grid.gy0 = static_cast<int32_t>(std::lround((kMapCorner - corner.x) / kCellSize));
        grid.missing = &c.missing;
        std::vector<Item> spilled;

        for (uint32_t bi = 0; bi < 4; bi++) {
            CDetailDoodadInstBatch& b = inst->m_batches[bi];

            if (!b.m_texture || !b.m_adds.Count()) {
                continue;
            }

            baked.resize(static_cast<size_t>(b.m_adds.Count()) * 16);
            SxcGrassPlants(b, corner, baked.data());

            for (uint32_t i = 0; i < b.m_adds.Count(); i++) {
                const CDetailDoodadInstAdd& add = b.m_adds[i];
                Item item;
                item.batch = bi;
                item.doodad = add.m_doodadId;
                std::copy(baked.data() + static_cast<size_t>(i) * 16, baked.data() + static_cast<size_t>(i) * 16 + 16, item.record);
                Entry(add.m_doodadId, item.record + 16);   // step 7b: the motion values

                // Step 7d: the density where it grows, its spot, its edge.
                const uint32_t cell = add.m_triIndex >> 2;
                const sxc_grass_density density = DensityOf(densities, area, map, add.m_doodadId, EffectAt(inst->m_chunk, cell));
                float s, t;
                SpotOf(add, s, t);
                PlaceDensity(density, Unmarked(add.m_doodadId), s, t, add.m_triIndex & 3, item.record + 20);
                item.copies = density.multiplier - 1.0f;
                item.radius = density.multiplier > 1.0f ? density.radius : 0.0f;

                if (edges) {
                    const int32_t gx = grid.gx0 + static_cast<int32_t>(cell & 7), gy = grid.gy0 + static_cast<int32_t>((cell >> 3) & 7);
                    Edge(add, s, t, bi, density, grid.At(gx, gy).texture, grid, gx, gy, item, spilled);
                }

                items.push_back(item);
            }
        }

        items.insert(items.end(), spilled.begin(), spilled.end());

        // Each batch's plants by model (contiguous runs), the placed ones first.
        std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.batch != y.batch ? x.batch < y.batch : x.doodad < y.doodad; });
        const uint32_t total = static_cast<uint32_t>(items.size());

        if (!s_device->allocate(total, c.first)) {
            return false;
        }

        c.count = total;
        static std::vector<float> records;
        records.resize(static_cast<size_t>(total) * kPlantFloats);

        for (uint32_t k = 0; k < total; k++) {
            const Item& item = items[k];
            std::copy(item.record, item.record + kPlantFloats, records.data() + static_cast<size_t>(k) * kPlantFloats);
            CTexture* texture = inst->m_batches[item.batch].m_texture;

            if (c.groups.empty() || c.groups.back().doodadId != item.doodad || c.groups.back().texture != texture) {
                c.groups.push_back(Group{ texture, item.doodad, k, 0, 0.0f, 0.0f });
            }

            Group& g = c.groups.back();
            g.count++;
            g.copies = std::max(g.copies, item.copies);
            g.radius = std::max(g.radius, item.radius);
        }

        s_device->write(c.first, records.data(), total);
        s_stats.spilled += static_cast<uint32_t>(spilled.size());
        c.ok = true;
        s_stats.built++;
        return true;
    }

    // Whether a chunk's edges waited for a neighbour that has loaded since (checked now and then).
    bool NeighboursArrived(const Chunk& c) {
        for (const std::pair<int32_t, int32_t>& chunk : c.missing) {
            CMapArea* area = AreaOfChunk(chunk.first, chunk.second);
            const CMapChunk* m = area && !area->m_asyncObject ? area->m_mapChunks[(chunk.first & 0xF) + 16 * (chunk.second & 0xF)] : nullptr;

            if (m && m->m_header && m->m_height && m->m_layers && m->m_lowQualityTexMap && m->m_predTexture) {
                return true;
            }
        }

        return false;
    }

    TextureRuns& RunsFor(CTexture* texture) {
        for (TextureRuns& t : s_frame) {
            if (t.texture == texture) {
                return t;
            }
        }

        s_frame.push_back(TextureRuns{ texture, {} });
        return s_frame.back();
    }
}

namespace SXC::Grass {
    void SetDevice(const Device* device) {
        if (device == s_device) {
            return;
        }

        // A new device (or none): what the old one held is gone with it.
        s_device = device;
        s_models.clear();
        s_chunks.clear();
        s_frame.clear();
    }

    bool Enabled() {
        return Setting().Get();
    }

    void SetEnabled(bool on) {
        Setting().Set(on);
    }

    uint32_t Placement() {
        return static_cast<uint32_t>(PlacementSetting().Get());
    }

    float PaintedMin() {
        return PaintedMinSetting().Get();
    }

    float SteepestGround() {
        return SteepestSetting().Get();
    }

    float SlopeSink(uint32_t doodadId, float scale, const C3Vector& normal) {
        const float sink = SlopeSinkSetting().Get();

        if (sink <= 0.0f || normal.z <= 0.0f || normal.z >= 0.9999f || doodadId >= CDetailDoodad::s_doodadList.Count()) {
            return 0.0f;
        }

        // Terrain-aligned plants lie on the slope already.
        const CDetailDoodadData* data = CDetailDoodad::s_doodadList[doodadId];

        if (!data || !data->m_record || (data->m_record->m_flags & 1)) {
            return 0.0f;
        }

        const Doodad& d = Analyze(doodadId);
        return sink * d.footprint * scale * std::sqrt(1.0f - normal.z * normal.z) / normal.z;
    }

    bool Active() {
        return s_device && CDetailDoodad::s_bUseShaders && Enabled();
    }

    void Begin() {
        s_collecting = Active();

        for (TextureRuns& t : s_frame) {
            t.runs.clear();
        }

        s_stats = Stats{};
        s_playerKnown = false;
        s_frameNumber++;
        s_frameStart = std::chrono::steady_clock::now();

        if (CGPlayer_C* player = s_collecting && s_limits.on ? ClntObjMgrGetActivePlayerObj() : nullptr) {
            const C3Vector p = player->GetPosition();
            s_player[0] = p.x;
            s_player[1] = p.y;
            s_playerKnown = true;
        }
    }

    bool Take(CDetailDoodadInst* inst) {
        if (!s_collecting || !inst) {
            return false;
        }

        Chunk& c = s_chunks[inst];

        // Step 7d: a chunk whose edges waited for a neighbour is placed again once that one has loaded.
        const bool arrived = !c.missing.empty() && ((s_frameNumber + static_cast<uint32_t>(c.first)) & 31) == 0 && NeighboursArrived(c);

        if (c.adds != AddsOf(inst) || !c.ok || c.generation != s_generation || arrived) {
            // Placing is spread over frames (a settings change places every chunk again): past the frame's
            // budget a placed chunk keeps its old plants, a new one waits (drawn nowhere this frame).
            const bool over = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s_frameStart).count() > kBuildBudgetMs;

            if (over && s_stats.built > 0) {
                s_stats.deferred++;

                if (!c.ok || c.adds != AddsOf(inst)) {
                    return true;
                }
            } else {
                Release(c);

                if (!Build(inst, c)) {
                    Release(c);
                    s_stats.stock++;
                    return false;
                }
            }
        }

        s_stats.chunks++;

        // Step 7d: the player's distance to the chunk's nearest point (it spans 33.3 yd toward -x and -y).
        float distance = 1e30f;

        if (s_playerKnown) {
            const float dx = std::max(std::max(c.cornerX - kChunkSize - s_player[0], s_player[0] - c.cornerX), 0.0f);
            const float dy = std::max(std::max(c.cornerY - kChunkSize - s_player[1], s_player[1] - c.cornerY), 0.0f);
            distance = std::sqrt(dx * dx + dy * dy);
        }

        for (const Group& g : c.groups) {
            const Model& m = ModelFor(g.doodadId);

            if (!m.ok) {
                continue;
            }

            std::vector<Run>& runs = RunsFor(g.texture).runs;
            runs.push_back(Run{ m.firstIndex, m.indexCount, m.vertexOffset, c.first + g.offset, g.count, 0 });
            s_stats.plants += g.count;
            s_stats.runs++;

            const uint32_t copies = s_playerKnown ? CopiesFor(g, distance) : 0;

            for (uint32_t k = 1; k <= copies; k++) {
                runs.push_back(Run{ m.firstIndex, m.indexCount, m.vertexOffset, c.first + g.offset, g.count, k });
            }

            s_stats.copies += copies * g.count;
            s_stats.copyRuns += copies;
        }

        return true;
    }

    void End() {
        if (!s_collecting) {
            return;
        }

        s_collecting = false;

        // One world matrix for all: the plants are in world space, the view is the scene's.
        C44Matrix world;
        world.Translate(C3Vector(0.0f, 0.0f, 0.0f) - CWorldScene::s_activeWorldView);
        CDetailDoodad::SetupVertexShader(world, 0);

        // Step 7b: the motion's rows (after the client's c0..c22), every frame: other shaders use them too.
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, kMotionFirstRow, &s_motionRows[0][0], kMotionRows);

        // Step 7d: the density's rows (Shaders/GrassDensity.glsl): the player, the limits.
        const float density[2][4] = {
            { s_player[0], s_player[1], std::max(s_limits.max_multiplier - 1.0f, 0.0f), s_limits.max_radius },
            { s_limits.max_spread, 0.0f, 0.0f, 0.0f },
        };
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, kDensityFirstRow, &density[0][0], 2);

        // The ground marks' rows: the map (1 / cell, size, first i, j; size 0: none), on, then per channel
        // and blight the tint (rgb, strength) and size, wind.
        float marks[kMarksRows][4] = {};
        float cell = 1.0f;
        uint32_t size = 0;
        int32_t firstI = 0, firstJ = 0;

        if (s_marks.on && SXC::Cover::Marks(cell, size, firstI, firstJ)) {
            const float m[4] = { 1.0f / std::max(cell, 0.01f), static_cast<float>(size), static_cast<float>(firstI), static_cast<float>(firstJ) };
            std::memcpy(marks[0], m, sizeof(m));
            marks[1][0] = 1.0f;
            const sxc_grass_mark_look* looks[5] = { &s_marks.channel[0], &s_marks.channel[1], &s_marks.channel[2], &s_marks.channel[3], &s_marks.blight };

            for (int k = 0; k < 5; k++) {
                const sxc_grass_mark_look& l = *looks[k];
                const float a[4] = { l.tint[0], l.tint[1], l.tint[2], std::min(std::max(l.tint_strength, 0.0f), 1.0f) };
                const float b[4] = { std::max(l.size, 0.0f), std::max(l.wind, 0.0f), 0.0f, 0.0f };
                std::memcpy(marks[2 + 2 * k], a, sizeof(a));
                std::memcpy(marks[3 + 2 * k], b, sizeof(b));
            }
        }

        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, kMarksFirstRow, &marks[0][0], kMarksRows);

        for (TextureRuns& t : s_frame) {
            if (t.runs.empty()) {
                continue;
            }

            // The copies after the plants, by copy number (each number binds its entry once).
            std::stable_sort(t.runs.begin(), t.runs.end(), [](const Run& a, const Run& b) { return a.copy < b.copy; });

            CGxTex* gxTex = TextureGetGxTex(t.texture, 0, nullptr);

            if (gxTex && s_device->draw(gxTex, t.runs.data(), static_cast<uint32_t>(t.runs.size()))) {
                s_stats.draws++;
            }
        }

        s_last = s_stats;
    }

    void Forget(CDetailDoodadInst* inst) {
        const auto it = s_chunks.find(inst);

        if (it != s_chunks.end()) {
            Release(it->second);
            s_chunks.erase(it);
        }
    }

    const char* Status() {
        if (!s_device) {
            return "off: this renderer draws the stock grass (Vulkan only)";
        }

        if (!Active()) {
            return CDetailDoodad::s_bUseShaders ? "off ([Grass] Instanced)" : "off: the client's grass shaders are off";
        }

        std::snprintf(s_status, sizeof(s_status),
                      "%u chunks, %u plants in %u runs, %u draws; %u copies in %u runs (density %s); %u chunks built this frame (%u plants spilled, %u "
                      "waiting), %u drawn the stock way; pool %u plants",
                      s_last.chunks, s_last.plants, s_last.runs, s_last.draws, s_last.copies, s_last.copyRuns,
                      !s_densityFn ? "no provider" : (s_limits.on ? "on" : "off"), s_last.built, s_last.spilled, s_last.deferred, s_last.stock,
                      s_device->plantsInUse());
        return s_status;
    }
}

// --- sxc.grass ----------------------------------------------------------------------------------------
namespace {
    wxl_bool WXL_CALL ApiAvailable() {
        return SXC::Grass::Active() ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL ApiSetMotion(const sxc_grass_motion* motion) {
        if (!motion || motion->struct_size < sizeof(sxc_grass_motion)) {
            s_motionOn = false;
            std::memset(s_motionRows, 0, sizeof(s_motionRows));
            return;
        }

        SetMotion(*motion);
    }

    void WXL_CALL ApiSetDoodads(const sxc_grass_doodad* rows, uint32_t count) {
        s_overrides.clear();

        for (uint32_t i = 0; rows && i < count; i++) {
            s_overrides[rows[i].id] = rows[i];
        }

        PlaceAnew();
    }

    void WXL_CALL ApiSetDensityProvider(sxc_grass_density_fn fn, void* user) {
        s_densityFn = fn;
        s_densityUser = user;
        PlaceAnew();
    }

    void WXL_CALL ApiSetDensityLimits(const sxc_grass_density_limits* limits) {
        if (!limits || limits->struct_size < sizeof(sxc_grass_density_limits)) {
            s_limits = sxc_grass_density_limits{};
            return;
        }

        s_limits = *limits;
        s_limits.max_multiplier = std::min(std::max(s_limits.max_multiplier, 1.0f), static_cast<float>(kMaxCopies + 1));
        s_limits.max_radius = std::max(s_limits.max_radius, 0.0f);
        s_limits.max_spread = std::max(s_limits.max_spread, 0.0f);
    }

    wxl_bool WXL_CALL ApiDoodadInfo(uint32_t id, sxc_grass_doodad_info* out) {
        if (!out || out->struct_size < sizeof(sxc_grass_doodad_info) || id >= CDetailDoodad::s_doodadList.Count() || !CDetailDoodad::s_doodadList[id]) {
            return WXL_FALSE;
        }

        const uint32_t size = out->struct_size;
        *out = sxc_grass_doodad_info{};
        out->struct_size = size;
        const Doodad& d = Analyze(id);
        static std::string path;
        path = d.path;
        out->path = path.c_str();
        out->analyzed = d.analyzed ? WXL_TRUE : WXL_FALSE;
        out->height = d.height;
        out->root_v = d.rootV;
        out->tip_v = d.tipV;
        out->flat = d.flat ? WXL_TRUE : WXL_FALSE;
        Entry(id, out->entry);
        return WXL_TRUE;
    }

    const char* WXL_CALL ApiStatus() {
        return SXC::Grass::Status();
    }

    void WXL_CALL ApiSetEdges(const sxc_grass_edges* edges) {
        sxc_grass_edges e = {};
        e.struct_size = sizeof(e);

        if (edges && edges->struct_size >= sizeof(sxc_grass_edges) && edges->on) {
            e.on = WXL_TRUE;
            e.band = std::min(std::max(edges->band, 0.0f), kCellSize);
            e.shrink = std::min(std::max(edges->shrink, 0.0f), 1.0f);
            e.spill = std::min(std::max(edges->spill, 0.0f), 2.0f * kCellSize);   // the grid around a chunk reaches that far
            e.share = std::min(std::max(edges->share, 0.0f), 4.0f);
            e.same_texture = edges->same_texture ? WXL_TRUE : WXL_FALSE;
        }

        if (std::memcmp(&e, &s_edges, sizeof(e)) != 0) {
            s_edges = e;
            PlaceAnew();
        }
    }

    void WXL_CALL ApiSetPlacement(uint32_t mode, float minShare) {
        mode = std::min(mode, SXC_GRASS_PLACE_PAINTED);
        minShare = std::min(std::max(minShare, 0.0f), 1.0f);

        if (mode == SXC::Grass::Placement() && minShare == PaintedMinSetting().Get()) {
            return;
        }

        // Every chunk's plants are placed again (CMapChunk::RenderPrep makes them anew as they're drawn).
        PlacementSetting().Set(static_cast<int>(mode));
        PaintedMinSetting().Set(minShare);
        CMap::ClearChunkDetailDoodads();
    }

    uint32_t WXL_CALL ApiPlacement(float* minShare) {
        if (minShare) {
            *minShare = PaintedMinSetting().Get();
        }

        return SXC::Grass::Placement();
    }

    void WXL_CALL ApiSetSlopes(float minNormalZ, float sink) {
        minNormalZ = std::min(std::max(minNormalZ, 0.1f), 1.0f);
        sink = std::min(std::max(sink, 0.0f), 1.5f);

        if (minNormalZ == SteepestSetting().Get() && sink == SlopeSinkSetting().Get()) {
            return;
        }

        SteepestSetting().Set(minNormalZ);
        SlopeSinkSetting().Set(sink);
        PlaceAnew();
        CMap::ClearChunkDetailDoodads();
    }

    void WXL_CALL ApiSlopes(float* minNormalZ, float* sink) {
        if (minNormalZ) {
            *minNormalZ = SteepestSetting().Get();
        }

        if (sink) {
            *sink = SlopeSinkSetting().Get();
        }
    }

    // Each placed plant near the player against the client's own terrain height: its origin (should be on
    // the ground), and for upright ones the lowest point of its base at 8 points around its reach.
    wxl_bool WXL_CALL ApiInspect(float radius, sxc_grass_inspect* out) {
        CGPlayer_C* player = ClntObjMgrGetActivePlayerObj();

        if (!out || out->struct_size < sizeof(sxc_grass_inspect) || !player) {
            return WXL_FALSE;
        }

        *out = sxc_grass_inspect{};
        out->struct_size = sizeof(sxc_grass_inspect);
        const C3Vector me = player->GetPosition();
        double slopeSum = 0.0, hangSlopeSum = 0.0;
        static std::string worstPath;
        worstPath.clear();

        for (CMapChunk* chunk = CMap::s_mapChunkList.Head(); chunk; chunk = CMap::s_mapChunkList.Next(chunk)) {
            CDetailDoodadInst* inst = chunk->m_detailDoodadInst;
            const C3Vector& corner = chunk->m_topLeftCoords;
            const float cx = corner.x - 0.5f * kChunkSize - me.x, cy = corner.y - 0.5f * kChunkSize - me.y;

            if (!inst || cx * cx + cy * cy > (radius + kChunkSize) * (radius + kChunkSize)) {
                continue;
            }

            for (CDetailDoodadInstBatch& b : inst->m_batches) {
                for (uint32_t i = 0; b.m_texture && i < b.m_adds.Count(); i++) {
                    const CDetailDoodadInstAdd& add = b.m_adds[i];
                    const C3Vector p = add.m_position + corner;
                    const float dx = p.x - me.x, dy = p.y - me.y;
                    float ground;

                    if (dx * dx + dy * dy > radius * radius || !SXC::WorldApi::GroundHeight(p.x, p.y, ground)) {
                        continue;
                    }

                    out->plants++;
                    const float slope = std::acos(std::min(std::max(add.m_normal.z, 0.0f), 1.0f)) * 57.29578f;
                    slopeSum += slope;
                    const float off = p.z - ground;

                    if (std::fabs(off) > 0.05f) {
                        out->off_ground++;
                    }

                    if (std::fabs(off) > std::fabs(out->off_max)) {
                        out->off_max = off;
                    }

                    const CDetailDoodadData* data = add.m_doodadId < CDetailDoodad::s_doodadList.Count() ? CDetailDoodad::s_doodadList[add.m_doodadId] : nullptr;

                    if (data && data->m_record && (data->m_record->m_flags & 1)) {
                        out->aligned++;
                        continue;
                    }

                    const Doodad& d = Analyze(add.m_doodadId);
                    const float reach = d.footprint * add.m_scale, bottom = p.z + d.bottom * add.m_scale;
                    float hang = 0.0f;

                    for (int k = 0; k < 8 && reach > 0.01f; k++) {
                        const float a = k * 0.78539816f;
                        float g;

                        if (SXC::WorldApi::GroundHeight(p.x + reach * std::cos(a), p.y + reach * std::sin(a), g)) {
                            hang = std::max(hang, bottom - g);
                        }
                    }

                    if (hang > 0.25f) {
                        out->hanging++;
                        hangSlopeSum += slope;
                    }

                    if (hang > out->hang_max) {
                        out->hang_max = hang;
                        out->worst[0] = p.x;
                        out->worst[1] = p.y;
                        out->worst[2] = p.z;
                        out->worst_slope = slope;
                        out->worst_scale = add.m_scale;
                        out->worst_doodad = add.m_doodadId;
                        worstPath = d.path;
                    }
                }
            }
        }

        out->slope = out->plants ? static_cast<float>(slopeSum / out->plants) : 0.0f;
        out->hang_slope = out->hanging ? static_cast<float>(hangSlopeSum / out->hanging) : 0.0f;
        out->worst_path = worstPath.c_str();
        return WXL_TRUE;
    }

    void WXL_CALL ApiSetMarks(const sxc_grass_marks* marks) {
        s_marks = marks && marks->struct_size >= sizeof(sxc_grass_marks) ? *marks : sxc_grass_marks{};
    }

    const sxc_grass_api s_api = { sizeof(sxc_grass_api), ApiAvailable,          ApiSetMotion,          ApiSetDoodads,
                                  ApiDoodadInfo,         ApiStatus,             ApiSetDensityProvider, ApiSetDensityLimits,
                                  ApiSetEdges,           ApiSetPlacement,       ApiPlacement,          ApiSetSlopes,
                                  ApiSlopes,             ApiInspect,            ApiSetMarks };
}

namespace SXC::Grass {
    const sxc_grass_api* Api() {
        return &s_api;
    }

    // The four cells around the grid's middle (the grid is centred on the player), averaged.
    bool WindAtPlayer(float& x, float& y, float& gust) {
        x = y = gust = 0.0f;

        if (!s_motionOn) {
            return false;
        }

        constexpr uint32_t kHalf = SXC_GRASS_GRID / 2;
        const uint32_t cells[4] = { (kHalf - 1) * SXC_GRASS_GRID + kHalf - 1, (kHalf - 1) * SXC_GRASS_GRID + kHalf,
                                    kHalf * SXC_GRASS_GRID + kHalf - 1, kHalf * SXC_GRASS_GRID + kHalf };

        for (uint32_t c : cells) {
            x += s_motionRows[4 + c][0] * 0.25f;
            y += s_motionRows[4 + c][1] * 0.25f;
            gust += s_motionRows[4 + c][2] * 0.25f;
        }

        return true;
    }
}
