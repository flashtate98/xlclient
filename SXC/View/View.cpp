// SXC: a second view of the world (SXC/View/View.h).
#include "SXC/View/View.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "SXC/Settings/Settings.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "SXC/Water/Liquid.h"
#include "SXC/Water/Shore.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    const SXC::View::Device* s_device = nullptr;
    bool s_recording = false;
    bool s_drawn = false;
    float s_planeZ = 0.0f;
    const char* s_why = "";   // why no mirror this frame (the panel)

    constexpr float kHalfWorld = 32.0f * 533.33333f;
    constexpr float kLift = 0.05f;   // yd: the clip plane a hair above the water (no gap along the waterline)

    SXC::Settings::Bool& MirrorOn() {
        static SXC::Settings::Bool s_setting("Liquid", "Mirror", true, "The mirror",
                                             "The world drawn again, mirrored at the water's height, for the water's reflection: what the "
                                             "screen-space reflections can't see (undersides, the sky off screen, the player). Costs the "
                                             "GPU a second drawing of what is on screen; the CPU only the recording.");
        return s_setting;
    }

    SXC::Settings::Float& Reach() {
        static SXC::Settings::Float s_setting("Liquid", "MirrorReach", 300.0f, 50.0f, 2000.0f, "The mirror's reach",
                                              "How far out the mirror's culling adds what only the mirror sees (yd): more shows "
                                              "in the water, at the cost of drawing it.");
        return s_setting;
    }

    SXC::Settings::Bool& FullReach() {
        static SXC::Settings::Bool s_setting("Liquid", "MirrorFullReach", true, "The mirror culls as far as you see",
                                             "The mirror's culling reaches as far as the view (the distant terrain and mountains "
                                             "included); off: only out to the mirror's reach.");
        return s_setting;
    }

    SXC::Settings::Bool& Grass() {
        static SXC::Settings::Bool s_setting("Liquid", "MirrorGrass", true, "Grass in the mirror",
                                             "The grass drawn into the mirror too: its plants near the player, without their density "
                                             "copies (most of the grass's triangles).");
        return s_setting;
    }

    SXC::Settings::Float& GrassReach() {
        static SXC::Settings::Float s_setting("Liquid", "MirrorGrassReach", 200.0f, 30.0f, 1000.0f, "The mirror's grass reach",
                                              "How far from the player the mirror draws grass (yd).");
        return s_setting;
    }

    // How far off the water may be for a mirror (Seyri, 2026-11-05: it popped in on lakes as you came
    // closer, at the ~100 yd the search reached then), and the last stretch of that over which it fades in.
    SXC::Settings::Float& Distance() {
        static SXC::Settings::Float s_setting("Liquid", "MirrorDistance", 250.0f, 50.0f, 260.0f, "The mirror's distance",
                                              "How far off the nearest water may be for the mirror (yd, at most the shore "
                                              "field's ~260): farther, the water shows the screen-space reflection only.");
        return s_setting;
    }

    SXC::Settings::Float& Fade() {
        static SXC::Settings::Float s_setting("Liquid", "MirrorFade", 100.0f, 0.0f, 250.0f, "The mirror's fade",
                                              "Over how many yards before its distance the mirror fades in, instead of switching on.");
        return s_setting;
    }

    bool s_planned = false;   // PlanMirror decided this frame (WorldBegin keeps to it)
    bool s_plannedOn = false; // and to mirror
    float s_plannedZ = 0.0f;
    float s_plannedFade = 1.0f;
    float s_fade = 1.0f;      // the drawn mirror's share (1 full, toward 0 as the water lies farther)
    float s_waterDistance = 0.0f;
    constexpr float kHighest = 400.0f;   // yd: the highest above the water the camera mirrors
    constexpr float kRightAbove = 15.0f; // yd: the water under the camera is the plane, seen or not
    bool s_hadPlane = false;             // the last frame's plane (SeenWater keeps to it)
    float s_lastPlane = 0.0f;

    // The panel's view of the choice: the levels SeenWater weighed this frame, and how the plane was taken.
    struct Candidate {
        float z, own, weight, nearest;   // weight: with the neighbouring half-yard steps (what is compared)
        uint32_t cells;
    };

    std::vector<Candidate> s_candidates;
    bool s_keptPrevious = false;         // a heavier level lost to the last plane (it wasn't 1.5x heavier)
    const char* s_pickedBy = "";

    SXC::Settings::Int& Divisor() {
        static SXC::Settings::Int s_setting("Liquid", "MirrorDivisor", 2, 1, 4, "The mirror's size",
                                            "The mirror's target is the screen's size divided by this.");
        return s_setting;
    }

    float FromHalf(uint16_t h) {
        const uint32_t sign = (h & 0x8000u) << 16;
        const uint32_t exponent = (h >> 10) & 0x1Fu;
        const uint32_t mantissa = h & 0x3FFu;

        if (exponent == 0) {
            return 0.0f;
        }

        const uint32_t x = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
        float f;
        std::memcpy(&f, &x, 4);
        return f;
    }

    // The surface of the water nearest (x, y) in the shore field (terrain water around the player, its whole
    // window searched in rings out to `reach` yd) and how far it lies (yd, across the ground); false where none
    // is. The rings are squares: past the first hit, those that could still hold a nearer cell are searched too.
    bool NearestWater(float x, float y, float reach, float& z, float& distance) {
        const SXC::Water::ShoreMap& m = SXC::Water::ShoreField();

        if (!m.valid) {
            return false;
        }

        const int32_t cx = static_cast<int32_t>(std::floor((kHalfWorld - y) / SXC::Water::kShoreCell));
        const int32_t cy = static_cast<int32_t>(std::floor((kHalfWorld - x) / SXC::Water::kShoreCell));
        const int32_t rings = std::min(static_cast<int32_t>(reach / SXC::Water::kShoreCell) + 1, SXC::Water::kShoreSize);
        int32_t last = rings;
        int32_t best = INT32_MAX;

        auto visit = [&](int32_t gx, int32_t gy) {
            if (gx < m.originX || gy < m.originY || gx >= m.originX + SXC::Water::kShoreSize || gy >= m.originY + SXC::Water::kShoreSize) {
                return;
            }

            const size_t k = static_cast<size_t>(gy & (SXC::Water::kShoreSize - 1)) * SXC::Water::kShoreSize + static_cast<size_t>(gx & (SXC::Water::kShoreSize - 1));

            if (FromHalf(m.texels[k * 4]) <= 0.0f) {
                return;   // land
            }

            const int32_t d = (gx - cx) * (gx - cx) + (gy - cy) * (gy - cy);

            if (d < best) {
                best = d;
                z = m.cells[k * 4];
            }
        };

        for (int32_t r = 0; r <= last; r++) {
            if (r == 0) {
                visit(cx, cy);
            } else {
                for (int32_t i = -r; i <= r; i++) {
                    visit(cx + i, cy - r);
                    visit(cx + i, cy + r);
                }

                for (int32_t i = -r + 1; i <= r - 1; i++) {
                    visit(cx - r, cy + i);
                    visit(cx + r, cy + i);
                }
            }

            if (best != INT32_MAX && last == rings) {
                last = std::min(rings, static_cast<int32_t>(std::ceil(r * 1.4143f)));
            }
        }

        if (best == INT32_MAX) {
            return false;
        }

        distance = std::sqrt(static_cast<float>(best)) * SXC::Water::kShoreCell;
        return distance <= reach;
    }

    // Row-major 4x4 helpers over the engine's matrices (row vectors: v' = v M).
    void Load(const C44Matrix& m, float out[16]) {
        std::memcpy(out, &m, 16 * sizeof(float));
    }

    void Multiply(const float* a, const float* b, float* out) {
        float r[16];

        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
            }
        }

        std::memcpy(out, r, sizeof r);
    }

    // The water the camera sees most of (Seyri, 2026-11-13: the plane was the water nearest the player, so a
    // lake looked at past a nearer river or pond never took the mirror and showed the sky's gradient). The
    // shore field's water cells in view, every second one each way, grouped by height (half-yard steps);
    // each counts what it covers on screen (its solid angle).
    // The level's weighted surface and its nearest cell across the ground; false when none is in view.
    // `previous` (the last plane) wins unless another covers clearly more, so two waters don't flicker.
    bool SeenWater(const C3Vector& camera, float reach, bool hadPrevious, float previous, float& z, float& distance) {
        const SXC::Water::ShoreMap& m = SXC::Water::ShoreField();

        if (!m.valid) {
            return false;
        }

        const CGxMatrixStack& viewStack = g_theGxDevicePtr->m_xforms[GxXform_View];
        float vp[16], p[16];
        Load(viewStack.m_mtx[viewStack.m_level], vp);
        Load(g_theGxDevicePtr->m_projNative, p);
        Multiply(vp, p, vp);

        struct Level {
            float weight = 0.0f, zSum = 0.0f, nearest = 1e30f;
            uint32_t cells = 0;
        };

        std::unordered_map<int32_t, Level> levels;
        const float cell = SXC::Water::kShoreCell, area = 4.0f * cell * cell;
        const float reach2 = reach * reach;

        for (int32_t gy = m.originY; gy < m.originY + SXC::Water::kShoreSize; gy += 2) {
            for (int32_t gx = m.originX; gx < m.originX + SXC::Water::kShoreSize; gx += 2) {
                const size_t k = static_cast<size_t>(gy & (SXC::Water::kShoreSize - 1)) * SXC::Water::kShoreSize + static_cast<size_t>(gx & (SXC::Water::kShoreSize - 1));

                if (FromHalf(m.texels[k * 4]) <= 0.0f) {
                    continue;   // land
                }

                const float wz = m.cells[k * 4];
                const float rx = kHalfWorld - (static_cast<float>(gy) + 0.5f) * cell - camera.x;
                const float ry = kHalfWorld - (static_cast<float>(gx) + 0.5f) * cell - camera.y;
                const float rz = wz - camera.z;
                const float flat2 = rx * rx + ry * ry;

                if (rz > -0.3f || flat2 > reach2) {
                    continue;   // at or above the camera, or past the distance
                }

                // In view: the clip space's x, y within w (with a margin: a wave's tilt reads a little aside).
                const float cx = rx * vp[0] + ry * vp[4] + rz * vp[8] + vp[12];
                const float cy = rx * vp[1] + ry * vp[5] + rz * vp[9] + vp[13];
                const float cw = rx * vp[3] + ry * vp[7] + rz * vp[11] + vp[15];

                if (cw <= 0.0f || std::abs(cx) > 1.2f * cw || std::abs(cy) > 1.2f * cw) {
                    continue;
                }

                // Water near the camera counts as if 25 yd off (the few yards under it outweighed a lake: Seyri,
                // 2026-11-13), and sloped water (a river running down; its neighbour two cells on differs) a
                // tenth: one flat plane suits it badly.
                // The solid angle: the area x the sine of the angle it's seen at / the distance squared.
                const float d = std::sqrt(flat2 + rz * rz);
                Level& l = levels[static_cast<int32_t>(std::floor(wz * 2.0f))];
                float w = area * (-rz / d) / std::max(d * d, 625.0f);
                const size_t kx = static_cast<size_t>(gy & (SXC::Water::kShoreSize - 1)) * SXC::Water::kShoreSize + static_cast<size_t>((gx + 2) & (SXC::Water::kShoreSize - 1));
                const size_t ky = static_cast<size_t>((gy + 2) & (SXC::Water::kShoreSize - 1)) * SXC::Water::kShoreSize + static_cast<size_t>(gx & (SXC::Water::kShoreSize - 1));

                if ((FromHalf(m.texels[kx * 4]) > 0.0f && std::abs(m.cells[kx * 4] - wz) > 0.25f) ||
                    (FromHalf(m.texels[ky * 4]) > 0.0f && std::abs(m.cells[ky * 4] - wz) > 0.25f)) {
                    w *= 0.1f;
                }
                l.weight += w;
                l.zSum += w * wz;
                l.cells++;
                l.nearest = std::min(l.nearest, flat2);
            }
        }

        // Neighbouring half-yard steps are one water (a level surface on a step's edge, a gentle slope): the
        // group decides, its heaviest step is the plane. (The best scoring step itself was a sliver of 7 cells
        // half a yard above a lake of 758, its score the lake's: Seyri, 2026-11-13.)
        const Level* best = nullptr;
        int32_t bestKey = 0;
        float bestWeight = 0.0f, previousWeight = 0.0f;
        s_candidates.clear();

        for (const auto& [key, l] : levels) {
            float w = l.weight;

            for (int32_t d : { -1, 1 }) {
                const auto n = levels.find(key + d);
                w += n != levels.end() ? n->second.weight : 0.0f;
            }

            s_candidates.push_back(Candidate{ l.zSum / l.weight, l.weight, w, std::sqrt(l.nearest), l.cells });

            if (w > bestWeight) {
                bestWeight = w;
                bestKey = key;
                best = &l;
            }

            if (hadPrevious && std::abs(l.zSum / l.weight - previous) < 0.6f) {
                previousWeight = std::max(previousWeight, w);
            }
        }

        if (!best) {
            return false;
        }

        for (int32_t d : { -1, 1 }) {
            const auto n = levels.find(bestKey + d);

            if (n != levels.end() && n->second.weight > best->weight) {
                best = &n->second;
            }
        }

        if (previousWeight > 0.0f && bestWeight < 1.5f * previousWeight) {
            const Level* kept = nullptr;

            for (const auto& [key, l] : levels) {
                if (std::abs(l.zSum / l.weight - previous) < 0.6f && (!kept || l.weight > kept->weight)) {
                    kept = &l;
                }
            }

            s_keptPrevious = kept != nullptr && kept != best;
            best = kept ? kept : best;
        } else {
            s_keptPrevious = false;
        }

        z = best->zSum / best->weight;
        distance = std::sqrt(best->nearest);
        return true;
    }

    // The sky's: directions mirrored (z to -z through the camera), nothing clipped; as a transform of view space.
    void SkyTransform(const C44Matrix& view, float out[16]) {
        C44Matrix reflect;
        reflect.c2 = -1.0f;
        float v[16], vi[16], r[16];
        Load(view, v);
        Load(view.Inverse(view.Determinant()), vi);
        Load(reflect, r);
        Multiply(vi, r, out);
        Multiply(out, v, out);
    }

    // The view: the camera mirrored at the plane (camera-relative height `rel`) as a transform of view space,
    // and the plane in the mirrored view's space, kept on its far side from the camera (the device makes it
    // each draw's near plane: nothing below the water is drawn into the mirror). `view` the world's view
    // (camera-relative world to view space). False when the camera isn't above the plane.
    bool MirrorTransform(const C44Matrix& view, float rel, float out[16], float plane[4]) {
        // The reflection in camera-relative world space: z to 2 rel - z.
        C44Matrix reflect;
        reflect.c2 = -1.0f;
        reflect.d2 = 2.0f * rel;
        float v[16], vi[16], r[16];
        Load(view, v);
        Load(view.Inverse(view.Determinant()), vi);
        Load(reflect, r);
        Multiply(vi, r, out);
        Multiply(out, v, out);   // view space to the mirrored view's

        // In camera-relative world (0, 0, -1, rel + lift), carried by the view's inverse (a plane goes by the
        // inverse transposed).
        const float world[4] = { 0.0f, 0.0f, -1.0f, rel + kLift };

        for (int i = 0; i < 4; i++) {
            plane[i] = vi[i * 4] * world[0] + vi[i * 4 + 1] * world[1] + vi[i * 4 + 2] * world[2] + vi[i * 4 + 3] * world[3];
        }

        return plane[3] < 0.0f;   // else the camera is on the kept side: under the water
    }
}

