// SXC: a second view of the world (SXC/View/View.h).
#include "SXC/View/View.h"

#include <cmath>
#include <cstring>
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

    bool s_planned = false;   // PlanMirror decided this frame (WorldBegin keeps to it)
    bool s_plannedOn = false; // and to mirror
    float s_plannedZ = 0.0f;

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

    // The surface of the water nearest (x, y) in the shore field (terrain water around the player, ~100 yd
    // searched in rings); false where none is.
    bool NearestWater(float x, float y, float& z) {
        const SXC::Water::ShoreMap& m = SXC::Water::ShoreField();

        if (!m.valid) {
            return false;
        }

        const int32_t cx = static_cast<int32_t>(std::floor((kHalfWorld - y) / SXC::Water::kShoreCell));
        const int32_t cy = static_cast<int32_t>(std::floor((kHalfWorld - x) / SXC::Water::kShoreCell));

        for (int32_t r = 0; r <= 48; r++) {
            float best = 1e9f;

            for (int32_t gy = cy - r; gy <= cy + r; gy++) {
                for (int32_t gx = cx - r; gx <= cx + r; gx++) {
                    if (std::abs(gx - cx) != r && std::abs(gy - cy) != r) {
                        continue;   // the ring only
                    }

                    if (gx < m.originX || gy < m.originY || gx >= m.originX + SXC::Water::kShoreSize || gy >= m.originY + SXC::Water::kShoreSize) {
                        continue;
                    }

                    const size_t k = static_cast<size_t>(gy & (SXC::Water::kShoreSize - 1)) * SXC::Water::kShoreSize + static_cast<size_t>(gx & (SXC::Water::kShoreSize - 1));

                    if (FromHalf(m.texels[k * 4]) <= 0.0f) {
                        continue;   // land
                    }

                    const float d = static_cast<float>((gx - cx) * (gx - cx) + (gy - cy) * (gy - cy));

                    if (d < best) {
                        best = d;
                        z = m.cells[k * 4];
                    }
                }
            }

            if (best < 1e9f) {
                return true;
            }
        }

        return false;
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

        // The water under the camera (a WMO's where the camera is inside its group), else the nearest in
        // the shore field around.
        if (!SXC::Water::LiquidAt(camera, planeZ, type) && !NearestWater(camera.x, camera.y, planeZ)) {
            s_why = "no water near";
            return false;
        }

        if (camera.z < planeZ + 0.3f || camera.z - planeZ > 400.0f) {
            s_why = camera.z < planeZ + 0.3f ? "the camera is under the water" : "the water is far below";
            return false;
        }

        s_plannedZ = planeZ;
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

    bool Mirror(float& planeZ) {
        planeZ = s_planeZ;
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

        if (!s_drawn) {
            ImGui::TextDisabled("Mirror: none this frame (%s).", s_why);
            return;
        }

        uint32_t copied = 0, left = 0, noProjection = 0, width = 0, height = 0;

        if (s_device && s_device->stats) {
            s_device->stats(copied, left, noProjection, width, height);
        }

        ImGui::TextDisabled("Mirror at z %.2f, %ux%u: %u draws copied, %u left out (interface, decals, water), %u without a projection found.",
                            s_planeZ, width, height, copied, left, noProjection);
    }
}
