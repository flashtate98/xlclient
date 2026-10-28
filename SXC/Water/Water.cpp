// Our water (SXC/Water/Water.h): the stock water materials' instances drawn through the "SxcWater" pair,
// which only our Vulkan device knows (Shaders/Water.vert, Water.frag). The draw follows the stock
// materials' own (MaterialWater.cpp, MaterialProcWater.cpp): the instance's mesh in the vertex format
// its stock material asks for (the terrain's batches keep that format once built), the liquid code's
// constants, then ours.
#include "SXC/Water/Water.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Storm/H/storm.h"
#include "SXC/Fog/Fog.h"
#include "SXC/Grass/Grass.h"
#include "SXC/Sky/Sky.h"
#include "SXC/Log.h"
#include "SXC/Settings/Settings.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"

namespace {
    bool s_available = false;
    SXC::Water::CaptureFn s_capture = nullptr;
    SXC::Water::StatusFn s_status = nullptr;
    CGxShader* s_vertexShader = nullptr;
    CGxShader* s_pixelShader = nullptr;
    bool s_shadersFailed = false;

    SXC::Settings::Bool& OurShader() {
        static SXC::Settings::Bool s_setting("Liquid", "OurShader", true, "Our water (Vulkan)",
                                             "Draw the water (the stock materials 1 and 3, terrain and WMO) with our own shader instead of the client's. "
                                             "Magma and slime stay stock.");
        return s_setting;
    }

    // Our pixel registers (Water.frag): p6 = (time, debug view, ripple strength, 0), p7 the sky's zenith,
    // p8 its horizon, p9..p11 the look below.
    constexpr uint32_t kPixelFirst = 6;
    constexpr uint32_t kPixelRows = 6;
    constexpr float kRippleStrength = 1.0f;

    // The look, as the Water panel sets it (live, not saved: the values go into the LiquidSurface table
    // per water later). The defaults are the first tuning (Seyri, 2026-10-28).
    struct Look {
        // The water.
        float clarityClear = 0.01f;   // absorption per yd where the stock tint's alpha is 0 ..
        float clarityMurky = 0.12f;   // .. and where it is 1
        float scatter = 0.65f;        // the water's own colour, x the stock tint
        float refraction = 0.6f;      // how far the ripples bend the scene behind
        // The surface.
        float reflectivity = 0.02f;   // Fresnel looking straight down (real water 0.02)
        float reflection = 1.0f;      // x the reflection
        bool screenReflections = true;
        float window = 0.3f;          // Snell's window from below (cos; real water 0.66)
        float glint = 1.5f;           // the sun's highlight
        float glintSharpness = 600.0f;
        // The waves.
        float amplitude = 0.35f;
        float chop = 1.2f;
        float spread = 0.3f;
        float foam = 1.0f;
        float foamThreshold = 0.95f;  // the Jacobian below which crests foam
        float move = 1.0f;            // the surface's displacement (0 flat; the height above sets how far)
        bool windOverride = false;    // the panel's wind instead of the mod's
        float windSpeed = 6.0f;       // yd / s
        float windAngle = 0.0f;       // degrees, 0 = +x
        // The debug view (Water.frag).
        int debugView = 0;
    };
    Look s_look;

    // Why the stock material kept a draw (the Water panel's counts).
    enum class Reason { Inactive, NotWater, NoTint, Shaders, Capture, Count };

    struct Counts {
        float since = 0.0f;
        uint32_t ours[4] = {};           // by stock material (1, 3)
        uint32_t stock[5] = {};          // by Reason
        uint32_t stockMaterials[8] = {}; // by LiquidMaterial (7: 7 and up, or none)
    };
    Counts s_counts, s_shown;
    bool s_hideStock = false;
    bool s_rebuildWaves = false;

    // The LiquidMaterial an instance's material was made for (the bank's index), 7 when none or past 6.
    uint32_t MaterialId(const Liquid::IMaterial* material) {
        TSGrowableArray<Liquid::IMaterial*>& bank = Liquid::CMaterialBank::s_materials;

        for (uint32_t id = 0; material && id < bank.Count() && id < 7; id++) {
            if (bank.m_data[id] == material) {
                return id;
            }
        }

        return 7;
    }

