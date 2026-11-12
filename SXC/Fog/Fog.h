#pragma once

#include <cstdint>

// SXC's volumetric fog: the sxc.fog service (SXC/Api/sxc_fog.h) holds what a mod sets (the medium and
// the look); the seam in the world render hands it to the renderer the graphics device registered.
// Only the Vulkan device registers one: on D3D9 the client's own fog stays alone.
namespace SXC::Fog {
    // What the renderer gets per frame, beside what it knows itself (its matrices and viewport).
    struct Input {
        float origin[3] = {};     // the camera's world position (the matrices are relative to it)
        float seconds = 0.0f;
        float colour[3] = {};     // the fog's lit colour

        // The near field (sxc_fog_field), as RGBA16F texels; null = none.
        uint32_t fieldSize = 0;
        float cellSize = 2.0f;
        int32_t firstI = 0, firstJ = 0;
        const uint16_t* light = nullptr;
        const uint16_t* pool = nullptr;
        const uint16_t* tint = nullptr;  // the colour per cell (rgb, x tintBrightness); null: `colour` everywhere
        float tintBrightness = 1.0f;
        bool fieldReady = false;         // the pool layer's w: how much the near field shows per cell (else 1)
        uint32_t fieldVersion = 0;

        // The far map (sxc_fog_far), the same layout; null = none.
        uint32_t farSize = 0;
        float farCellSize = 33.333f;
        int32_t farFirstI = 0, farFirstJ = 0;
        const uint16_t* farLayer = nullptr;
        const uint16_t* farTint = nullptr;   // null: `colour` everywhere
        uint32_t farVersion = 0;

        // The wake map (sxc_fog_wake); null = none.
        uint32_t wakeSize = 0;
        float wakeTexel = 1.0f;
        float wakeOrigin[2] = {};
        const uint16_t* wake = nullptr;
        uint32_t wakeVersion = 0;

        float densityScale = 1.0f;
        float noiseAmount = 0.6f;
        float noiseTile = 64.0f;
        float drift[2] = {};      // how far the patches have drifted (yd, summed over the frames)
        float nearDistance = 0.5f;
        float farDistance = 250.0f;
        float hazeDensity = 0.0f;
        float hazeBase = 0.0f;
        float hazeThickness = 30.0f;
        int32_t debug = 0;
        float stepLength = 0.5f; // yd: the steps near the camera
        bool perPixel = true;    // march each pixel's ray; false: the froxel volume
        int32_t maxSteps = 160;  // per pixel: the most steps a ray takes
        float handover = 16.0f;  // yd: the near field fades into the far map over this band
        float lightStrength = 0.0f; // our lights in the fog: x their light; 0 off
        float lightGlow = 0.3f;     // forward scattering (Henyey-Greenstein g)
        float lightRange = 96.0f;   // yd
    };

    // Draws the fog over the current target; false when nothing was drawn.
    using Renderer = bool (*)(const Input& input);
    // A renderer's one-line status ("drawn ...", "off: ...").
    using StatusFn = const char* (*)();

    // The graphics device's renderer (null when it goes away).
    void SetRenderer(Renderer renderer, StatusFn status);

    // The seam: CGWorldFrame::OnWorldRender, after the opaque world and the water, before the
    // weather and the see-through models (camera above water only).
    void Composite();

    // The procedural water's depth writes (MaterialProcWater): on while the fog draws a field, so the
    // fog lies on the water's surface instead of reaching down to the lake bed (WXL did the same).
    bool WaterWritesDepth();
}