namespace SXC::View {
    void SetDevice(const Device* device) {
        s_device = device;
        s_recording = false;
        s_drawn = false;
    }

    float MirrorReach() {
        return FullReach().Get() ? 1e7f : Reach().Get();
    }

    bool Planned(float& planeZ, bool& full) {
        planeZ = s_plannedZ;
        full = FullReach().Get();
        return s_planned && s_plannedOn;
    }

    bool PlanMirror(const C3Vector& camera, float& planeZ) {
        s_planned = true;
        s_plannedOn = false;
        s_plannedZ = 0.0f;

        if (!s_device || !MirrorOn().Get()) {
            s_why = !s_device ? "no device (the Vulkan device draws second views)" : "off";
            return false;
        }

        uint32_t type = 0;
        const float reach = Distance().Get();
        float distance = 0.0f;

        // A WMO's water right under the camera (where it is inside its group: the shore field holds only the
        // terrain's), else the water seen most of, else the nearest in the shore field around. Terrain water
        // under the camera competes with the rest (a river under it broke the lake beyond: Seyri, 2026-11-13).
        float under = 0.0f;
        bool underIsMapObj = false;
        const bool hasUnder = SXC::Water::LiquidAt(camera, under, type, &underIsMapObj);

        s_candidates.clear();
        s_keptPrevious = false;

        if (hasUnder && underIsMapObj && camera.z - under < kRightAbove) {
            planeZ = under;
            s_pickedBy = "a WMO's water right under the camera";
        } else if (SeenWater(camera, reach, s_hadPlane, s_lastPlane, planeZ, distance)) {
            s_pickedBy = s_keptPrevious ? "the water seen most of, kept from the last frame" : "the water seen most of";
        } else if (hasUnder) {
            planeZ = under;
            s_pickedBy = "no water in view: the water under the camera";
        } else if (NearestWater(camera.x, camera.y, reach, planeZ, distance)) {
            s_pickedBy = "no water in view: the nearest water";
        } else {
            s_why = "no water near";
            s_pickedBy = "";
            s_hadPlane = false;
            return false;
        }

        s_hadPlane = true;
        s_lastPlane = planeZ;

        if (camera.z < planeZ + 0.3f || camera.z - planeZ > kHighest) {
            s_why = camera.z < planeZ + 0.3f ? "the camera is under the water" : "the water is far below";
            return false;
        }

        // Faded in over the last stretch of the distance across the ground, and of the height above the
        // water (flying straight up, it popped at the height's limit: Seyri, 2026-11-13).
        const float fade = std::max(Fade().Get(), 0.0f);
        const float acrossT = fade > 0.0f ? std::clamp((reach - distance) / fade, 0.0f, 1.0f) : 1.0f;
        const float aboveT = fade > 0.0f ? std::clamp((kHighest - (camera.z - planeZ)) / fade, 0.0f, 1.0f) : 1.0f;
        const float t = std::min(acrossT, aboveT);
        s_waterDistance = distance;

        if (t <= 0.0f) {
            s_why = "the water is at the distance's edge";
            return false;
        }

        s_plannedZ = planeZ;
        s_plannedFade = t * t * (3.0f - 2.0f * t);
        s_plannedOn = true;
        s_why = "";
        return true;
    }