    // The stock material an instance has: 1 or 3 when it's one of the stock water materials, else 0.
    uint32_t StockWaterId(const Liquid::IMaterial* material) {
        TSGrowableArray<Liquid::IMaterial*>& bank = Liquid::CMaterialBank::s_materials;
        const uint32_t ids[2] = { 1u, 3u };

        for (uint32_t id : ids) {
            if (material && id < bank.Count() && bank.m_data[id] == material) {
                return id;
            }
        }

        return 0;
    }

    // The tint: the settings' texture the client generates ("procedural...": the river, ocean or WMO
    // water gradient), wherever the LiquidType row put it (material 1 rows: texture 1, ProcWater rows:
    // texture 4).
    CGxTex* TintTexture(Liquid::CMaterialSettings* settings) {
        for (uint32_t i = 0; i < 6; i++) {
            if (settings->m_textureNames[i][0] && SStrStrI(settings->m_textureNames[i], "procedural")) {
                return settings->GetTexture(i, 1250);
            }
        }

        return nullptr;
    }

    bool Shaders() {
        if (s_vertexShader && s_pixelShader) {
            return true;
        }

        if (s_shadersFailed) {
            return false;
        }

        if (!s_vertexShader) {
            g_theGxDevicePtr->ShaderCreate(&s_vertexShader, GxSh_Vertex, "Shaders\\Vertex", "SxcWater", 1);
        }

        if (!s_pixelShader) {
            g_theGxDevicePtr->ShaderCreate(&s_pixelShader, GxSh_Pixel, "Shaders\\Pixel", "SxcWater", 1);
        }

        if (!s_vertexShader || !s_pixelShader || !s_vertexShader->Valid() || !s_pixelShader->Valid()) {
            s_shadersFailed = true;
            SXC::LogOnce("water-shaders", "water: the device has no SxcWater programs; the stock water draws");
            return false;
        }

        return true;
    }

    // The ripples' clock in seconds, wrapping every 600 s (Water.frag's frequencies are whole turns per
    // 600 s). A high-resolution clock: the client's millisecond snapshot can step by the system tick
    // (~16 ms), which the ripples showed as ticking (Seyri, 2026-10-28).
    float RippleTime() {
        using Clock = std::chrono::steady_clock;
        static const Clock::time_point s_start = Clock::now();
        const double seconds = std::chrono::duration<double>(Clock::now() - s_start).count();
        return static_cast<float>(seconds - 600.0 * static_cast<double>(static_cast<uint64_t>(seconds / 600.0)));
    }

    SXC::Settings::Bool& WavesOn() {
        static SXC::Settings::Bool s_setting("Liquid", "Waves", true, "Waves (FFT)",
                                             "Our water's waves: an FFT sea driven by the wind around the player (off: small ripples).");
        return s_setting;
    }

    // The wind the waves take: the mod's at the player (the grass's wind grid), smoothed over a few
    // seconds so gusts don't make the spectrum again every frame; a light breeze without the mod.
    void WindForWaves(SXC::Water::Waves& w, float dt) {
        static float s_x = 0.3f, s_y = 0.0f;
        float x = 0.3f, y = 0.0f, gust = 0.0f;

        if (SXC::Grass::WindAtPlayer(x, y, gust)) {
            const float length = std::sqrt(x * x + y * y);

            if (length < 0.05f) {
                // Calm: keep the last direction with a breath of wind.
                const float last = std::sqrt(s_x * s_x + s_y * s_y);
                x = last > 1e-4f ? s_x / last * 0.05f : 0.05f;
                y = last > 1e-4f ? s_y / last * 0.05f : 0.0f;
            }
        }

        const float blend = 1.0f - std::exp(-dt / 4.0f);
        s_x += (x - s_x) * blend;
        s_y += (y - s_y) * blend;
        const float strength = std::sqrt(s_x * s_x + s_y * s_y);
        w.windX = strength > 1e-4f ? s_x / strength : 1.0f;
        w.windY = strength > 1e-4f ? s_y / strength : 0.0f;
        // Strength 0..1 (more in storms) to a wind speed: a breeze to a strong wind.
        w.speed = 2.5f + 11.0f * std::min(strength, 1.5f);
    }

    void SkyRow(float* row, const CImVector& c) {
        row[0] = c.r / 255.0f;
        row[1] = c.g / 255.0f;
        row[2] = c.b / 255.0f;
        row[3] = 1.0f;
    }
}

