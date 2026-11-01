// Our water (SXC/Water/Water.h): the stock water materials' instances drawn through the "SxcWater" pair,
// which only our Vulkan device knows (Shaders/Water.vert, Water.frag). The draw follows the stock
// materials' own (MaterialWater.cpp, MaterialProcWater.cpp): the instance's mesh in the vertex format
// its stock material asks for (the terrain's batches keep that format once built), the liquid code's
// constants, then ours.
#include "SXC/Water/Water.h"
#include "SXC/Water/Medium.h"
#include "SXC/Water/Ride.h"
#include "SXC/Water/Shore.h"
#include "SXC/Water/Surface.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Storm/H/storm.h"
#include "SXC/Grass/Grass.h"
#include "SXC/Sky/Sky.h"
#include "SXC/Log.h"
#include "SXC/Settings/Settings.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/LiquidGeomFactories.h"

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
    // p8 its horizon, p9..p13 the draw's own look (Surface.h's PackLook), p14 = (screen-space reflections,
    // the sea's foam threshold, 1 when the look comes from the look grid, 1 for terrain water: the shore
    // field applies). Not p15: the device fills it with the fixed-function fog for every draw.
    constexpr uint32_t kPixelFirst = 6;
    constexpr uint32_t kPixelRows = 9;
    constexpr float kRippleStrength = 1.0f;

    // The Water panel: test values over every water while `test` is on (not saved), and what isn't per
    // water (the wind override, screen-space reflections, the debug view).
    struct Panel {
        bool test = false;
        SXC::Water::Look look;
        bool screenReflections = true;
        bool windOverride = false;    // the panel's wind instead of the mod's
        float windSpeed = 6.0f;       // yd / s
        float windAngle = 0.0f;       // degrees, 0 = +x
        int debugView = 0;
        int causticDebug = 0;          // the terrain's caustics debug view
        int writeScope = 0;           // SXC::Water::ScopeKind: where "Write" puts the row
        // The shore (one setting for every water for now).
        bool shoreField = true;          // off: the depth byte alone, as before the shore field
        bool nearMesh = true;            // our own water mesh over the shore field (off: the stock mesh, pinned)
    };
    Panel s_panel;

    // The look a water draws with at a world point: the panel's test values, else its LiquidSurface row there.
    const SXC::Water::Look& LookFor(uint32_t liquidType, float x, float y) {
        return s_panel.test ? s_panel.look : SXC::Water::SurfaceLookAt(liquidType, x, y);
    }

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

    const SXC::Water::Look& FillWaves(SXC::Water::Waves& waves, const C3Vector& cameraPos);

    void Frame(const C3Vector& cameraPos) {
        if (!Active() || !s_capture || !Shaders()) {
            return;
        }

        SXC::Water::Waves waves;
        FillWaves(waves, cameraPos);
        waves.prepareOnly = true;
        s_capture(waves, nullptr);
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

    // What a water draw needs beyond its mesh.
    struct Setup {
        CGxTex* tint = nullptr;
        float depthScale = 1.0f;
        bool identity = true;
        const C44Matrix* transform = nullptr;   // the mesh's, unless identity
        Liquid::IEnvironment* environment = nullptr;
        CAaSphere bounds;
        uint32_t liquidType = 0;
        float lookX = 0.0f, lookY = 0.0f;       // where its own look is taken
        float terrain = 0.0f;                   // p14.w: 0 WMO, 1 terrain, 2 terrain giving way to the near mesh
        bool nearMesh = false;                  // the near mesh (v55)
    };

    // The near mesh drew this frame (the stock terrain water gives way where it covers).
    bool s_nearDrawn = false;

    // The state, the liquid code's constants and ours, then the draw (the mesh's buffers already set).
    void Submit(const Setup& d, const C3Vector& cameraPos, const SXC::Water::Look& sea, bool gridValid, CGxBatch& batch) {
        g_theGxDevicePtr->RsSet(GxRs_Culling, 0);
        // Opaque to the depth buffer like any solid surface: our water covers what lies behind it itself (the
        // scene's copy), and its moving surface hides parts of itself. Without writing depth, whichever patch
        // the (unsorted) liquid list drew last painted over the others: chunk-shaped steps of the wrong wave,
        // waves behind a character drawn over her (Seyri, 2026-10-29). The test explicitly too: the liquid
        // pass inherits it from whatever drew before.
        g_theGxDevicePtr->RsSet(GxRs_DepthTest, 1);
        g_theGxDevicePtr->RsSet(GxRs_DepthWrite, 1);
        g_theGxDevicePtr->RsSet(GxRs_BlendingMode, 2);
        GxUpdateAlphaRef(GxRs_AlphaRef);
        g_theGxDevicePtr->RsSet(GxRs_Fog, 1);

        Liquid::s_vsConstants.m_projection = g_theGxDevicePtr->m_projNative;

        C44Matrix matrix;
        Liquid::SetShaderTransforms(cameraPos, d.identity ? matrix : *d.transform);

        CM2Lighting lighting(d.bounds);
        d.environment->SelectLights(&lighting);
        Liquid::SetShaderLighting(lighting);

        // Texture transform 1 takes the depth coordinate (the stock vsLiquidWater's c13..c16).
        C44Matrix depthMatrix;
        depthMatrix.b1 = d.depthScale;
        Liquid::s_vsConstants.m_texTransforms[1] = depthMatrix;

        g_theGxDevicePtr->RsSet(GxRs_Texture0, d.tint);
        Liquid::SetShaderConstants();

        // Ours past the liquid's (Water.vert, step 3b): c46..c49 the mesh's world transform (camera-
        // relative when the liquid code works that way), c50..c53 the view, c54 what the world transform
        // leaves out of the world position (the camera's, when camera-relative): the vertex stage moves
        // the surface in the world and then views it. c55 = (1 for the near mesh, its LiquidType).
        {
            C44Matrix world = d.identity ? C44Matrix() : *d.transform;
            C3Vector offset(0.0f, 0.0f, 0.0f);

            if (Liquid::UseCameraRelativeCoords()) {
                world.d0 -= cameraPos.x;
                world.d1 -= cameraPos.y;
                world.d2 -= cameraPos.z;
                offset = cameraPos;
            }

            C44Matrix view;
            GxXformView(view);
            float vertexRows[10 * 4] = {};
            std::memcpy(&vertexRows[0], &world, sizeof(world));
            std::memcpy(&vertexRows[16], &view, sizeof(view));
            vertexRows[32] = offset.x;
            vertexRows[33] = offset.y;
            vertexRows[34] = offset.z;
            vertexRows[36] = d.nearMesh ? 1.0f : 0.0f;
            vertexRows[37] = static_cast<float>(d.liquidType);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 46, vertexRows, 10);
        }

        float rows[kPixelRows * 4] = {};
        rows[0] = RippleTime();
        rows[1] = static_cast<float>(s_panel.debugView);
        rows[2] = kRippleStrength;
        const DayNight::DNInfo* info = DayNight::GetInfo();

        if (info) {
            SkyRow(&rows[4], info->m_bands.m_skyColors[0]);
            SkyRow(&rows[8], info->m_bands.m_skyColors[4]);
        }

        // The draw's own look, where it lies (WMO water, the panel's tests); terrain water reads the look
        // grid per vertex instead, blended between places.
        SXC::Water::PackLook(LookFor(d.liquidType, d.lookX, d.lookY), &rows[12]);
        rows[32] = s_panel.screenReflections ? 1.0f : 0.0f;
        rows[33] = sea.foamThreshold;
        rows[34] = d.terrain > 0.0f && !s_panel.test && gridValid ? 1.0f : 0.0f;
        rows[35] = d.terrain;

        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, kPixelFirst, rows, kPixelRows);

        g_theGxDevicePtr->RsSet(GxRs_VertexShader, s_vertexShader);
        g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_pixelShader);
        g_theGxDevicePtr->Draw(&batch, 1);
    }

    // The near mesh's grid: a vertex per cell of the shore field's window (its position the cell in the
    // window), two triangles per square; made once, filled again if the device lost it.
    CGxBuf* s_nearVertices = nullptr;
    CGxBuf* s_nearIndices = nullptr;
    constexpr uint32_t kNear = static_cast<uint32_t>(SXC::Water::kShoreSize);
    constexpr EGxVertexBufferFormat kNearFormat = GxVBF_PNCT2;

    bool NearBuffers() {
        const uint32_t vertexCount = kNear * kNear, indexCount = (kNear - 1) * (kNear - 1) * 6;

        if (!s_nearVertices) {
            const uint32_t stride = GxVertexSize(kNearFormat);
            CGxPool* vertexPool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Vertex, GxPoolUsage_Dynamic, stride * vertexCount, GxPoolHintBit_Unk3, "SxcWaterNear_vtx");
            CGxPool* indexPool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Index, GxPoolUsage_Dynamic, 2 * indexCount, GxPoolHintBit_Unk3, "SxcWaterNear_idx");
            s_nearVertices = g_theGxDevicePtr->BufCreate(vertexPool, stride, vertexCount, 0);
            s_nearIndices = g_theGxDevicePtr->BufCreate(indexPool, 2, indexCount, 0);
        }

        if (!s_nearVertices || !s_nearIndices) {
            return false;
        }

        if (!s_nearVertices->m_hasData || !s_nearVertices->m_valid) {
            char* data = g_theGxDevicePtr->BufLock(s_nearVertices);

            if (!data) {
                return false;
            }

            const uint32_t stride = GxVertexSize(kNearFormat);
            std::memset(data, 0, static_cast<size_t>(stride) * vertexCount);
            const uint32_t position = GxVertexAttribOffset(kNearFormat, GxVA_Position), normal = GxVertexAttribOffset(kNearFormat, GxVA_Normal),
                           colour = GxVertexAttribOffset(kNearFormat, GxVA_Color0);

            for (uint32_t j = 0; j < kNear; j++) {
                for (uint32_t i = 0; i < kNear; i++) {
                    char* v = data + static_cast<size_t>(j * kNear + i) * stride;
                    const float p[3] = { static_cast<float>(i), static_cast<float>(j), 0.0f }, n[3] = { 0.0f, 0.0f, 1.0f };
                    const uint32_t white = 0xFFFFFFFFu;
                    std::memcpy(v + position, p, sizeof(p));
                    std::memcpy(v + normal, n, sizeof(n));
                    std::memcpy(v + colour, &white, sizeof(white));
                }
            }

            g_theGxDevicePtr->BufUnlock(s_nearVertices, 0);
            s_nearVertices->m_hasData = 1;
        }

        if (!s_nearIndices->m_hasData || !s_nearIndices->m_valid) {
            uint16_t* indices = reinterpret_cast<uint16_t*>(g_theGxDevicePtr->BufLock(s_nearIndices));

            if (!indices) {
                return false;
            }

            for (uint32_t j = 0; j + 1 < kNear; j++) {
                for (uint32_t i = 0; i + 1 < kNear; i++) {
                    const uint16_t a = static_cast<uint16_t>(j * kNear + i), b = static_cast<uint16_t>(a + 1);
                    const uint16_t c = static_cast<uint16_t>(a + kNear), e = static_cast<uint16_t>(c + 1);
                    *indices++ = a;
                    *indices++ = b;
                    *indices++ = c;
                    *indices++ = b;
                    *indices++ = e;
                    *indices++ = c;
                }
            }

            g_theGxDevicePtr->BufUnlock(s_nearIndices, 0);
            s_nearIndices->m_hasData = 1;
        }

        return true;
    }

    // The near mesh, once per water type it covers, at the frame's first water draw (`from`: the instance
    // whose environment picks the lights).
    void DrawNear(Liquid::CInstance* from, const C3Vector& cameraPos, const SXC::Water::ShoreMap& shore, const SXC::Water::Look& sea, bool gridValid) {
        if (!s_panel.nearMesh || !s_panel.shoreField || !shore.valid || !shore.typeCount || !NearBuffers()) {
            return;
        }

        float playerX = 0.0f, playerY = 0.0f;
        SXC::Water::PlayerPosition(playerX, playerY);

        for (uint32_t t = 0; t < shore.typeCount; t++) {
            const uint32_t type = shore.types[t];
            Liquid::CMaterialSettings* settings = Liquid::CSettingsBank::GetSettings(type);
            const uint32_t stockId = StockWaterId(Liquid::CMaterialBank::GetMaterial(type));
            CGxTex* tint = settings && stockId ? TintTexture(settings) : nullptr;

            if (!tint) {
                continue;   // not one of our waters (magma, slime): the stock mesh keeps it
            }

            g_theGxDevicePtr->RsPush();
            GxPrimVertexPtr(s_nearVertices, kNearFormat);
            g_theGxDevicePtr->PrimIndexPtr(s_nearIndices);
            CGxBatch batch;
            batch.m_primType = GxPrim_Triangles;
            batch.m_start = 0;
            batch.m_count = (kNear - 1) * (kNear - 1) * 6;
            batch.m_minIndex = 0;
            batch.m_maxIndex = static_cast<uint16_t>(kNear * kNear - 1);

            Setup d;
            d.tint = tint;
            d.depthScale = settings->GetFloat(stockId == 3 ? 8 : 2);
            d.environment = from->m_environment;
            d.bounds = CAaSphere();
            d.bounds.c = C3Vector(playerX, playerY, cameraPos.z);
            d.bounds.r = 300.0f;
            d.liquidType = type;
            d.lookX = playerX;
            d.lookY = playerY;
            d.terrain = 1.0f;
            d.nearMesh = true;
            Submit(d, cameraPos, sea, gridValid, batch);
            g_theGxDevicePtr->RsPop();
            s_nearDrawn = true;
        }
    }

    // The frame's water as the device takes it (the waves, the looks, the shore, the caustics, the swimmers),
    // from any water draw and from the liquid pass's start (Frame); the sea's look (the water the player is in).
    const SXC::Water::Look& FillWaves(SXC::Water::Waves& waves, const C3Vector& cameraPos) {
        static float s_lastTime = -1.0f;
        waves.on = WavesOn().Get();
        waves.time = RippleTime();
        float dt = s_lastTime < 0.0f ? 0.0f : waves.time - s_lastTime;
        dt = dt < 0.0f ? 0.0f : (dt > 0.5f ? 0.5f : dt);
        s_lastTime = waves.time;
        WindForWaves(waves, dt);
        // The shared sea's spectrum from the water the player is in (looked up four times a second).
        static uint32_t s_typeHere = 0;
        static auto s_typeChecked = std::chrono::steady_clock::time_point();
        const auto now = std::chrono::steady_clock::now();

        if (now - s_typeChecked > std::chrono::milliseconds(250)) {
            s_typeChecked = now;
            s_typeHere = SXC::Water::LiquidTypeHere();
        }

        float playerX = 0.0f, playerY = 0.0f;
        SXC::Water::PlayerPosition(playerX, playerY);
        const SXC::Water::Look& sea = LookFor(s_typeHere, playerX, playerY);
        waves.amplitude = sea.amplitude;
        waves.chop = sea.chop;
        waves.spread = sea.spread;
        waves.foam = sea.foam;
        waves.move = sea.move;

        if (s_panel.windOverride) {
            const float angle = s_panel.windAngle * 0.017453292f;
            waves.windX = std::cos(angle);
            waves.windY = std::sin(angle);
            waves.speed = s_panel.windSpeed;
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

        // The looks of terrain water by place (none while the panel tests its values over every water).
        const SXC::Water::LookGrid& grid = SXC::Water::SurfaceGrid();

        if (!s_panel.test && grid.valid) {
            waves.lookIndex = grid.index;
            waves.lookPalette = &grid.palette[0][0];
            waves.lookCount = grid.paletteCount;
            waves.lookOriginX = grid.originX;
            waves.lookOriginY = grid.originY;
        }

        // The shore field around the player (terrain liquid).
        const SXC::Water::ShoreMap& shore = SXC::Water::ShoreField();

        if (s_panel.shoreField && shore.valid) {
            waves.shore = shore.texels;
            waves.shoreCells = shore.cells;
            waves.shoreVersion = shore.version;
            waves.shoreOriginX = shore.originX;
            waves.shoreOriginY = shore.originY;
        }

        // The shore's values, like the sea's, from the row of the water the player is in.
        waves.shoreFoam = sea.shoreFoam;
        waves.shoreWaveHeight = sea.shoreWaveHeight;
        waves.shoreWavelength = sea.shoreWavelength;
        waves.shoreReach = sea.shoreReach;
        waves.shoreSwash = sea.swash;
        waves.caustics = sea.caustics;
        waves.causticScale = sea.causticScale;
        waves.causticDepth = sea.causticDepth;

        // The sun for the caustics, in the world (the light's direction is the way it travels), and the
        // weather's cloud (the storm blend: the direct light goes, so do they).
        if (const DayNight::DNInfo* day = DayNight::GetInfo()) {
            const C3Vector& dir = day->m_light0.m_dir;
            const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);

            if (length > 1e-4f) {
                waves.sun[0] = -dir.x / length;
                waves.sun[1] = -dir.y / length;
                waves.sun[2] = -dir.z / length;
            }

            waves.clouds = std::clamp(day->m_stormBlend, 0.0f, 1.0f);
            waves.sunColour[0] = day->m_light1.m_diffuse.r / 255.0f;
            waves.sunColour[1] = day->m_light1.m_diffuse.g / 255.0f;
            waves.sunColour[2] = day->m_light1.m_diffuse.b / 255.0f;
        }

        waves.causticDebug = s_panel.causticDebug;
        SXC::Water::MediumGlow(waves.underGlow);   // the deep the surface mirrors from below

        // The swimmers near the camera, to ride the waves (their answers come back a few frames on). Their
        // movement row without the look grid is the water's the player is in, as the near mesh draws it.
        // The terrain's waterline takes the same row and camera (its swash rises and falls with the waves).
        float packed[SXC::Water::kPackedLook];
        SXC::Water::PackLook(sea, packed);
        waves.queryCamera[0] = cameraPos.x;
        waves.queryCamera[1] = cameraPos.y;
        waves.queryCamera[2] = cameraPos.z;
        waves.queryCamera[3] = waves.time;
        std::memcpy(waves.queryLook, &packed[16], sizeof waves.queryLook);
        SXC::Water::RideAsk(waves);

        return sea;
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
        SXC::Water::Waves waves;
        const SXC::Water::Look& sea = FillWaves(waves, cameraPos);
        const SXC::Water::LookGrid& grid = SXC::Water::SurfaceGrid();
        const SXC::Water::ShoreMap& shore = SXC::Water::ShoreField();

        bool first = false;

        if (!s_capture(waves, &first)) {
            return false;
        }

        // Our own water mesh near the player, ahead of the frame's stock terrain water.
        if (first) {
            s_nearDrawn = false;
            DrawNear(instance, cameraPos, shore, sea, grid.valid);
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

        Setup d;
        d.tint = tint;
        d.depthScale = depthScale;
        d.identity = identity;
        d.transform = &instance->m_transform;
        d.environment = instance->m_environment;
        d.bounds = instance->m_bounds;
        d.liquidType = SXC::Water::LiquidTypeOf(instance->m_settings);
        d.lookX = instance->m_bounds.c.x;
        d.lookY = instance->m_bounds.c.y;
        const bool terrain = dynamic_cast<Liquid::CChunkGeomFactory*>(instance->m_geomFactory) != nullptr;
        d.terrain = terrain ? (s_nearDrawn ? 2.0f : 1.0f) : 0.0f;
        Submit(d, cameraPos, sea, grid.valid, batch);
        g_theGxDevicePtr->RsPop();
        return true;
    }

    void DrawPanel(void*) {
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
        // The look: each water's LiquidSurface row, or the test values below over every water.
        const uint32_t here = SXC::Water::LiquidTypeHere();
        const LiquidTypeRec* hereRec = here ? g_liquidTypeDB.GetRecord(here) : nullptr;
        ImGui::Separator();
        ImGui::Text("Water here: %s", here ? (hereRec && hereRec->m_name ? hereRec->m_name : "?") : "none");

        if (here) {
            ImGui::SameLine();
            ImGui::TextDisabled("(LiquidType %u)", here);
        }

        ImGui::Checkbox("Test values over every water (not saved)", &s_panel.test);

        if (here && ImGui::Button("Load the look here")) {
            float x = 0.0f, y = 0.0f;
            SXC::Water::PlayerPosition(x, y);
            s_panel.look = SXC::Water::SurfaceLookAt(here, x, y);
            s_panel.test = true;
        }

        if (here) {
            // Where the row goes: the places the player is in.
            std::string scopes[5], labels[5];
            const char* names[5] = {};
            int kinds[5] = {}, count = 0, chosen = 0;

            for (int k = 0; k < 5; k++) {
                if (SXC::Water::ScopeHere(static_cast<SXC::Water::ScopeKind>(k), scopes[count], labels[count])) {
                    kinds[count] = k;
                    names[count] = labels[count].c_str();
                    chosen = k == s_panel.writeScope ? count : chosen;
                    count++;
                }
            }

            if (ImGui::Combo("Row for", &chosen, names, count)) {
                s_panel.writeScope = kinds[chosen];
            }

            if (ImGui::Button("Write the test values to that row")) {
                if (SXC::Water::WriteSurfaceRow(here, scopes[chosen], s_panel.look)) {
                    s_panel.test = false;
                }
            }

            ImGui::TextDisabled("This LiquidType's row for that place in LiquidSurface (made if missing); save in the Data view.");
        }

        static const char* const kViews[] = { "Off", "Reflection hits (red: sky)", "Water crossed (white 20 yd)", "Normal",
                                              "Folding / foam", "Reflection alone", "Scene behind", "Screen mapping check (black: right)",
                                              "Reflection search outcome (blue: sky on screen)", "Depth conversion self-test (black: right)", "Movement easing (white: moves)",
                                              "Shore field (blue water, red land, green depth)", "Shore waves (red crests, white foam)" };
        ImGui::Combo("Debug view", &s_panel.debugView, kViews, IM_ARRAYSIZE(kViews));
        ImGui::Checkbox("Screen-space reflections", &s_panel.screenReflections);
        ImGui::Checkbox("Override the wind", &s_panel.windOverride);

        if (s_panel.windOverride) {
            ImGui::SliderFloat("Wind speed (yd/s)", &s_panel.windSpeed, 0.5f, 25.0f);
            ImGui::SliderFloat("Wind direction", &s_panel.windAngle, 0.0f, 360.0f, "%.0f deg");
        }

        if (!s_panel.test) {
            ImGui::TextDisabled("The sliders below are the test values; tick the box above to see them.");
        }

        SXC::Water::Look& l = s_panel.look;

        if (ImGui::CollapsingHeader("Water", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Absorption, clear (/yd)", &l.clarityClear, 0.0f, 0.2f, "%.3f");
            ImGui::SliderFloat("Absorption, murky (/yd)", &l.clarityMurky, 0.0f, 0.6f, "%.3f");
            ImGui::SliderFloat("Own colour (x tint)", &l.scatter, 0.0f, 1.5f);
            ImGui::SliderFloat("Refraction", &l.refraction, 0.0f, 3.0f);
        }

        if (ImGui::CollapsingHeader("Colour", ImGuiTreeNodeFlags_DefaultOpen)) {
            static const char* const kModes[] = { "Stock (the light data, through the day)", "Hue (stock brightness, our hue)", "Replace" };
            int mode = static_cast<int>(l.colourMode);

            if (ImGui::Combo("Colour mode", &mode, kModes, IM_ARRAYSIZE(kModes))) {
                l.colourMode = static_cast<SXC::Water::ColourMode>(mode);
            }

            ImGui::ColorEdit3("Shallow", l.shallow);
            ImGui::ColorEdit3("Deep", l.deep);
        }

        if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Reflectivity (straight down)", &l.reflectivity, 0.0f, 0.5f, "%.3f");
            ImGui::SliderFloat("Reflection strength", &l.reflection, 0.0f, 4.0f);
            ImGui::SliderFloat("Window from below (cos)", &l.window, 0.0f, 0.9f);
            ImGui::SliderFloat("Glint", &l.glint, 0.0f, 6.0f);
            ImGui::SliderFloat("Glint sharpness", &l.glintSharpness, 20.0f, 2000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        }

        if (ImGui::CollapsingHeader("Waves", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Wave scale (this water)", &l.waveScale, 0.0f, 3.0f);
            ImGui::SliderFloat("Height (the sea)", &l.amplitude, 0.0f, 2.0f);
            ImGui::SliderFloat("Surface movement", &l.move, 0.0f, 2.0f);
            ImGui::SliderFloat("Choppiness (the sea)", &l.chop, 0.0f, 3.0f);
            ImGui::SliderFloat("Spread (the sea)", &l.spread, 0.0f, 1.0f);
            ImGui::SliderFloat("Foam", &l.foam, 0.0f, 3.0f);
            ImGui::SliderFloat("Foam threshold (the sea)", &l.foamThreshold, 0.0f, 1.5f);
        }

        if (ImGui::CollapsingHeader("Shore", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Shore field", &s_panel.shoreField);
            ImGui::SameLine();
            ImGui::Checkbox("Our water mesh near you", &s_panel.nearMesh);
            ImGui::SetItemTooltip("Over the shore field's window, our own mesh in place of the stock terrain water: the waterline is "
                                  "wherever the surface meets the ground. Off: the stock mesh, its edge held still.");
            ImGui::SliderFloat("Shore foam", &l.shoreFoam, 0.0f, 2.0f);
            ImGui::SliderFloat("Shore wave height (yd)", &l.shoreWaveHeight, 0.0f, 0.6f);
            ImGui::SliderFloat("Shore wavelength (yd)", &l.shoreWavelength, 2.0f, 20.0f);
            ImGui::SliderFloat("Shore waves reach (yd)", &l.shoreReach, 4.0f, 60.0f);
            ImGui::SliderFloat("Swash run-up (yd)", &l.swash, 0.0f, 2.0f);
            ImGui::SetItemTooltip("%s", "How high the water runs up the beach as each wave arrives (on the terrain, past the water): a flat beach far, a steep one hardly. 0 off.");
            ImGui::TextDisabled("Test values like the rest; saved with the row (the water you're in sets them for all).");
            ImGui::Separator();
            ImGui::TextUnformatted("Caustics (on the floor under the water)");
            ImGui::SliderFloat("Caustics", &l.caustics, 0.0f, 3.0f);
            ImGui::SetItemTooltip("%s", "The sunlight focused by the waves into a moving net of light on the floor under the water. 0 none (murky water).");
            ImGui::SliderFloat("Caustic size (yd)", &l.causticScale, 0.5f, 10.0f);
            ImGui::SliderFloat("Caustic depth (yd)", &l.causticDepth, 0.5f, 40.0f);
            ImGui::SetItemTooltip("%s", "How deep the caustics reach before they fade out (clear water deep, cloudy water shallow).");
            ImGui::Combo("Caustics debug", &s_panel.causticDebug, "Off\0The pattern alone\0The sun's shadow\0What the terrain gets\0");
            ImGui::SetItemTooltip("%s", "The terrain unlit: the caustics' pattern (white lines), the sun's shadow term (white lit, black shadowed), "
                                        "or what it was handed (red: the shore field, green: the sun's height, blue: caustics on).");
            char ride[192];
            SXC::Water::RideStatus(ride, sizeof ride);
            ImGui::TextWrapped("Riding the waves: %s", ride);
        }

        if (ImGui::Button("Reset the test values")) {
            l = SXC::Water::Look();
        }
    }
}
