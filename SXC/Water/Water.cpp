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
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Storm/H/storm.h"
#include "SXC/Fog/Fog.h"
#include "SXC/Grass/Grass.h"
#include "SXC/Log.h"
#include "SXC/Settings/Settings.h"
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

    // Our pixel registers: p6 = (time, 0, ripple strength, 0), p7 the sky's zenith, p8 its horizon.
    constexpr uint32_t kPixelFirst = 6;
    constexpr uint32_t kPixelRows = 3;
    constexpr float kRippleStrength = 1.0f;

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

    bool Draw(Liquid::CInstance* instance, const C3Vector& cameraPos) {
        if (!Active() || !instance) {
            return false;
        }

        const uint32_t stockId = StockWaterId(instance->m_material);

        if (!stockId || !instance->m_settings || !instance->m_geomFactory || !instance->m_environment) {
            return false;
        }

        CGxTex* tint = TintTexture(instance->m_settings);

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

        if (!tint || !Shaders() || !s_capture(waves)) {
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

        float rows[kPixelRows * 4] = {};
        rows[0] = RippleTime();
        rows[2] = kRippleStrength;
        const DayNight::DNInfo* info = DayNight::GetInfo();

        if (info) {
            SkyRow(&rows[4], info->m_bands.m_skyColors[0]);
            SkyRow(&rows[8], info->m_bands.m_skyColors[4]);
        }

        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, kPixelFirst, rows, kPixelRows);

        g_theGxDevicePtr->RsSet(GxRs_VertexShader, s_vertexShader);
        g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_pixelShader);
        g_theGxDevicePtr->Draw(&batch, 1);
        g_theGxDevicePtr->RsPop();
        return true;
    }
}