namespace SXC::Water {
    void SetDevice(bool available, CaptureFn capture, StatusFn status) {
        s_available = available && capture;
        s_capture = capture;
        s_status = status;
        s_shadersFailed = false;
    }

    const char* Status() {
        return s_status ? s_status() : "";
    }

    bool Active() {
        return s_available && OurShader().Get();
    }

    bool DrawOurs(Liquid::CInstance* instance, const C3Vector& cameraPos, Reason& why);

    bool Draw(Liquid::CInstance* instance, const C3Vector& cameraPos) {
        // The Water panel's counts, per second: who drew what and why the stock material kept a draw.
        const float now = RippleTime();

        if (now < s_counts.since || now - s_counts.since >= 1.0f) {
            s_shown = s_counts;
            s_counts = Counts();
            s_counts.since = now;
        }

        Reason why = Reason::Inactive;
        const bool ours = DrawOurs(instance, cameraPos, why);

        if (ours) {
            s_counts.ours[std::min(StockWaterId(instance->m_material), 3u)]++;
            return true;
        }

        s_counts.stock[static_cast<int>(why)]++;
        const uint32_t id = MaterialId(instance ? instance->m_material : nullptr);
        s_counts.stockMaterials[std::min(id, 7u)]++;
        // The panel's test switch: what we don't draw isn't drawn at all.
        return s_hideStock;
    }