    void WorldBegin() {
        s_recording = false;
        s_drawn = false;
        const C3Vector& camera = CWorldScene::s_activeWorldView;
        float planeZ = s_plannedZ;

        // The plan the culling took (CWorldScene::Update), else decided now (nothing extra culled then).
        if (!s_planned) {
            PlanMirror(camera, planeZ);
        }

        s_planned = false;

        if (!s_plannedOn) {
            return;
        }

        planeZ = s_plannedZ;

        const CGxMatrixStack& viewStack = g_theGxDevicePtr->m_xforms[GxXform_View];
        const C44Matrix& view = viewStack.m_mtx[viewStack.m_level];
        const C44Matrix& proj = g_theGxDevicePtr->m_projNative;
        float mirror[16], plane[4], sky[16], p[16];

        if (!MirrorTransform(view, planeZ - camera.z, mirror, plane)) {
            s_why = "the camera is under the water";
            return;
        }

        SkyTransform(view, sky);
        Load(proj, p);

        if (!s_device->begin(mirror, plane, sky, p, static_cast<uint32_t>(Divisor().Get()), Grass().Get(), GrassReach().Get())) {
            s_why = "the device refused (no target)";
            return;
        }

        s_recording = true;
        s_planeZ = planeZ;
        s_fade = s_plannedFade;
        s_why = "";
    }

