// sxc.lights (SXC/Api/sxc_lights.h) and the light list the device pulls (SXC/Lights/Lights.h). Ported
// from WXL's env/Lights and features/DoodadLightTable (port plan, step 6a), reading the client's own
// structures instead of offsets.

#include "SXC/Lights/Lights.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "SXC/Api/sxc_lights.h"
#include "SXC/Log.h"
#include "SXC/Profiler/Profiler.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2ModelLight.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WoW/Source/Object/ObjectClient/GameObject_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/CMapBaseObjLink.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapDoodadDef.h"
#include "WoW/Source/WorldClient/MapLoad.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    using namespace SXC::Lights;

    static_assert(SXC_LIGHTS_MAX_POOL == kMaxPool && SXC_LIGHTS_MAX_PER_CELL == kMaxPerCell && SXC_LIGHTS_MAX_CELLS == kMaxCells,
                  "SXC/Lights/Lights.h follows the API");

    bool s_available = false;
    StatusFn s_deviceStatus = nullptr;

    sxc_lights_settings s_settings = {};
    bool s_haveSettings = false;

    sxc_lights_stats s_stats = {};
    char s_status[768] = "";

    // --- the lights by data ------------------------------------------------------------------------
    struct Row {
        sxc_lights_assignment a = {};
        std::string path;   // normalised
        int kind = -1;      // index into s_kinds, -1 none
    };
    std::vector<sxc_lights_kind> s_kinds;
    std::vector<Row> s_rows;
    std::unordered_map<std::string, std::vector<uint32_t>> s_byPath; // normalised path -> rows
    uint32_t s_tableGeneration = 1;
    bool s_anyEmitterRows = false;    // a row places an emitter (else emitters() walks nothing)

    // Per model file (CM2Shared, shared by every model of the file): its rows, and whether its own lights
    // are left out. The file name is kept to notice a CM2Shared freed and reused by another file.
    struct FileRows {
        uint32_t generation = 0;
        std::string name;
        const std::vector<uint32_t>* rows = nullptr;
        bool suppress = false;
    };
    std::unordered_map<const CM2Shared*, FileRows> s_files;

    std::string Normalize(const char* path) {
        std::string s = path ? path : "";

        for (char& c : s) {
            c = c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }

        return s;
    }

    // (s_files is trimmed only at the start of a frame's gathering: the lights keep pointers to its names.)
    const FileRows& RowsFor(const CM2Shared* shared) {
        FileRows& f = s_files[shared];

        if (f.generation != s_tableGeneration || f.name != shared->m_fileName) {
            f.generation = s_tableGeneration;
            f.name = shared->m_fileName;
            const auto it = s_byPath.find(Normalize(shared->m_fileName));
            f.rows = it != s_byPath.end() ? &it->second : nullptr;
            f.suppress = false;

            if (f.rows) {
                for (uint32_t r : *f.rows) {
                    f.suppress = f.suppress || (s_rows[r].a.flags & SXC_LIGHTS_SUPPRESS_MODEL_LIGHTS) != 0;
                }
            }
        }

        return f;
    }

    // --- this frame's lights -----------------------------------------------------------------------
    struct Found {
        C3Vector pos;
        C3Vector color;
        float radius = 0.0f;       // yd, scaled with the model; 0: the settings' radius
        float falloff = 1.0f;
        C3Vector spotDir = C3Vector(0.0f, 0.0f, -1.0f);
        float cosOuter = -2.0f, spotScale = 1.0f;
        int flickerMode = SXC_LIGHTS_FLICKER_DEFAULT;
        float flickerSpeed = -1.0f, flickerAmount = -1.0f;
        float distance = 0.0f;     // from the camera
        bool merged = false;
        uint64_t key = 0;          // which light it is from frame to frame (its model and row / own light)
        bool still = false;        // on a model hanging on nothing (a doodad, a unit): may get a shadow map once it stands
        uint32_t rowId = 0;        // its DoodadLightAssignment row; 0 a model's own light
        const std::string* path = nullptr; // its model file (s_files' copy: stable until the next frame's gathering)
    };

    std::unordered_set<uint32_t> s_missingRows;  // assignment ids whose attachment a model lacked (since SetTable)

    // A light's identity: its model and which of its lights (a table row, or 0x80000000 | its own index).
    uint64_t LightKey(const CM2Model* model, uint32_t which) {
        uint64_t k = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(model)) * 0x9E3779B97F4A7C15ull;
        k ^= (static_cast<uint64_t>(which) + 1) * 0xC2B2AE3D27D4EB4Full;
        return k ? k : 1;
    }

    std::vector<Found> s_found;
    std::vector<sxc_lights_emitter> s_emitters;   // the rows' emitters, placed by the last gathering
    uint32_t s_emitterStamp = 0;                  // the scene frame they were placed in (0: never)
    std::vector<GpuLight> s_lights;
    std::vector<float> s_strength;   // per light: its brightness before fade and flicker (full cells rank by it)
    std::vector<uint32_t> s_cellLights;
    std::vector<sxc_lights_active> s_active;   // per drawn light, for the editor
    Frame s_frame;

    constexpr int kMaxAttachDepth = 8;

    bool Settled() { return s_available && s_haveSettings && s_settings.enabled; }

    float Brightest(const C3Vector& c) { return std::fmax(c.x, std::fmax(c.y, c.z)); }

    // A direction through a matrix's rotation and scale (row vectors), and the length it came out with.
    C3Vector Dir(const C3Vector& v, const C44Matrix& m) {
        return C3Vector(v.x * m.a0 + v.y * m.b0 + v.z * m.c0, v.x * m.a1 + v.y * m.b1 + v.z * m.c1, v.x * m.a2 + v.y * m.b2 + v.z * m.c2);
    }

    float Length(const C3Vector& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

    // The first key of a track's first sequence, if it has one.
    template <class T>
    bool FirstKey(const M2Track<T>& track, T& out) {
        if (!track.m_sequenceKeys.m_count || !track.m_sequenceKeys[0].m_count) {
            return false;
        }

        out = track.m_sequenceKeys[0][0];
        return true;
    }

    bool Finite(const C3Vector& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

    // How a model puts a point of its own space into the world this frame: through a bone (it animated
    // this frame: its bone matrices are this frame's, view space), else its world matrix (a doodad out of
    // view: at rest). An attached model not animated has no position to trust.
    struct Placement {
        const CM2Scene* scene;
        const CM2Model* model;
        bool animated;
    };

    bool Place(const Placement& at, const C3Vector& local, uint32_t bone, const C3Vector* localDir, C3Vector& out, C3Vector* outDir, float& scale) {
        const M2Data* data = at.model->m_shared->m_data;

        if (at.animated && at.model->m_boneMatrices && bone < data->m_bones.m_count) {
            const C44Matrix& b = at.model->m_boneMatrices[bone];
            out = local * b * at.scene->m_viewInv;
            const C3Vector unit = Dir(Dir(C3Vector(1.0f, 0.0f, 0.0f), b), at.scene->m_viewInv);
            scale = Length(unit);

            if (localDir && outDir) {
                *outDir = Dir(Dir(*localDir, b), at.scene->m_viewInv);
            }
        } else if (!at.model->m_attachParent) {
            out = local * at.model->m_worldTransform;
            scale = Length(Dir(C3Vector(1.0f, 0.0f, 0.0f), at.model->m_worldTransform));

            if (localDir && outDir) {
                *outDir = Dir(*localDir, at.model->m_worldTransform);
            }
        } else {
            return false;
        }

        if (outDir) {
            const float len = Length(*outDir);
            *outDir = len > 1e-6f ? *outDir * (1.0f / len) : C3Vector(0.0f, 0.0f, -1.0f);
        }

        scale = scale > 1e-4f && std::isfinite(scale) ? scale : 1.0f;
        return Finite(out);
    }

    // An assignment's light and emitter on one model (`lights` false: the emitters' own scan, no lights).
    void AddRow(const Placement& at, const Row& row, uint64_t key, const std::string* path, const C3Vector& camera, float reach, bool lights) {
        const bool light = lights && row.kind >= 0;
        const bool emitter = row.a.emitter_id != 0;

        if (!light && !emitter) {
            return;
        }

        const M2Data* data = at.model->m_shared->m_data;
        const sxc_lights_assignment& a = row.a;
        C3Vector local(a.offset[0], a.offset[1], a.offset[2]);
        uint32_t bone = 0;  // the root bone for the origin (it carries the model's own movement)
        bool found = a.attach_type == SXC_LIGHTS_ATTACH_ORIGIN;

        if (a.attach_type == SXC_LIGHTS_ATTACH_POINT && a.attach_index >= 0 && static_cast<uint32_t>(a.attach_index) < data->m_attachmentIndicesById.m_count) {
            const uint16_t index = data->m_attachmentIndicesById[a.attach_index];

            if (index < data->m_attachments.m_count) {
                const M2Attachment& p = data->m_attachments[index];
                local = local + p.m_position;
                bone = p.m_bone;
                found = true;
            }
        } else if (a.attach_type == SXC_LIGHTS_ATTACH_BONE && a.attach_index >= 0 && static_cast<uint32_t>(a.attach_index) < data->m_bones.m_count) {
            local = local + data->m_bones[a.attach_index].m_pivot;
            bone = static_cast<uint32_t>(a.attach_index);
            found = true;
        } else if (a.attach_type == SXC_LIGHTS_ATTACH_EMITTER && a.attach_index >= 0 && static_cast<uint32_t>(a.attach_index) < data->m_particles.m_count) {
            const M2Particle& p = data->m_particles[a.attach_index];
            local = local + p.m_position;
            bone = p.m_boneIndex;
            found = true;
        }

        if (!found) {
            s_stats.attach_missing++;
            s_missingRows.insert(a.id);
            char key[64];
            std::snprintf(key, sizeof(key), "lights-attach|%u|%u", a.id, s_tableGeneration);
            static const char* const kTypes[] = { "origin", "attachment point", "bone", "particle emitter" };
            const uint32_t count = a.attach_type == SXC_LIGHTS_ATTACH_POINT ? data->m_attachmentIndicesById.m_count
                                 : a.attach_type == SXC_LIGHTS_ATTACH_BONE  ? data->m_bones.m_count
                                                                            : data->m_particles.m_count;
            SXC::LogOnce(key, "lights: DoodadSourceAssignment %u (%s): %s %d not on the model (it has %u%s); placed at its origin", a.id,
                         at.model->m_shared->m_fileName, a.attach_type < 4 ? kTypes[a.attach_type] : "unknown attachment type", a.attach_index, count,
                         a.attach_type == SXC_LIGHTS_ATTACH_POINT ? " attachment IDs" : "");
        }

        Found f;
        const C3Vector localDir(a.direction[0], a.direction[1], a.direction[2]);
        float scale = 1.0f;

        if (!Place(at, local, bone, &localDir, f.pos, &f.spotDir, scale)) {
            return;
        }

        const C3Vector d = f.pos - camera;
        f.distance = Length(d);

        if (f.distance > reach) {
            return;
        }

        // The emitter: placed only; what it does is the mod's.
        if (emitter) {
            sxc_lights_emitter e = {};
            e.row_id = a.id;
            e.emitter_id = a.emitter_id;
            std::memcpy(e.pos, &f.pos, sizeof e.pos);
            std::memcpy(e.direction, &f.spotDir, sizeof e.direction);
            e.scale = scale;
            e.distance = f.distance;
            e.model_path = path ? path->c_str() : "";
            s_emitters.push_back(e);
        }

        if (!light) {
            return;
        }

        const sxc_lights_kind& k = s_kinds[row.kind];
        f.color = C3Vector(k.color[0], k.color[1], k.color[2]);
        f.radius = k.radius > 0.0f ? k.radius * scale : 0.0f;
        f.falloff = k.falloff > 0.0f ? k.falloff : 1.0f;
        f.flickerMode = k.flicker_mode;
        f.flickerSpeed = k.flicker_speed;
        f.flickerAmount = k.flicker_amount;
        f.key = key;
        f.still = !at.model->m_attachParent;
        f.rowId = a.id;
        f.path = path;

        if (k.type == SXC_LIGHTS_SPOT) {
            constexpr float kRad = 3.14159265f / 180.0f;
            const float inner = std::cos(std::fmin(k.inner_angle, k.outer_angle) * kRad);
            f.cosOuter = std::cos(k.outer_angle * kRad);
            f.spotScale = inner - f.cosOuter > 1e-4f ? 1.0f / (inner - f.cosOuter) : 1e4f;
        }

        if (Brightest(f.color) > 0.0f) {
            s_found.push_back(f);
            s_stats.table_lights++;
        }
    }

    // A model's lights: its file's assignments, and its own point lights unless they're left out (where
    // the client placed them this frame, else the file's position through the model's world matrix; the
    // colour as the client last had it, else the file's first diffuse colour x intensity). Then the
    // models hanging on it (a torch in a hand).
    void Visit(const CM2Scene* scene, CM2Model* model, const C3Vector& camera, float reach, int depth, bool lights) {
        if (!model || depth > kMaxAttachDepth) {
            return;
        }

        s_stats.models++;
        // Only a loaded model: one still loading can have its data's header but not its arrays (a crash
        // reading an emitter's position, 2026-10-24).
        const M2Data* data = model->m_shared && model->IsLoaded(0, 0) ? model->m_shared->m_data : nullptr;

        if (data) {
            const Placement at{ scene, model, model->m_frameStamp == scene->m_frameStamp };
            const FileRows& fileRows = RowsFor(model->m_shared);
            const bool suppress = fileRows.suppress;
            s_stats.suppressed += suppress ? 1 : 0;

            if (fileRows.rows) {
                for (uint32_t r : *fileRows.rows) {
                    AddRow(at, s_rows[r], LightKey(model, r), &fileRows.name, camera, reach, lights);
                }
            }

            for (uint32_t i = 0; lights && model->m_lights && !suppress && i < data->m_lights.m_count; i++) {
                const M2Light& file = data->m_lights[i];
                const M2ModelLight& light = model->m_lights[i];

                if (file.m_type != 1 || !light.m_enabled) {
                    continue;
                }

                s_stats.model_lights++;
                const bool placed = light.m_light.m_stamp != 0 && light.m_light.m_stamp + 1 >= scene->m_frameStamp;
                Found f;
                float scale = 1.0f;

                if (placed) {
                    if (!light.m_light.m_visible) {
                        continue;
                    }

                    f.pos = light.m_light.m_pos;
                    s_stats.placed++;
                } else if (!Place(at, file.m_position, file.m_bone, nullptr, f.pos, nullptr, scale)) {
                    continue;
                }

                f.color = light.m_light.m_dirColor;

                if (Brightest(f.color) <= 0.0f) {
                    C3Vector c(0.0f, 0.0f, 0.0f);
                    float intensity = 1.0f;
                    FirstKey(file.m_diffuseColor, c);
                    FirstKey(file.m_diffuseIntensity, intensity);
                    f.color = c * intensity;
                }

                if (Brightest(f.color) <= 0.001f || !Finite(f.pos)) {
                    continue;
                }

                f.distance = Length(f.pos - camera);
                f.key = LightKey(model, 0x80000000u | i);
                f.still = !model->m_attachParent;
                f.path = &fileRows.name;

                if (f.distance <= reach) {
                    s_found.push_back(f);
                }
            }
        }

        for (CM2Model* child = model->m_attachList; child; child = child->m_attachNext) {
            Visit(scene, child, camera, reach, depth + 1, lights);
        }
    }

    // Point lights closer than the merge distance become one (Blizzard's torch groups): the colours
    // summed, the position weighted by them, the larger radius. Spots never merge (their cones differ).
    // Neighbours through a hash of merge-distance cells (a city holds thousands of lights: comparing every
    // pair cost hundreds of milliseconds in WXL, Dalaran 2026-10-08).
    void Merge(float distance) {
        if (distance <= 1e-3f) {
            return;
        }

        const float inv = 1.0f / distance, d2 = distance * distance;
        auto key = [inv](const C3Vector& p, int dx, int dy, int dz) {
            const int64_t x = static_cast<int64_t>(std::floor(p.x * inv)) + dx, y = static_cast<int64_t>(std::floor(p.y * inv)) + dy,
                          z = static_cast<int64_t>(std::floor(p.z * inv)) + dz;
            return (static_cast<uint64_t>(x + 1048576) & 0x1FFFFF) | ((static_cast<uint64_t>(y + 1048576) & 0x1FFFFF) << 21) |
                   ((static_cast<uint64_t>(z + 1048576) & 0x1FFFFF) << 42);
        };
        static std::vector<std::pair<uint64_t, uint32_t>> cells;
        static std::vector<C3Vector> original;
        cells.clear();
        original.resize(s_found.size());

        for (uint32_t k = 0; k < s_found.size(); k++) {
            original[k] = s_found[k].pos;

            if (s_found[k].cosOuter < -1.5f) {
                cells.emplace_back(key(s_found[k].pos, 0, 0, 0), k);
            }
        }

        std::sort(cells.begin(), cells.end());

        for (uint32_t a = 0; a < s_found.size(); a++) {
            Found& keep = s_found[a];

            if (keep.merged || keep.cosOuter > -1.5f) {
                continue;
            }

            float wa = keep.color.x + keep.color.y + keep.color.z;

            for (int n = 0; n < 27; n++) {
                const uint64_t k = key(original[a], n % 3 - 1, (n / 3) % 3 - 1, n / 9 - 1);

                for (auto it = std::lower_bound(cells.begin(), cells.end(), std::make_pair(k, 0u)); it != cells.end() && it->first == k; ++it) {
                    const uint32_t b = it->second;
                    Found& other = s_found[b];

                    if (b <= a || other.merged) {
                        continue;
                    }

                    const C3Vector d = original[a] - original[b];

                    if (d.x * d.x + d.y * d.y + d.z * d.z > d2) {
                        continue;
                    }

                    const float wb = other.color.x + other.color.y + other.color.z;
                    const float share = wa + wb > 0.0f ? wb / (wa + wb) : 0.5f;
                    keep.pos = keep.pos + (other.pos - keep.pos) * share;
                    keep.color = keep.color + other.color;
                    keep.radius = std::fmax(keep.radius, other.radius);
                    keep.distance = std::fmin(keep.distance, other.distance);
                    keep.still = keep.still && other.still;
                    wa += wb;
                    other.merged = true;
                    s_stats.merged++;
                }
            }
        }
    }

    uint32_t Hash(uint32_t x) {
        x ^= x >> 16;
        x *= 0x7FEB352Du;
        x ^= x >> 15;
        x *= 0x846CA68Bu;
        x ^= x >> 16;
        return x;
    }

    float Unit(uint32_t x) { return static_cast<float>(Hash(x) >> 8) * (1.0f / 16777216.0f); }

    // How much a flame dips now, 0..1; each light its own phase from its position, so neighbouring
    // torches don't pulse together (WXL's). Smooth: three sines at unrelated rates. Noise: random levels
    // ~6 a second, eased into each other. Steps: the same levels, held.
    float Flicker(int mode, const C3Vector& pos, double time) {
        const double phase = std::fmod(std::fabs(pos.x * 12.9898 + pos.y * 78.233 + pos.z * 37.719), 1000.0);
        const double t = time + phase;

        if (mode == SXC_LIGHTS_FLICKER_OFF) {
            return 0.0f;
        }

        if (mode == SXC_LIGHTS_FLICKER_NOISE || mode == SXC_LIGHTS_FLICKER_STEPS) {
            const double u = t * 6.0;
            const uint32_t seed = static_cast<uint32_t>(phase * 1000.0);
            const uint32_t k = static_cast<uint32_t>(static_cast<int64_t>(std::floor(u)));
            const float a = Unit(k * 2654435761u + seed), b = Unit((k + 1) * 2654435761u + seed);

            if (mode == SXC_LIGHTS_FLICKER_STEPS) {
                return a;
            }

            float f = static_cast<float>(u - std::floor(u));
            f = f * f * (3.0f - 2.0f * f);
            return a + (b - a) * f;
        }

        const double s = 0.5 * std::sin(t * 7.13) + 0.3 * std::sin(t * 13.71 + 1.3) + 0.2 * std::sin(t * 23.17 + 2.1);
        return static_cast<float>(0.5 + 0.5 * s);
    }

    // The grid snaps to whole cells around the camera. A light goes into every cell its radius reaches
    // (circle against square); a cell holding more than perCell keeps the strongest at its middle (the
    // light's base brightness x (1 - d / reach)^2: no flicker or fade, so the choice doesn't flip).
    void BuildGrid(const C3Vector& camera, float range, float cell, uint32_t perCell) {
        const int half = std::min(static_cast<int>(std::ceil(range / cell)), static_cast<int>(kMaxCells / 2));
        const int cells = std::max(1, half * 2);
        const float ox = (std::floor(camera.x / cell) - half) * cell, oy = (std::floor(camera.y / cell) - half) * cell;
        s_frame.originX = ox;
        s_frame.originY = oy;
        s_frame.cellSize = cell;
        s_frame.cells = static_cast<uint32_t>(cells);
        s_frame.perCell = perCell;
        s_cellLights.assign(static_cast<size_t>(cells) * cells * perCell, kNoLight);
        s_stats.cells = static_cast<uint32_t>(cells);

        struct Entry {
            uint32_t cell;
            float score;
            uint32_t light;
        };
        static std::vector<Entry> entries;
        entries.clear();
        const float halfDiagonal = cell * 0.70710678f;

        for (uint32_t i = 0; i < s_lights.size(); i++) {
            const GpuLight& l = s_lights[i];
            const float r = 1.0f / l.invRadius, lx = l.pos[0] - ox, ly = l.pos[1] - oy;
            const int x0 = std::max(0, static_cast<int>(std::floor((lx - r) / cell))), y0 = std::max(0, static_cast<int>(std::floor((ly - r) / cell)));
            const int x1 = std::min(cells - 1, static_cast<int>(std::floor((lx + r) / cell))), y1 = std::min(cells - 1, static_cast<int>(std::floor((ly + r) / cell)));

            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    const float nx = std::fmax(x * cell, std::fmin(lx, (x + 1) * cell)) - lx, ny = std::fmax(y * cell, std::fmin(ly, (y + 1) * cell)) - ly;

                    if (nx * nx + ny * ny > r * r) {
                        continue;
                    }

                    const float cx = (x + 0.5f) * cell - lx, cy = (y + 0.5f) * cell - ly;
                    const float t = 1.0f - std::fmin(std::sqrt(cx * cx + cy * cy) / (r + halfDiagonal), 1.0f);
                    entries.push_back(Entry{ static_cast<uint32_t>(y * cells + x), s_strength[i] * t * t, i });
                }
            }
        }

        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.cell != b.cell ? a.cell < b.cell : (a.score != b.score ? a.score > b.score : a.light < b.light);
        });

        for (size_t e = 0; e < entries.size();) {
            const uint32_t c = entries[e].cell;
            uint32_t n = 0;

            for (; e < entries.size() && entries[e].cell == c; e++, n++) {
                if (n < perCell) {
                    s_cellLights[static_cast<size_t>(c) * perCell + n] = entries[e].light;
                }
            }

            s_stats.busiest_cell = std::max(s_stats.busiest_cell, n);
            s_stats.full_cells += n > perCell ? 1 : 0;
        }
    }

    // --- inside or outside ---------------------------------------------------------------------------
    // Whether a point lies in an indoor WMO group: the client's own viewer locate, as CWorldScene::
    // LocateViewer3 calls it (a segment straight down, cut where it meets the terrain; true: the first
    // group below is an interior one). Cached per half yard; answers expire after 5-7 s, and at most
    // kTestsPerFrame new tests run per frame (the old answer, or outside, stands past that). WXL's
    // lights::IsIndoor.
    struct IndoorAnswer {
        bool indoor = false;
        uint32_t expires = 0;
    };
    std::unordered_map<uint64_t, IndoorAnswer> s_indoor;
    constexpr uint32_t kTestsPerFrame = 48;

    // `budgeted`: the lights' 48 a frame apply (the models count their own).
    bool IsIndoor(const C3Vector& p, uint32_t nowMs, bool budgeted = true) {
        const auto q = [](float v) { return static_cast<uint64_t>(static_cast<int64_t>(std::floor(v * 2.0f)) + 1048576) & 0x1FFFFF; };
        const uint64_t key = q(p.x) | (q(p.y) << 21) | (q(p.z) << 42);
        auto it = s_indoor.find(key);

        if (it != s_indoor.end() && it->second.expires > nowMs) {
            return it->second.indoor;
        }

        if (budgeted && s_stats.indoor_tests >= kTestsPerFrame) {
            return it != s_indoor.end() && it->second.indoor;
        }

        s_stats.indoor_tests += budgeted ? 1 : 0;

        if (s_indoor.size() > 16384) {
            s_indoor.clear();
        }

        C3Vector start = p;
        C3Vector end(p.x, p.y, p.z - 1760.0f);
        float dist = 1.0f;
        CMapChunk* chunk = nullptr;
        CMap::VectorIntersectTerrain(&start, &end, &dist, 0x100, &chunk);
        CMapObjDef* defs[2] = {};
        uint32_t groups[4] = {};
        IndoorAnswer& a = s_indoor[key];
        a.indoor = CMap::LocateViewerMapObjs(start, end, dist, defs, groups);
        a.expires = nowMs + 5000 + static_cast<uint32_t>(key % 2000);
        return a.indoor;
    }

    int s_groupDepth = 0;
    bool s_groupIndoor = false;

    // A model's side, located at its root's origin (a little above it: the floor it stands on is below),
    // cached per model; re-located when it moved more than kMoved yd, or after 3-5 s; at most
    // kModelTestsPerFrame a frame (a model not located yet takes every light: as before the test).
    struct ModelAnswer {
        C3Vector at;
        Surface surface = kSurfaceAny;
        uint32_t expires = 0;
    };
    std::unordered_map<const CM2Model*, ModelAnswer> s_models;
    constexpr uint32_t kModelTestsPerFrame = 128;
    constexpr float kMoved = 1.0f;
    const CM2Model* s_drawModel = nullptr;

    C3Vector RootOrigin(const CM2Model* model) {
        int guard = 0;

        while (model->m_attachParent && guard++ < 16) {
            model = model->m_attachParent;
        }

        return C3Vector(model->m_worldTransform.d0, model->m_worldTransform.d1, model->m_worldTransform.d2);
    }

    // --- shadows ----------------------------------------------------------------------------------
    // The nearest lights that stand still get a cube shadow map (the device's Vulkan/Shadows): the WMO
    // and terrain triangles within their radius, gathered here from the client's own data (each loaded
    // WMO group's triangles through its placement; the terrain chunks' heights, holes left open) and
    // drawn once into a slot; kept while the light stays within kShadowMoved of where it was drawn and
    // its surroundings don't change (a signature of the groups and chunks it reaches, rechecked every
    // few seconds: a WMO group or a chunk finishing loading). WMO faces that only collide (material
    // 0xFF) or are see-through (blended materials: windows) cast nothing. Models cast nothing.
    std::vector<uint32_t> s_lightFound;   // per drawn light: its index in s_found

    uint32_t s_shadowSlotCount = 0;       // the device's
    uint32_t s_shadowMaxVertices = 0;

    struct ShadowSlot {
        uint64_t key = 0;
        C3Vector pos;
        float radius = 0.0f;
        uint32_t signature = 0;
        bool valid = false;        // drawn for this key, position and radius
        uint32_t usedFrame = 0;
        uint32_t checkMs = 0;      // when to look at its surroundings again
        uint32_t incomplete = 0;   // drawn with textures missing, in a row (the next look waits longer each time)
    };
    std::vector<ShadowSlot> s_shadowSlots;
    uint32_t s_shadowFrame = 0;

    // Where each keyed light has been standing, and since when (a light that moves gets no map until it
    // has stood kShadowSettleMs within kShadowMoved of one place).
    struct Anchor {
        C3Vector at;
        uint32_t sinceMs = 0;
        uint32_t seenMs = 0;
    };
    std::unordered_map<uint64_t, Anchor> s_anchors;

    constexpr float kShadowMoved = 0.35f;
    constexpr uint32_t kShadowSettleMs = 1000;
    constexpr uint32_t kShadowChecksPerFrame = 8;
    constexpr uint32_t kShadowMaxPerLight = 256 * 1024;   // vertices
    constexpr float kShadowKeepBonus = 4.0f;              // yd nearer a light that has its map counts as (no flapping at the edge)
    constexpr double kShadowGatherBudgetMs = 2.0;         // a frame's gathering (and checks) before the rest wait

    std::vector<float> s_shadowVertices;
    std::vector<ShadowJob> s_shadowJobs;
    std::vector<ShadowPart> s_shadowParts;
    std::vector<uint32_t> s_shadowJobSlots;

    // An axis-aligned box against a sphere.
    bool BoxReaches(const CAaBox& b, const C3Vector& c, float r) {
        const float dx = c.x < b.b.x ? b.b.x - c.x : (c.x > b.t.x ? c.x - b.t.x : 0.0f);
        const float dy = c.y < b.b.y ? b.b.y - c.y : (c.y > b.t.y ? c.y - b.t.y : 0.0f);
        const float dz = c.z < b.b.z ? b.b.z - c.z : (c.z > b.t.z ? c.z - b.t.z : 0.0f);
        return dx * dx + dy * dy + dz * dz <= r * r;
    }

    // A triangle's box against a sphere (conservative).
    bool TriangleReaches(const C3Vector& a, const C3Vector& b, const C3Vector& c, const C3Vector& centre, float r) {
        CAaBox box;
        box.b = C3Vector(std::fmin(a.x, std::fmin(b.x, c.x)), std::fmin(a.y, std::fmin(b.y, c.y)), std::fmin(a.z, std::fmin(b.z, c.z)));
        box.t = C3Vector(std::fmax(a.x, std::fmax(b.x, c.x)), std::fmax(a.y, std::fmax(b.y, c.y)), std::fmax(a.z, std::fmax(b.z, c.z)));
        return BoxReaches(box, centre, r);
    }

    uint32_t Mix(uint32_t h, uint64_t v) {
        for (int i = 0; i < 8; i++, v >>= 8) {
            h = (h ^ static_cast<uint32_t>(v & 0xFF)) * 16777619u;
        }

        return h;
    }

    struct Gathered {
        std::vector<float>* out = nullptr;   // null: the signature only; else 5 floats a vertex (xyz, uv)
        std::vector<ShadowPart>* parts = nullptr; // the job's runs (from partBase on), with out
        size_t partBase = 0;
        C3Vector centre;
        float radius = 0.0f;
        uint32_t limit = 0;                  // vertices at most (out's size may grow by that much)
        uint32_t added = 0;
        bool truncated = false;
        std::unordered_set<const void*>* doodads = nullptr;  // the doodads taken so far (linked from several chunks)
        uint32_t doodadCount = 0;

        // A triangle; with a texture, alpha-keyed by it at the texture coordinates `uv`.
        void Add(const C3Vector& a, const C3Vector& b, const C3Vector& c, CGxTex* texture = nullptr, const C2Vector* uv = nullptr) {
            if (added + 3 > limit) {
                truncated = true;
                return;
            }

            if (parts->size() == partBase || parts->back().texture != texture) {
                parts->push_back(ShadowPart{ added, 0, texture });
            }

            const C3Vector v[3] = { a - centre, b - centre, c - centre };

            for (int k = 0; k < 3; k++) {
                out->push_back(v[k].x);
                out->push_back(v[k].y);
                out->push_back(v[k].z);
                out->push_back(uv ? uv[k].x : 0.0f);
                out->push_back(uv ? uv[k].y : 0.0f);
            }

            parts->back().vertexCount += 3;
            added += 3;
        }
    };

    bool Holds(const CAaBox& b, const C3Vector& p, float margin) {
        return p.x >= b.b.x - margin && p.x <= b.t.x + margin && p.y >= b.b.y - margin && p.y <= b.t.y + margin && p.z >= b.b.z - margin && p.z <= b.t.z + margin;
    }

    // A placed doodad's solid, alpha-keyed and alpha-blended batches (additive ones: glows, flames cast nothing), at
    // rest through its placement. Its alpha-keyed textures' readiness goes into the signature (a map drawn
    // before they loaded is drawn again).
    uint32_t AddModel(Gathered& g, CM2Model* model, const C44Matrix& placement, uint32_t h) {
        const M2Data* data = model->m_shared->m_data;
        const M2SkinProfile* skin = model->m_shared->m_skinData;
        static std::vector<C3Vector> world;
        static std::vector<uint8_t> sectionDone;
        world.clear();
        sectionDone.assign(skin->m_skinSections.m_count, 0);

        if (g.out) {
            world.resize(skin->m_vertices.m_count);

            for (uint32_t v = 0; v < skin->m_vertices.m_count; v++) {
                const uint16_t global = skin->m_vertices[v];
                world[v] = global < data->m_vertices.m_count ? data->m_vertices[global].m_position * placement : C3Vector(0.0f, 0.0f, 0.0f);
            }
        }

        for (uint32_t b = 0; b < skin->m_batches.m_count; b++) {
            const M2Batch& batch = skin->m_batches[b];

            if (batch.m_materialIndex >= data->m_materials.m_count || batch.m_skinSectionIndex >= skin->m_skinSections.m_count) {
                continue;
            }

            const uint16_t blend = data->m_materials[batch.m_materialIndex].m_blendMode;

            // Additive and modulating parts (glows, flames) cast nothing; alpha-blended ones (fence panes, nets;
            // glass is mostly under the cut) cast where their texture is at least half opaque, as alpha-keyed ones.
            if (blend >= 3 || sectionDone[batch.m_skinSectionIndex]) {
                continue;
            }

            CGxTex* texture = nullptr;

            if (blend == 1 || blend == 2) {
                const uint32_t combo = batch.m_textureComboIndex < data->m_textureCombos.m_count ? data->m_textureCombos[batch.m_textureComboIndex] : 0xFFFF;
                CTexture* t = combo < data->m_textures.m_count && model->m_textures ? model->m_textures[combo] : nullptr;
                texture = t ? TextureGetGxTex(t, 0, nullptr) : nullptr;
                h = Mix(h, texture ? 5 : 6);

                if (!texture) {
                    continue;   // not loaded yet: nothing rather than a solid blob
                }
            }

            sectionDone[batch.m_skinSectionIndex] = 1;

            if (!g.out) {
                continue;
            }

            const M2SkinSection& section = skin->m_skinSections[batch.m_skinSectionIndex];
            const uint32_t end = std::min<uint32_t>(section.m_indexStart + section.m_indexCount, skin->m_indices.m_count);

            for (uint32_t k = section.m_indexStart; k + 2 < end; k += 3) {
                const uint16_t i0 = skin->m_indices[k], i1 = skin->m_indices[k + 1], i2 = skin->m_indices[k + 2];

                if (i0 >= world.size() || i1 >= world.size() || i2 >= world.size() || !TriangleReaches(world[i0], world[i1], world[i2], g.centre, g.radius)) {
                    continue;
                }

                if (texture) {
                    const C2Vector uv[3] = { data->m_vertices[skin->m_vertices[i0]].m_texcoord[0], data->m_vertices[skin->m_vertices[i1]].m_texcoord[0],
                                             data->m_vertices[skin->m_vertices[i2]].m_texcoord[0] };
                    g.Add(world[i0], world[i1], world[i2], texture, uv);
                } else {
                    g.Add(world[i0], world[i1], world[i2]);
                }
            }
        }

        return h;
    }

    void GatherDoodad(Gathered& g, const CMapBaseObjLink* link, uint32_t& h);

    // Game objects the server spawned (chairs, braziers, mailboxes, lamp posts: in no chunk's or WMO group's
    // doodad list), collected once per frame for the shadow maps: the model and its box in the world.
    // Doors, transports and destructible buildings move or are WMOs: left out.
    struct CasterObject {
        CM2Model* model;
        CAaBox box;
        uint64_t guid;
    };
    std::vector<CasterObject> s_objects;

    int32_t CollectObject(WOWGUID guid, void* param) {
        CGObject_C* object = ClntObjMgrObjectPtr(guid, TYPEMASK_GAMEOBJECT, __FILE__, __LINE__);

        if (!object) {
            return 1;
        }

        const CGGameObjectData* data = static_cast<CGGameObject_C*>(object)->m_gameObject;
        const int type = data ? data->m_typeID : -1;

        if (type < 0 || type == 0 || type == 11 || type == 15 || type == 33) {  // door, transport, map object transport, destructible building
            return 1;
        }

        CM2Model* model = object->GetObjectModel();

        if (!model || !model->m_shared || !model->m_shared->m_data) {
            return 1;
        }

        const CAaBox& e = model->m_shared->m_data->m_bounds.m_extent;
        CasterObject o;
        o.model = model;
        o.guid = guid.m_guid;
        o.box.b = C3Vector(1e30f, 1e30f, 1e30f);
        o.box.t = C3Vector(-1e30f, -1e30f, -1e30f);

        for (int k = 0; k < 8; k++) {
            const C3Vector corner = C3Vector(k & 1 ? e.t.x : e.b.x, k & 2 ? e.t.y : e.b.y, k & 4 ? e.t.z : e.b.z) * model->m_worldTransform;
            o.box.b = C3Vector(std::fmin(o.box.b.x, corner.x), std::fmin(o.box.b.y, corner.y), std::fmin(o.box.b.z, corner.z));
            o.box.t = C3Vector(std::fmax(o.box.t.x, corner.x), std::fmax(o.box.t.y, corner.y), std::fmax(o.box.t.z, corner.z));
        }

        static_cast<std::vector<CasterObject>*>(param)->push_back(o);
        return 1;
    }

    // What a light's map is drawn from (into g.out when set); the surroundings' signature either way.
    uint32_t Gather(Gathered& g) {
        uint32_t h = 2166136261u;
        static std::vector<C3Vector> world;
        static std::unordered_set<const void*> doodads;
        doodads.clear();
        g.doodads = &doodads;
        const bool withDoodads = s_settings.shadow_doodads != WXL_FALSE;

        for (CMapObjDef* def = CMap::mapObjDefHash.m_fulllist.Head(); def; def = CMap::mapObjDefHash.m_fulllist.Next(def)) {
            if ((static_cast<uint32_t>(def->m_flags) & 0x20) || !BoxReaches(def->m_bbox, g.centre, g.radius)) {
                continue;
            }

            CMapObj* obj = def->m_owner;
            h = Mix(h, reinterpret_cast<uintptr_t>(def));

            if (!obj || !obj->m_isGroupLoaded) {
                h = Mix(h, 1);
                continue;
            }

            for (CMapBaseObjLink* link = def->m_mapObjDefGroupLinkList.Head(); link; link = def->m_mapObjDefGroupLinkList.Next(link)) {
                CMapObjDefGroup* defGroup = static_cast<CMapObjDefGroup*>(link->m_owner);

                if (!defGroup || !BoxReaches(defGroup->m_bbox, g.centre, g.radius)) {
                    continue;
                }

                for (CMapBaseObjLink* d = withDoodads ? defGroup->m_doodadDefLinkList.Head() : nullptr; d; d = defGroup->m_doodadDefLinkList.Next(d)) {
                    GatherDoodad(g, d, h);
                }

                CMapObjGroup* group = obj->GetGroup(defGroup->m_groupNum, 0);
                h = Mix(h, group ? reinterpret_cast<uintptr_t>(group) : 2);

                if (!g.out || !group || !group->m_vertexList || !group->m_indices || !group->m_polyList || group->m_vertexListCount <= 0) {
                    continue;
                }

                world.resize(static_cast<size_t>(group->m_vertexListCount));

                for (int32_t v = 0; v < group->m_vertexListCount; v++) {
                    world[v] = group->m_vertexList[v] * def->m_mat;
                }

                const int32_t triangles = std::min(group->m_indicesCount / 3, group->m_polyListSize);

                for (int32_t t = 0; t < triangles; t++) {
                    const SMOPoly& poly = group->m_polyList[t];

                    if (poly.m_materialId == 0xFF || (obj->m_materialList && obj->m_materialList[poly.m_materialId].m_blendMode >= 2)) {
                        continue;
                    }

                    const uint16_t* i = &group->m_indices[t * 3];

                    if (i[0] >= world.size() || i[1] >= world.size() || i[2] >= world.size()) {
                        continue;
                    }

                    if (TriangleReaches(world[i[0]], world[i[1]], world[i[2]], g.centre, g.radius)) {
                        g.Add(world[i[0]], world[i[1]], world[i[2]]);
                    }
                }
            }
        }

        for (CMapChunk* chunk = CMap::s_mapChunkList.Head(); chunk; chunk = CMap::s_mapChunkList.Next(chunk)) {
            if (!chunk->m_height || !chunk->m_header) {
                continue;
            }

            // Its box's x and y are its place on the map; its heights may not be in z yet.
            CAaBox box = chunk->m_bbox;

            if (!(box.b.z <= box.t.z)) {
                box.b.z = -1e9f;
                box.t.z = 1e9f;
            }

            if (!BoxReaches(box, g.centre, g.radius)) {
                continue;
            }

            h = Mix(h, reinterpret_cast<uintptr_t>(chunk));

            for (CMapBaseObjLink* d = withDoodads ? chunk->m_doodadDefLinkList.Head() : nullptr; d; d = chunk->m_doodadDefLinkList.Next(d)) {
                GatherDoodad(g, d, h);
            }

            if (!g.out) {
                continue;
            }

            const uint32_t holes = chunk->m_header->m_holes;

            // 9 x 9 outer vertices with 8 x 8 inner ones between them (rows of 9 then 8): each cell is
            // four triangles around its inner vertex.
            for (int32_t y = 0; y < 8; y++) {
                for (int32_t x = 0; x < 8; x++) {
                    if (holes & CMap::s_holeMask[4 * (y >> 1) + (x >> 1)]) {
                        continue;
                    }

                    C3Vector a, b, c, d, e;
                    chunk->GetVertexPosition(17 * y + x, &a);
                    chunk->GetVertexPosition(17 * y + x + 1, &b);
                    chunk->GetVertexPosition(17 * y + 9 + x, &c);
                    chunk->GetVertexPosition(17 * (y + 1) + x, &d);
                    chunk->GetVertexPosition(17 * (y + 1) + x + 1, &e);
                    const C3Vector* tris[4][3] = { { &a, &b, &c }, { &b, &e, &c }, { &e, &d, &c }, { &d, &a, &c } };

                    for (const auto& t : tris) {
                        if (TriangleReaches(*t[0], *t[1], *t[2], g.centre, g.radius)) {
                            g.Add(*t[0], *t[1], *t[2]);
                        }
                    }
                }
            }
        }

        // The game objects (this frame's, collected by AssignShadows), as the doodads: where one stands goes
        // into the signature too (one put down or moved: the map again).
        for (const CasterObject& o : s_objects) {   // empty without doodads casting
            if (!BoxReaches(o.box, g.centre, g.radius) || Holds(o.box, g.centre, 0.1f)) {
                continue;
            }

            CM2Model* model = o.model;
            const bool ready = model->m_shared && model->m_shared->m_data && model->m_shared->m_skinData && model->IsLoaded(0, 0);
            const C44Matrix& at = model->m_worldTransform;
            h = Mix(h, o.guid);
            h = Mix(h, ready ? 3 : 4);
            h = Mix(h, (static_cast<uint64_t>(static_cast<int64_t>(at.d0 * 4.0f)) << 42) ^ (static_cast<uint64_t>(static_cast<int64_t>(at.d1 * 4.0f)) << 21)
                           ^ static_cast<uint64_t>(static_cast<int64_t>(at.d2 * 4.0f)));

            if (ready) {
                g.doodadCount++;
                h = AddModel(g, model, at, h);
            }
        }

        return h;
    }

    // A placed doodad (linked from the terrain chunks it stands on and the WMO groups it is in: units, game
    // objects and held items never are), once per gathering, except the light's own: a lantern's or a
    // torch's box holds its light. (CMap::doodadDefList is only the queue of doodads still loading.)
    void GatherDoodad(Gathered& g, const CMapBaseObjLink* link, uint32_t& h) {
        CMapDoodadDef* def = static_cast<CMapDoodadDef*>(link->m_owner);

        if (!def || !g.doodads->insert(def).second) {
            return;
        }

        CM2Model* model = def->m_model;

        if (!model || !BoxReaches(def->m_bbox, g.centre, g.radius) || Holds(def->m_bbox, g.centre, 0.1f)) {
            return;
        }

        const bool ready = model->m_shared && model->m_shared->m_data && model->m_shared->m_skinData && model->IsLoaded(0, 0);
        h = Mix(h, reinterpret_cast<uintptr_t>(def));
        h = Mix(h, ready ? 3 : 4);

        if (ready) {
            g.doodadCount++;
            h = AddModel(g, model, def->m_mat, h);
        }
    }

    // Which drawn lights get a shadow map this frame, and which maps are drawn. Lights in s_lights,
    // s_lightFound filled; sets GpuLight::shadow, fills s_shadowJobs.
    void AssignShadows(uint32_t nowMs) {
        s_shadowJobs.clear();
        s_shadowJobSlots.clear();
        s_shadowVertices.clear();
        s_shadowParts.clear();
        s_stats.shadow_slots = s_shadowSlotCount;
        s_shadowFrame++;

        if (!s_settings.shadows || !s_shadowSlotCount) {
            return;
        }

        if (s_shadowSlots.size() != s_shadowSlotCount) {
            s_shadowSlots.assign(s_shadowSlotCount, ShadowSlot{});
        }

        // The game objects that cast, for this frame's gatherings.
        s_objects.clear();

        if (s_settings.shadow_doodads != WXL_FALSE) {
            ClntObjMgrEnumVisibleObjects(&CollectObject, &s_objects);
        }

        // Where each keyed light stands.
        if (s_anchors.size() > 4096) {
            for (auto it = s_anchors.begin(); it != s_anchors.end();) {
                it = nowMs - it->second.seenMs > 10000 ? s_anchors.erase(it) : std::next(it);
            }
        }

        struct Candidate {
            float score;
            uint32_t light;
        };
        static std::vector<Candidate> candidates;
        candidates.clear();
        static std::unordered_map<uint64_t, uint32_t> slotOf;
        slotOf.clear();

        for (uint32_t s = 0; s < s_shadowSlots.size(); s++) {
            if (s_shadowSlots[s].key) {
                slotOf[s_shadowSlots[s].key] = s;
            }
        }

        for (uint32_t i = 0; i < s_lights.size(); i++) {
            const Found& f = s_found[s_lightFound[i]];

            if (!f.key || !f.still) {
                continue;
            }

            Anchor& a = s_anchors[f.key];
            const C3Vector moved = f.pos - a.at;

            if (!a.seenMs || moved.x * moved.x + moved.y * moved.y + moved.z * moved.z > kShadowMoved * kShadowMoved) {
                a.at = f.pos;
                a.sinceMs = nowMs;
            }

            a.seenMs = nowMs;

            if (nowMs - a.sinceMs < kShadowSettleMs) {
                s_stats.shadow_waiting++;
                continue;
            }

            const auto held = slotOf.find(f.key);
            const bool hasMap = held != slotOf.end() && s_shadowSlots[held->second].valid;
            candidates.push_back(Candidate{ f.distance - (hasMap ? kShadowKeepBonus : 0.0f), i });
        }

        const uint32_t wanted = std::min(static_cast<uint32_t>(std::max(s_settings.shadow_count, 0)), s_shadowSlotCount);

        if (candidates.size() > wanted) {
            std::nth_element(candidates.begin(), candidates.begin() + wanted, candidates.end(),
                             [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
            candidates.resize(wanted);
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score < b.score; });

        // The chosen keep their slots this frame (marked first, so a newcomer never takes one of theirs).
        for (const Candidate& c : candidates) {
            const auto held = slotOf.find(s_found[s_lightFound[c.light]].key);

            if (held != slotOf.end()) {
                s_shadowSlots[held->second].usedFrame = s_shadowFrame;
            }
        }

        const uint32_t updates = static_cast<uint32_t>(std::max(s_settings.shadow_updates, 0));
        uint32_t checks = 0;
        const auto gatherStart = std::chrono::steady_clock::now();
        std::vector<std::pair<size_t, uint32_t>> ranges; // per job: first float, vertex count
        std::vector<size_t> partRanges;                  // per job: its first part

        for (const Candidate& c : candidates) {
            GpuLight& light = s_lights[c.light];
            const Found& f = s_found[s_lightFound[c.light]];
            const float radius = 1.0f / light.invRadius;
            const auto held = slotOf.find(f.key);
            uint32_t slot = 0;
            bool draw = false, usable = false;

            if (held != slotOf.end()) {
                slot = held->second;
                ShadowSlot& s = s_shadowSlots[slot];
                const C3Vector d = f.pos - s.pos;
                const bool moved = d.x * d.x + d.y * d.y + d.z * d.z > kShadowMoved * kShadowMoved || std::fabs(s.radius - radius) > 0.05f * radius;
                usable = s.valid && !moved;
                draw = !usable;

                // Its surroundings, now and then (an old map still serves while a new one waits).
                if (usable && nowMs >= s.checkMs && checks < kShadowChecksPerFrame) {
                    checks++;
                    s.checkMs = nowMs + 2000 + static_cast<uint32_t>(f.key % 1000);
                    Gathered sig;
                    sig.centre = s.pos;
                    sig.radius = s.radius;
                    SXC::Profiler::Scope p(SXC::Profiler::kShadowChecks);
                    draw = Gather(sig) != s.signature;

                    if (!draw) {
                        s.incomplete = 0;   // the last map came out whole
                    }
                }
            } else {
                // A free slot, else the one unused longest (never one chosen this frame).
                uint32_t best = UINT32_MAX;

                for (uint32_t s = 0; s < s_shadowSlots.size(); s++) {
                    const ShadowSlot& o = s_shadowSlots[s];

                    if (o.usedFrame == s_shadowFrame) {
                        continue;
                    }

                    if (best == UINT32_MAX || !o.key || (s_shadowSlots[best].key && o.usedFrame < s_shadowSlots[best].usedFrame)) {
                        best = s;

                        if (!o.key) {
                            break;
                        }
                    }
                }

                if (best == UINT32_MAX) {
                    continue;
                }

                slot = best;
                ShadowSlot& s = s_shadowSlots[slot];
                slotOf.erase(s.key);
                s = ShadowSlot{};
                s.key = f.key;
                slotOf[f.key] = slot;
                draw = true;
            }

            ShadowSlot& s = s_shadowSlots[slot];
            s.usedFrame = s_shadowFrame;

            // A frame gathers for kShadowGatherBudgetMs (always one map): the rest wait for the next frames,
            // the old maps (or none) meanwhile.
            const bool inBudget = s_shadowJobs.empty() || std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gatherStart).count() < kShadowGatherBudgetMs;

            if (draw && inBudget && s_shadowJobs.size() < updates && s_shadowVertices.size() / 5 < s_shadowMaxVertices) {
                Gathered g;
                g.out = &s_shadowVertices;
                g.centre = f.pos;
                g.radius = radius;
                g.limit = std::min(kShadowMaxPerLight, s_shadowMaxVertices - static_cast<uint32_t>(s_shadowVertices.size() / 5));
                g.parts = &s_shadowParts;
                g.partBase = s_shadowParts.size();
                const size_t first = s_shadowVertices.size();
                SXC::Profiler::Enter(SXC::Profiler::kShadowGather);
                s.signature = Gather(g);
                SXC::Profiler::Leave(SXC::Profiler::kShadowGather);

                if (g.truncated) {
                    SXC::LogOnce("lights-shadow-full", "lights: a shadow map was cut short at %u triangles (light at %.1f %.1f %.1f, radius %.1f)", g.added / 3, f.pos.x,
                                 f.pos.y, f.pos.z, radius);
                }

                s.pos = f.pos;
                s.radius = radius;
                s.valid = true;
                s.checkMs = nowMs + 2000 + static_cast<uint32_t>(f.key % 1000);
                s_shadowJobs.push_back(ShadowJob{ slot, radius, nullptr, g.added, nullptr, static_cast<uint32_t>(s_shadowParts.size() - g.partBase) });
                s_shadowJobSlots.push_back(slot);
                ranges.emplace_back(first, g.added);
                partRanges.push_back(g.partBase);
                s_stats.shadow_draws++;
                s_stats.shadow_triangles += g.added / 3;
                s_stats.shadow_doodads += g.doodadCount;
                usable = true;
            } else if (draw && !usable) {
                s_stats.shadow_waiting++;
            }

            if (usable) {
                light.shadow = static_cast<float>(slot + 1);
                s_stats.shadowed++;
            }
        }

        // The vertices are all in place: point the jobs at them.
        for (size_t j = 0; j < s_shadowJobs.size(); j++) {
            s_shadowJobs[j].vertices = ranges[j].second ? s_shadowVertices.data() + ranges[j].first : nullptr;
            s_shadowJobs[j].parts = s_shadowJobs[j].partCount ? s_shadowParts.data() + partRanges[j] : nullptr;
        }
    }

    // --- the service -------------------------------------------------------------------------------
    wxl_bool WXL_CALL Available() { return s_available ? WXL_TRUE : WXL_FALSE; }

    void WXL_CALL SetSettings(const sxc_lights_settings* settings) {
        if (!settings || settings->struct_size < sizeof(sxc_lights_settings)) {
            s_haveSettings = false;
            return;
        }

        s_settings = *settings;
        s_haveSettings = true;
    }

    void WXL_CALL Stats(sxc_lights_stats* out) {
        if (!out || out->struct_size < sizeof(sxc_lights_stats)) {
            return;
        }

        const uint32_t size = out->struct_size;
        *out = s_stats;
        out->struct_size = size;
    }

    const char* WXL_CALL Status() {
        if (!s_available) {
            return "off: this renderer doesn't draw our lights (Vulkan with our terrain shader only)";
        }

        if (!Settled()) {
            return "off: no mod turned them on";
        }

        std::snprintf(s_status, sizeof(s_status),
                      "%u models; their own point lights %u (%u placed by the client this frame; left out on %u models by the table); "
                      "%u table lights (%u at the model's origin: attachment missing)\n"
                      "%u in range, %u merged, %u drawn%s (%u indoors); grid %u x %u cells of %.0f yd, busiest cell %u lights, %u cells over their limit; "
                      "stock terrain lights %s\n"
                      "shadows: %u lights with a map (of %u slots), %u waiting; %u maps drawn this frame (%u triangles, %u doodads)",
                      s_stats.models, s_stats.model_lights, s_stats.placed, s_stats.suppressed, s_stats.table_lights, s_stats.attach_missing,
                      s_stats.in_range, s_stats.merged, s_stats.active, s_stats.dropped ? " (the farthest left out: more than 512)" : "", s_stats.indoor, s_stats.cells,
                      s_stats.cells, s_frame.cellSize, s_stats.busiest_cell, s_stats.full_cells, StockTerrainOff() ? "off" : "on", s_stats.shadowed,
                      s_stats.shadow_slots, s_stats.shadow_waiting, s_stats.shadow_draws, s_stats.shadow_triangles, s_stats.shadow_doodads);
        return s_status;
    }

    void WXL_CALL SetTable(const sxc_lights_kind* kinds, uint32_t kindCount, const sxc_lights_assignment* rows, uint32_t rowCount) {
        s_kinds.assign(kinds && kindCount ? kinds : nullptr, kinds && kindCount ? kinds + kindCount : nullptr);
        s_rows.clear();
        s_byPath.clear();
        s_anyEmitterRows = false;
        std::unordered_map<uint32_t, int> kindIndex;

        for (size_t k = 0; k < s_kinds.size(); k++) {
            kindIndex[s_kinds[k].id] = static_cast<int>(k);
        }

        for (uint32_t r = 0; rows && r < rowCount; r++) {
            Row row;
            row.a = rows[r];
            row.path = Normalize(rows[r].model_path);
            row.a.model_path = nullptr; // not kept: the caller's
            const auto it = kindIndex.find(rows[r].light_id);
            row.kind = rows[r].light_id && it != kindIndex.end() ? it->second : -1;

            if (row.path.empty()) {
                continue;
            }

            s_anyEmitterRows = s_anyEmitterRows || row.a.emitter_id != 0;
            s_byPath[row.path].push_back(static_cast<uint32_t>(s_rows.size()));
            s_rows.push_back(std::move(row));
        }

        s_tableGeneration++;
        s_missingRows.clear();
    }

    // --- the light editor's queries ----------------------------------------------------------------
    // Every model of the world scene and the models hanging on them, with where its origin is.
    struct SceneModel {
        CM2Model* model;
        C3Vector at;
        bool animated;
    };

    void CollectModels(const CM2Scene* scene, CM2Model* model, int depth, std::vector<SceneModel>& out) {
        if (!model || depth > kMaxAttachDepth) {
            return;
        }

        if (model->m_shared && model->m_shared->m_data && model->IsLoaded(0, 0)) {   // as Visit: loaded models only
            const Placement at{ scene, model, model->m_frameStamp == scene->m_frameStamp };
            C3Vector p;
            float scale = 1.0f;

            if (!Place(at, C3Vector(0.0f, 0.0f, 0.0f), 0, nullptr, p, nullptr, scale)) {
                p = RootOrigin(model);
            }

            out.push_back(SceneModel{ model, p, at.animated });
        }

        for (CM2Model* child = model->m_attachList; child; child = child->m_attachNext) {
            CollectModels(scene, child, depth + 1, out);
        }
    }

    std::deque<std::string> s_names;   // the paths handed out by the last call

    const char* Keep(const char* text) {
        s_names.emplace_back(text ? text : "");
        return s_names.back().c_str();
    }

    uint32_t WXL_CALL ModelsNear(const float pos[3], float range, sxc_lights_model* out, uint32_t max) {
        const CM2Scene* scene = World::s_m2Scene;

        if (!scene || !pos || !out || !max) {
            return 0;
        }

        static std::vector<SceneModel> models;
        models.clear();

        for (CM2Model* model = scene->m_modelList; model; model = model->m_sceneNext) {
            CollectModels(scene, model, 0, models);
        }

        const C3Vector centre(pos[0], pos[1], pos[2]);
        static std::vector<std::pair<float, uint32_t>> inRange;
        inRange.clear();

        for (uint32_t i = 0; i < models.size(); i++) {
            const float d = Length(models[i].at - centre);

            if (d <= range) {
                inRange.emplace_back(d, i);
            }
        }

        std::sort(inRange.begin(), inRange.end());
        s_names.clear();
        uint32_t n = 0;

        for (; n < inRange.size() && n < max; n++) {
            const SceneModel& m = models[inRange[n].second];
            const M2Data* data = m.model->m_shared->m_data;
            sxc_lights_model& o = out[n];
            o = sxc_lights_model{};
            o.path = Keep(m.model->m_shared->m_fileName);
            o.parent_path = m.model->m_attachParent && m.model->m_attachParent->m_shared ? Keep(m.model->m_attachParent->m_shared->m_fileName) : nullptr;
            o.pos[0] = m.at.x;
            o.pos[1] = m.at.y;
            o.pos[2] = m.at.z;
            o.distance = inRange[n].first;
            o.bones = data->m_bones.m_count;
            o.attachments = data->m_attachments.m_count;
            o.emitters = data->m_particles.m_count;
            o.own_lights = data->m_lights.m_count;
            o.animated = m.animated ? WXL_TRUE : WXL_FALSE;
        }

        return n;
    }

    // The placed model of file `path` nearest `pos` (valid until the next call), or null.
    const SceneModel* NearestOf(const char* path, const float pos[3]) {
        const CM2Scene* scene = World::s_m2Scene;

        if (!scene || !path || !pos) {
            return nullptr;
        }

        static std::vector<SceneModel> models;
        models.clear();

        for (CM2Model* model = scene->m_modelList; model; model = model->m_sceneNext) {
            CollectModels(scene, model, 0, models);
        }

        const std::string wanted = Normalize(path);
        const C3Vector centre(pos[0], pos[1], pos[2]);
        const SceneModel* best = nullptr;
        float bestDistance = 0.0f;

        for (const SceneModel& m : models) {
            const float d = Length(m.at - centre);

            if ((!best || d < bestDistance) && Normalize(m.model->m_shared->m_fileName) == wanted) {
                best = &m;
                bestDistance = d;
            }
        }

        return best;
    }

    wxl_bool WXL_CALL ModelAxes(const char* path, const float pos[3], float axes[9]) {
        const SceneModel* best = axes ? NearestOf(path, pos) : nullptr;

        // An attached model's placement is its parent's bone: not a rotation to hand out.
        if (!best || best->model->m_attachParent) {
            return WXL_FALSE;
        }

        const C44Matrix& w = best->model->m_worldTransform;
        const C3Vector unit[3] = { C3Vector(1.0f, 0.0f, 0.0f), C3Vector(0.0f, 1.0f, 0.0f), C3Vector(0.0f, 0.0f, 1.0f) };

        for (int k = 0; k < 3; k++) {
            const C3Vector a = Dir(unit[k], w);
            const float len = Length(a);

            if (!(len > 1e-6f)) {
                return WXL_FALSE;
            }

            axes[k * 3 + 0] = a.x / len;
            axes[k * 3 + 1] = a.y / len;
            axes[k * 3 + 2] = a.z / len;
        }

        return WXL_TRUE;
    }

    uint32_t WXL_CALL ModelPoints(const char* path, const float pos[3], wxl_bool bones, sxc_lights_point* out, uint32_t max) {
        if (!out || !max) {
            return 0;
        }

        const CM2Scene* scene = World::s_m2Scene;
        const SceneModel* best = NearestOf(path, pos);

        if (!best) {
            return 0;
        }

        const Placement at{ scene, best->model, best->animated };
        const M2Data* data = best->model->m_shared->m_data;
        uint32_t n = 0;

        auto add = [&](uint32_t kind, int32_t index, uint32_t bone, const C3Vector& local) {
            C3Vector p;
            float scale = 1.0f;

            if (n < max && Place(at, local, bone, nullptr, p, nullptr, scale)) {
                out[n++] = sxc_lights_point{ kind, index, bone, { p.x, p.y, p.z } };
            }
        };

        for (uint32_t i = 0; i < data->m_attachments.m_count; i++) {
            const M2Attachment& a = data->m_attachments[i];
            add(SXC_LIGHTS_ATTACH_POINT, static_cast<int32_t>(a.m_id), a.m_bone, a.m_position);
        }

        for (uint32_t i = 0; i < data->m_particles.m_count; i++) {
            add(SXC_LIGHTS_ATTACH_EMITTER, static_cast<int32_t>(i), data->m_particles[i].m_boneIndex, data->m_particles[i].m_position);
        }

        for (uint32_t i = 0; i < data->m_lights.m_count; i++) {
            add(SXC_LIGHTS_POINT_OWN_LIGHT, static_cast<int32_t>(i), data->m_lights[i].m_bone, data->m_lights[i].m_position);
        }

        for (uint32_t i = 0; bones && i < data->m_bones.m_count; i++) {
            add(SXC_LIGHTS_ATTACH_BONE, static_cast<int32_t>(i), i, data->m_bones[i].m_pivot);
        }

        return n;
    }

    uint32_t WXL_CALL ActiveLights(sxc_lights_active* out, uint32_t max) {
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(s_active.size(), out ? max : 0));
        std::copy(s_active.begin(), s_active.begin() + n, out);
        return n;
    }

    uint32_t WXL_CALL MissingRows(uint32_t* out, uint32_t max) {
        uint32_t n = 0;

        for (auto it = s_missingRows.begin(); out && it != s_missingRows.end() && n < max; ++it) {
            out[n++] = *it;
        }

        return n;
    }

    uint32_t WXL_CALL Emitters(sxc_lights_emitter* out, uint32_t max) {
        const CM2Scene* scene = World::s_m2Scene;

        if (!scene || !s_anyEmitterRows) {
            s_emitters.clear();
            return 0;
        }

        // The lights' gathering of this frame or the last placed them; else (the lights off, another
        // renderer) a walk of the scene for the emitters alone.
        const uint32_t now = scene->m_frameStamp ? scene->m_frameStamp : 1;

        if (!s_emitterStamp || now - s_emitterStamp > 1) {
            s_emitters.clear();
            const C3Vector camera(scene->m_viewInv.d0, scene->m_viewInv.d1, scene->m_viewInv.d2);
            const float range = s_haveSettings ? std::fmax(s_settings.range, 8.0f) : 256.0f;

            for (CM2Model* model = scene->m_modelList; model; model = model->m_sceneNext) {
                Visit(scene, model, camera, range, 0, false);
            }

            s_emitterStamp = now;
        }

        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(s_emitters.size(), out ? max : 0));
        std::copy(s_emitters.begin(), s_emitters.begin() + n, out);
        return n;
    }

    const sxc_lights_api s_api = { sizeof(sxc_lights_api), Available, SetSettings, Stats, Status, SetTable, ModelsNear, ModelPoints, ActiveLights, MissingRows,
                                   Emitters, ModelAxes };
}