    bool DrawOurs(Liquid::CInstance* instance, const C3Vector& cameraPos, Reason& why) {
        if (!Active() || !instance) {
            why = Reason::Inactive;
            return false;
        }

        const uint32_t stockId = StockWaterId(instance->m_material);

        if (!stockId || !instance->m_settings || !instance->m_geomFactory || !instance->m_environment) {
            why = Reason::NotWater;
            return false;
        }

        CGxTex* tint = TintTexture(instance->m_settings);

        if (!tint) {
            why = Reason::NoTint;
            return false;
        }

        if (!Shaders()) {
            why = Reason::Shaders;
            return false;
        }

        why = Reason::Capture;

        // The scene behind the water and the waves, before the frame's first water draw (the later ones
        // reuse them; the device answers once per frame).
        static float s_lastTime = -1.0f;
        SXC::Water::Waves waves;
        waves.on = WavesOn().Get();
        waves.time = RippleTime();
        float dt = s_lastTime < 0.0f ? 0.0f : waves.time - s_lastTime;
        dt = dt < 0.0f ? 0.0f : (dt > 0.5f ? 0.5f : dt);
        s_lastTime = waves.time;
        WindForWaves(waves, dt);
        waves.amplitude = s_look.amplitude;
        waves.chop = s_look.chop;
        waves.spread = s_look.spread;
        waves.foam = s_look.foam;
        waves.move = s_look.move;

        if (s_look.windOverride) {
            const float angle = s_look.windAngle * 0.017453292f;
            waves.windX = std::cos(angle);
            waves.windY = std::sin(angle);
            waves.speed = s_look.windSpeed;
        }

        waves.rebuild = s_rebuildWaves;
        s_rebuildWaves = false;

        // The sky's bodies as drawn this frame (the mod's, or the stock sun and moons): their reflections,
        // rather than the light's direction, which needn't follow the visible sun.
        SXC::Sky::BodySeen seen[4];
        waves.bodies = SXC::Sky::BodiesSeen(seen, 4);

        for (uint32_t b = 0; b < waves.bodies; b++) {
            waves.body[b][0] = seen[b].dir[0];
            waves.body[b][1] = seen[b].dir[1];
            waves.body[b][2] = seen[b].dir[2];
            waves.body[b][3] = seen[b].sizeDegrees * 0.5f * 0.017453292f;
            waves.body[b][4] = seen[b].colour[0];
            waves.body[b][5] = seen[b].colour[1];
            waves.body[b][6] = seen[b].colour[2];
        }

        if (!s_capture(waves)) {
            return false;
        }

        // The format the stock material builds the mesh in (ProcWater PT2, the old water PNCT2).
        const EGxVertexBufferFormat format = stockId == 3 ? GxVBF_PT2 : GxVBF_PNCT2;
        // The depth coordinate's scale, where each kind of row keeps it.
        const float depthScale = instance->m_settings->GetFloat(stockId == 3 ? 8 : 2);

        g_theGxDevicePtr->RsPush();

        CGxBuf* vertexBuf = nullptr;
        CGxBuf* indexBuf = nullptr;
        CGxBatch batch;
        batch.m_primType = GxPrims_Last;
        batch.m_start = 0;
        batch.m_count = 0;
        batch.m_minIndex = 0;
        batch.m_maxIndex = 0;

        bool identity = instance->m_geomFactory->SetTransform(&instance->m_transform);
        instance->m_geomFactory->GetBuffers(format, &vertexBuf, &indexBuf, &batch);

        if (!vertexBuf || !indexBuf) {
            g_theGxDevicePtr->RsPop();
            return true;
        }

        GxPrimVertexPtr(vertexBuf, format);
        g_theGxDevicePtr->PrimIndexPtr(indexBuf);

        g_theGxDevicePtr->RsSet(GxRs_Culling, 0);
        g_theGxDevicePtr->RsSet(GxRs_DepthWrite, SXC::Fog::WaterWritesDepth() ? 1 : 0);
        g_theGxDevicePtr->RsSet(GxRs_BlendingMode, 2);
        GxUpdateAlphaRef(GxRs_AlphaRef);
        g_theGxDevicePtr->RsSet(GxRs_Fog, 1);

        Liquid::s_vsConstants.m_projection = g_theGxDevicePtr->m_projNative;

        C44Matrix matrix;
        Liquid::SetShaderTransforms(cameraPos, identity ? matrix : instance->m_transform);

        CM2Lighting lighting(instance->m_bounds);
        instance->m_environment->SelectLights(&lighting);
        Liquid::SetShaderLighting(lighting);

        // Texture transform 1 takes the depth coordinate (the stock vsLiquidWater's c13..c16).
        C44Matrix depthMatrix;
        depthMatrix.b1 = depthScale;
        Liquid::s_vsConstants.m_texTransforms[1] = depthMatrix;

        g_theGxDevicePtr->RsSet(GxRs_Texture0, tint);
        Liquid::SetShaderConstants();

        // Ours past the liquid's (Water.vert, step 3b): c46..c49 the mesh's world transform (camera-
        // relative when the liquid code works that way), c50..c53 the view, c54 what the world transform
        // leaves out of the world position (the camera's, when camera-relative): the vertex stage moves
        // the surface in the world and then views it.
        {
            C44Matrix world = identity ? C44Matrix() : instance->m_transform;
            C3Vector offset(0.0f, 0.0f, 0.0f);

            if (Liquid::UseCameraRelativeCoords()) {
                world.d0 -= cameraPos.x;
                world.d1 -= cameraPos.y;
                world.d2 -= cameraPos.z;
                offset = cameraPos;
            }

            C44Matrix view;
            GxXformView(view);
            float vertexRows[9 * 4];
            std::memcpy(&vertexRows[0], &world, sizeof(world));
            std::memcpy(&vertexRows[16], &view, sizeof(view));
            vertexRows[32] = offset.x;
            vertexRows[33] = offset.y;
            vertexRows[34] = offset.z;
            vertexRows[35] = 0.0f;
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 46, vertexRows, 9);
        }

        float rows[kPixelRows * 4] = {};
        rows[0] = RippleTime();
        rows[1] = static_cast<float>(s_look.debugView);
        rows[2] = kRippleStrength;
        const DayNight::DNInfo* info = DayNight::GetInfo();

        if (info) {
            SkyRow(&rows[4], info->m_bands.m_skyColors[0]);
            SkyRow(&rows[8], info->m_bands.m_skyColors[4]);
        }