    void WorldEnd() {
        if (!s_recording) {
            return;
        }

        s_recording = false;
        s_device->end();
        s_drawn = true;
    }

    void Sky(bool on) {
        if (s_recording && s_device->sky) {
            s_device->sky(on);
        }
    }

    bool Mirror(float& planeZ, float& fade) {
        planeZ = s_planeZ;
        fade = s_fade;
        return s_drawn;
    }

    void DrawPanel() {
        bool on = MirrorOn().Get();

        if (ImGui::Checkbox("The mirror", &on)) {
            MirrorOn().Set(on);
        }

        ImGui::SetItemTooltip("%s", "The world drawn again, mirrored at the water's height: what screen-space reflections can't see "
                                    "(undersides, the sky off screen, your own character). One plane: the water under the camera or nearest it.");
        static const char* const kSizes[] = { "Full", "Half", "A third", "A quarter" };
        int size = Divisor().Get() - 1;
        ImGui::SetNextItemWidth(140.0f);

        if (ImGui::Combo("Mirror size", &size, kSizes, IM_ARRAYSIZE(kSizes))) {
            Divisor().Set(size + 1);
        }

        float distance = Distance().Get();
        ImGui::SetNextItemWidth(200.0f);

        if (ImGui::SliderFloat("Mirror distance (yd)", &distance, 50.0f, 260.0f, "%.0f")) {
            Distance().Set(distance);
        }

        ImGui::SetItemTooltip("%s", "How far off the nearest water may be for the mirror (at most the shore field's ~260 yd).");
        float fade = Fade().Get();
        ImGui::SetNextItemWidth(200.0f);

        if (ImGui::SliderFloat("Mirror fade (yd)", &fade, 0.0f, 250.0f, "%.0f")) {
            Fade().Set(fade);
        }

        ImGui::SetItemTooltip("%s", "Over how many yards before the distance the mirror fades in (0: it switches on).");

        bool grass = Grass().Get();

        if (ImGui::Checkbox("Grass in the mirror", &grass)) {
            Grass().Set(grass);
        }

        ImGui::SetItemTooltip("%s", "The grass drawn into the mirror too: its plants near you, without the density copies.");

        if (grass) {
            float reach = GrassReach().Get();
            ImGui::SetNextItemWidth(200.0f);

            if (ImGui::SliderFloat("Mirror grass reach (yd)", &reach, 30.0f, 1000.0f, "%.0f")) {
                GrassReach().Set(reach);
            }
        }
        bool full = FullReach().Get();

        if (ImGui::Checkbox("Mirror culls as far as you see", &full)) {
            FullReach().Set(full);
        }

        ImGui::SetItemTooltip("%s", "What only the mirror sees (above and behind you, looking down) is culled in out to the view's "
                                    "distance, the distant terrain too. Off: only out to the reach below.");

        if (!full) {
            float reach = Reach().Get();
            ImGui::SetNextItemWidth(200.0f);

            if (ImGui::SliderFloat("Mirror reach (yd)", &reach, 50.0f, 2000.0f, "%.0f")) {
                Reach().Set(reach);
            }
        }

        // How the plane was chosen: the levels of water in view, by how much of the screen each covers.
        if (*s_pickedBy) {
            ImGui::TextDisabled("Plane: %s.", s_pickedBy);
        }

        if (!s_candidates.empty() && ImGui::TreeNode("mirrorLevels", "Water levels in view (%zu)", s_candidates.size())) {
            std::vector<Candidate> sorted = s_candidates;
            std::sort(sorted.begin(), sorted.end(), [](const Candidate& a, const Candidate& b) { return a.weight > b.weight; });
            const float top = sorted.front().weight > 0.0f ? sorted.front().weight : 1.0f;

            if (ImGui::BeginTable("mirrorLevelsTable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("z");
                ImGui::TableSetupColumn("score (with +-0.5 yd)");
                ImGui::TableSetupColumn("own");
                ImGui::TableSetupColumn("cells");
                ImGui::TableSetupColumn("nearest (yd)");
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < sorted.size() && i < 12; i++) {
                    const Candidate& c = sorted[i];
                    const bool chosen = std::abs(c.z - s_lastPlane) < 0.3f;
                    const ImVec4 colour = chosen ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(colour, "%.2f%s", c.z, chosen ? "  <" : "");
                    ImGui::TableNextColumn();
                    ImGui::TextColored(colour, "%.0f%%", 100.0f * c.weight / top);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(colour, "%.0f%%", 100.0f * c.own / top);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(colour, "%u", c.cells);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(colour, "%.0f", c.nearest);
                }

                ImGui::EndTable();
            }

            ImGui::TextDisabled("Each level scores what it covers on screen (water nearer than 25 yd counted as 25 yd off, "
                                "sloped water a tenth). The last plane stays unless another scores 1.5x more.");
            ImGui::TreePop();
        }

        if (!s_drawn) {
            ImGui::TextDisabled("Mirror: none this frame (%s).", s_why);
            return;
        }

        uint32_t copied = 0, left = 0, noProjection = 0, width = 0, height = 0;

        if (s_device && s_device->stats) {
            s_device->stats(copied, left, noProjection, width, height);
        }

        ImGui::TextDisabled("Mirror at z %.2f, the water %.0f yd off, %.0f%% in, %ux%u: %u draws copied, %u left out (interface, decals, water), "
                            "%u without a projection found.",
                            s_planeZ, s_waterDistance, s_fade * 100.0f, width, height, copied, left, noProjection);
    }
}