namespace SXC::Lights {
    const sxc_lights_api* Api() {
        return &s_api;
    }

    void SetDevice(bool available, StatusFn status) {
        s_available = available;
        s_deviceStatus = status;
    }

    bool StockTerrainOff() {
        return Settled() && s_settings.stock_off;
    }

    static int s_otherScenes = 0; // scenes other than the world's drawing now (nested: a portrait's)

    void SceneDrawBegin(const CM2Scene* scene) {
        s_otherScenes += scene != World::s_m2Scene ? 1 : 0;
    }

    void SceneDrawEnd(const CM2Scene* scene) {
        s_otherScenes -= scene != World::s_m2Scene && s_otherScenes > 0 ? 1 : 0;
    }

    bool WorldDrawing() {
        return s_otherScenes == 0;
    }

    void SetDrawModel(const CM2Model* model) {
        s_drawModel = model;
    }

    Surface ModelSurface(const CM2Model* model) {
        if (!model || !s_settings.separate_indoor || !Settled()) {
            return kSurfaceAny;
        }

        // The budget is per frame (the frame's numbers start over in Current()).
        const uint32_t now = OsGetAsyncTimeMs();

        if (s_models.size() > 32768) {
            s_models.clear();
        }

        const C3Vector at = RootOrigin(model);
        ModelAnswer& a = s_models[model];
        const C3Vector d = at - a.at;
        const bool stale = a.expires <= now || d.x * d.x + d.y * d.y + d.z * d.z > kMoved * kMoved;

        if (stale && s_stats.model_tests < kModelTestsPerFrame && std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z)) {
            s_stats.model_tests++;
            a.at = at;
            a.surface = IsIndoor(C3Vector(at.x, at.y, at.z + 0.5f), now, false) ? kSurfaceIndoor : kSurfaceOutdoor;
            a.expires = now + 3000 + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(model) % 2000);
        }

        return a.surface;
    }

    void GroupDrawBegin(bool indoor) {
        s_groupDepth++;
        s_groupIndoor = indoor;
    }

    void GroupDrawEnd() {
        s_groupDepth -= s_groupDepth > 0 ? 1 : 0;
    }

    Surface DrawSurface() {
        if (s_otherScenes) {
            return kSurfaceOff;
        }

        if (!s_settings.separate_indoor) {
            return kSurfaceAny;
        }

        if (s_groupDepth) {
            return s_groupIndoor ? kSurfaceIndoor : kSurfaceOutdoor;
        }

        return s_drawModel ? ModelSurface(s_drawModel) : kSurfaceAny;
    }

    const Frame& Current() {
        const uint32_t size = s_stats.struct_size;
        s_stats = sxc_lights_stats{};
        s_stats.struct_size = size;
        s_frame = Frame{};
        s_found.clear();
        s_lights.clear();
        s_strength.clear();
        s_lightFound.clear();
        s_active.clear();

        if (s_files.size() > 16384) {
            s_files.clear();
        }
        const CM2Scene* scene = World::s_m2Scene;

        if (!Settled() || !scene) {
            return s_frame;
        }

        SXC::Profiler::Enter(SXC::Profiler::kLightsScan);
        const sxc_lights_settings& set = s_settings;
        const float range = std::fmax(set.range, 8.0f);
        const float radius = std::fmax(set.radius, 1.0f);
        const float cell = std::fmax(set.cell_size, 4.0f);
        const uint32_t perCell = static_cast<uint32_t>(std::min(std::max(set.max_per_cell, 1), static_cast<int32_t>(kMaxPerCell)));
        // The camera: the scene's view origin in the world (row vectors: world = view x the view's inverse).
        const C3Vector camera(scene->m_viewInv.d0, scene->m_viewInv.d1, scene->m_viewInv.d2);

        s_emitters.clear();

        for (CM2Model* model = scene->m_modelList; model; model = model->m_sceneNext) {
            Visit(scene, model, camera, range, 0, true);
        }

        s_emitterStamp = scene->m_frameStamp ? scene->m_frameStamp : 1;

        s_stats.in_range = static_cast<uint32_t>(s_found.size());
        Merge(set.merge_distance);

        // The nearest kMaxPool, kept in the scan's order (stable between frames: the grid doesn't reshuffle).
        static std::vector<std::pair<float, uint32_t>> order;
        order.clear();

        for (uint32_t k = 0; k < s_found.size(); k++) {
            if (!s_found[k].merged) {
                order.emplace_back(s_found[k].distance, k);
            }
        }

        if (order.size() > kMaxPool) {
            s_stats.dropped = static_cast<uint32_t>(order.size() - kMaxPool);
            std::nth_element(order.begin(), order.begin() + kMaxPool, order.end());
            order.resize(kMaxPool);
            std::sort(order.begin(), order.end(), [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) { return a.second < b.second; });
        }

        const uint32_t nowMs = OsGetAsyncTimeMs();
        const double now = static_cast<double>(nowMs) * 0.001;
        const float fadeStart = range * (2.0f / 3.0f);

        for (const std::pair<float, uint32_t>& o : order) {
            const Found& f = s_found[o.second];
            const float fade = f.distance <= fadeStart ? 1.0f : std::fmax(0.0f, 1.0f - (f.distance - fadeStart) / (range - fadeStart));

            if (fade <= 0.0f) {
                continue;
            }

            const int mode = f.flickerMode == SXC_LIGHTS_FLICKER_DEFAULT ? SXC_LIGHTS_FLICKER_SMOOTH : f.flickerMode;
            const float amount = std::fmin(f.flickerAmount >= 0.0f ? f.flickerAmount : std::fmax(set.flicker, 0.0f), 1.0f);
            const float speed = (f.flickerSpeed >= 0.0f ? f.flickerSpeed : 1.0f) * (set.flicker_speed > 0.0f ? set.flicker_speed : 1.0f);
            const float scale = set.brightness * fade * (1.0f - amount * Flicker(mode, f.pos, now * speed));
            GpuLight l = {};
            l.pos[0] = f.pos.x;
            l.pos[1] = f.pos.y;
            l.pos[2] = f.pos.z;
            l.invRadius = 1.0f / std::fmax(f.radius > 0.0f ? f.radius : radius, 0.5f);
            l.color[0] = f.color.x * scale;
            l.color[1] = f.color.y * scale;
            l.color[2] = f.color.z * scale;
            l.falloff = f.falloff;
            l.spotDir[0] = f.spotDir.x;
            l.spotDir[1] = f.spotDir.y;
            l.spotDir[2] = f.spotDir.z;
            l.cosOuter = f.cosOuter;
            l.spotScale = f.spotScale;

            if (set.separate_indoor && IsIndoor(f.pos, nowMs)) {
                l.flags = kLightIndoor;
                s_stats.indoor++;
            }

            s_lights.push_back(l);
            s_strength.push_back((f.color.x + f.color.y + f.color.z) * set.brightness);
            s_lightFound.push_back(o.second);
        }

        s_stats.active = static_cast<uint32_t>(s_lights.size());
        SXC::Profiler::Leave(SXC::Profiler::kLightsScan);
        { SXC::Profiler::Scope p(SXC::Profiler::kLightsShadows); AssignShadows(nowMs); }
        { SXC::Profiler::Scope p(SXC::Profiler::kLightsGrid); BuildGrid(camera, range, cell, perCell); }

        // For the light editor's markers.
        for (uint32_t i = 0; i < s_lights.size(); i++) {
            const GpuLight& l = s_lights[i];
            const Found& f = s_found[s_lightFound[i]];
            sxc_lights_active a = {};
            std::memcpy(a.pos, l.pos, sizeof a.pos);
            a.radius = 1.0f / l.invRadius;
            std::memcpy(a.color, l.color, sizeof a.color);
            a.cos_outer = l.cosOuter;
            std::memcpy(a.spot_dir, l.spotDir, sizeof a.spot_dir);
            a.indoor = l.flags == kLightIndoor ? WXL_TRUE : WXL_FALSE;
            a.shadowed = l.shadow > 0.0f ? WXL_TRUE : WXL_FALSE;
            a.row_id = f.rowId;
            a.model_path = f.path ? f.path->c_str() : "";
            s_active.push_back(a);
        }

        // The view back to the world (row vectors: world = view x the view's inverse), a row per world axis.
        const C44Matrix& vi = scene->m_viewInv;
        const float rows[12] = { vi.a0, vi.b0, vi.c0, vi.d0, vi.a1, vi.b1, vi.c1, vi.d1, vi.a2, vi.b2, vi.c2, vi.d2 };
        std::memcpy(s_frame.viewInv, rows, sizeof rows);

        s_frame.on = true;
        s_frame.debug = set.debug;
        s_frame.lightCount = static_cast<uint32_t>(s_lights.size());
        s_frame.lights = s_lights.empty() ? nullptr : s_lights.data();
        s_frame.cellLights = s_cellLights.data();
        s_frame.shadowJobs = s_shadowJobs.empty() ? nullptr : s_shadowJobs.data();
        s_frame.shadowJobCount = static_cast<uint32_t>(s_shadowJobs.size());
        return s_frame;
    }

    void SetShadowDevice(uint32_t slots, uint32_t maxVertices) {
        s_shadowSlotCount = slots;
        s_shadowMaxVertices = maxVertices;
        s_shadowSlots.clear();   // a new device: nothing drawn
    }

    void ShadowIncomplete(uint32_t slot) {
        if (slot < s_shadowSlots.size()) {
            // Redrawn at its next look (a zero signature differs from any gathered one), each time a little
            // later: a texture that never comes costs a gathering every 16 s, not every half second.
            ShadowSlot& s = s_shadowSlots[slot];
            s.signature = 0;
            s.checkMs = OsGetAsyncTimeMs() + std::min(500u << std::min(s.incomplete, 5u), 16000u);
            s.incomplete++;
        }
    }

    void ShadowsDrawn(uint32_t drawn) {
        for (size_t j = drawn; j < s_shadowJobSlots.size(); j++) {
            if (s_shadowJobSlots[j] < s_shadowSlots.size()) {
                s_shadowSlots[s_shadowJobSlots[j]].valid = false;
            }
        }
    }
}