        const float look[12] = {
            s_look.clarityClear, s_look.clarityMurky, s_look.scatter, s_look.refraction,
            s_look.reflectivity, s_look.reflection, s_look.window, s_look.screenReflections ? 1.0f : 0.0f,
            s_look.glint, s_look.glintSharpness, s_look.foamThreshold, 0.0f,
        };
        std::memcpy(&rows[12], look, sizeof(look));

        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, kPixelFirst, rows, kPixelRows);

        g_theGxDevicePtr->RsSet(GxRs_VertexShader, s_vertexShader);
        g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_pixelShader);
        g_theGxDevicePtr->Draw(&batch, 1);
        g_theGxDevicePtr->RsPop();
        return true;
    }

    void DrawPanel(void*) {
        const Look defaults;
        bool on = OurShader().Get();

        if (ImGui::Checkbox("Our water", &on)) {
            OurShader().Set(on);
        }

        ImGui::SameLine();
        bool wavesOn = WavesOn().Get();

        if (ImGui::Checkbox("FFT waves", &wavesOn)) {
            WavesOn().Set(wavesOn);
        }

        ImGui::TextDisabled("device: %s", s_available ? Status() : "not available (D3D9, or the lights' set is off)");

        // Who drew the liquids in the last second.
        const Counts& c = s_shown;
        ImGui::Text("Ours /s: material 1: %u, material 3: %u", c.ours[1], c.ours[3]);
        ImGui::Text("Stock /s: off %u, not water %u, no tint %u, no shaders %u, no scene copy %u", c.stock[0], c.stock[1], c.stock[2], c.stock[3], c.stock[4]);
        ImGui::TextDisabled("  stock by material: 0: %u  1: %u  2: %u  3: %u  4+: %u", c.stockMaterials[0], c.stockMaterials[1], c.stockMaterials[2],
                            c.stockMaterials[3], c.stockMaterials[4] + c.stockMaterials[5] + c.stockMaterials[6] + c.stockMaterials[7]);
        ImGui::Checkbox("Hide everything not drawn by us (test)", &s_hideStock);

        if (ImGui::Button("Rebuild waves")) {
            s_rebuildWaves = true;
        }
        ImGui::TextWrapped("Live values for tuning, not saved: they go into the LiquidSurface table per water later.");

        static const char* const kViews[] = { "Off", "Reflection hits (red: sky)", "Water crossed (white 20 yd)", "Normal",
                                              "Folding / foam", "Reflection alone", "Scene behind", "Screen mapping check (black: right)",
                                              "Reflection search outcome (blue: sky on screen)", "Depth conversion self-test (black: right)" };
        ImGui::Combo("Debug view", &s_look.debugView, kViews, IM_ARRAYSIZE(kViews));

        if (ImGui::CollapsingHeader("Water", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Absorption, clear (/yd)", &s_look.clarityClear, 0.0f, 0.2f, "%.3f");
            ImGui::SliderFloat("Absorption, murky (/yd)", &s_look.clarityMurky, 0.0f, 0.6f, "%.3f");
            ImGui::SliderFloat("Own colour (x tint)", &s_look.scatter, 0.0f, 1.5f);
            ImGui::SliderFloat("Refraction", &s_look.refraction, 0.0f, 3.0f);
        }

        if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Reflectivity (straight down)", &s_look.reflectivity, 0.0f, 0.5f, "%.3f");
            ImGui::SliderFloat("Reflection strength", &s_look.reflection, 0.0f, 4.0f);
            ImGui::Checkbox("Screen-space reflections", &s_look.screenReflections);
            ImGui::SliderFloat("Window from below (cos)", &s_look.window, 0.0f, 0.9f);
            ImGui::SliderFloat("Sun glint", &s_look.glint, 0.0f, 6.0f);
            ImGui::SliderFloat("Glint sharpness", &s_look.glintSharpness, 20.0f, 2000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        }

        if (ImGui::CollapsingHeader("Waves", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Height", &s_look.amplitude, 0.0f, 2.0f);
            ImGui::SliderFloat("Surface movement", &s_look.move, 0.0f, 2.0f);
            ImGui::SliderFloat("Choppiness", &s_look.chop, 0.0f, 3.0f);
            ImGui::SliderFloat("Spread", &s_look.spread, 0.0f, 1.0f);
            ImGui::SliderFloat("Foam", &s_look.foam, 0.0f, 3.0f);
            ImGui::SliderFloat("Foam threshold", &s_look.foamThreshold, 0.0f, 1.5f);
            ImGui::Checkbox("Override the wind", &s_look.windOverride);

            if (s_look.windOverride) {
                ImGui::SliderFloat("Wind speed (yd/s)", &s_look.windSpeed, 0.5f, 25.0f);
                ImGui::SliderFloat("Wind direction", &s_look.windAngle, 0.0f, 360.0f, "%.0f deg");
            }
        }

        if (ImGui::Button("Reset to defaults")) {
            const int view = s_look.debugView;
            s_look = defaults;
            s_look.debugView = view;
        }
    }
}
