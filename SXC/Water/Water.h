// SXC: our water (kb: features/render/water-shader). The stock water materials (LiquidMaterial 1, the
// old water, and 3, the unfinished "Water 2.0") are drawn by our own shaders while [Liquid] OurShader is
// on and the Vulkan device draws them (Shaders/Water.vert, Water.frag); magma and slime, the D3D9 device
// and the setting off keep the stock materials, untouched.
#pragma once

#include <cstdint>

class C3Vector;

namespace Liquid {
    class CInstance;
}

namespace SXC::Water {
    // The waves this frame (the device's FFT sea, step 3): the wind at the player and the look.
    struct Waves {
        bool on = false;
        float time = 0.0f;       // seconds, wrapping every `period`
        float period = 600.0f;
        float windX = 1.0f;      // the wind's direction (unit)
        float windY = 0.0f;
        float speed = 6.0f;      // yd / s
        float spread = 0.3f;     // 0 all with the wind .. 1 every way
        float amplitude = 0.35f; // x the spectrum's heights
        float chop = 1.2f;       // horizontal displacement (sharper crests)
        float foam = 1.0f;       // the foam's strength where crests fold
        bool rebuild = false;    // make the spectrum again even if the wind hasn't changed
        float move = 1.0f;       // x the surface's displacement near the camera (0 flat)
        // The sky's bodies this frame (SXC::Sky::BodiesSeen), reflected on the water: per body the direction
        // toward it, its angular radius (rad), its colour.
        uint32_t bodies = 0;
        float body[4][7] = {};
        // The look grid (SXC/Water/Surface.h's LookGrid): 64 x 64 chunk indices into up to 64 packed looks of
        // 20 floats; null when there is none (the draws then take their own look).
        const uint8_t* lookIndex = nullptr;
        const float* lookPalette = nullptr;
        uint32_t lookCount = 0;
        int32_t lookOriginX = 0, lookOriginY = 0;
        // The shore field (SXC/Water/Shore.h): 256 x 256 cells of four half floats, stored toroidally; null
        // when there is none. Uploaded when the version changes.
        const uint16_t* shore = nullptr;
        const float* shoreCells = nullptr;   // the near mesh's cells (Shore.h's ShoreMap::cells)
        uint32_t shoreVersion = 0;
        int32_t shoreOriginX = 0, shoreOriginY = 0;
        // The rivers' flow on the same window (SXC/Water/Flow.h): four half floats a cell; null: none.
        const uint16_t* flow = nullptr;
        uint32_t flowVersion = 0;
        float shoreFoam = 1.0f;          // the shore's foam strength
        float shoreWaveHeight = 0.12f;   // yd
        float shoreWavelength = 7.0f;    // yd
        float shoreReach = 28.0f;        // yd: how far out the shore waves begin
        float shoreSwash = 0.5f;         // yd: how high the swash runs up the beach (the terrain draws it)
        float caustics = 1.0f;           // the caustics on the floor (the terrain draws them): strength,
        float causticScale = 2.5f;       // yd per cell,
        float causticDepth = 6.0f;       // yd they reach
        float sun[3] = { 0.0f, 0.0f, 1.0f };   // toward the sun (world, unit): the light's direction reversed
        float clouds = 0.0f;             // how cloudy (DayNight's storm blend, 0..1)
        float sunColour[3] = { 1.0f, 1.0f, 1.0f };   // the sun's direct light (the day's diffuse), for the models' caustics
        float underGlow[4] = {};         // the underwater medium's glow (rgb), w 1 while it draws (SXC/Water/Medium.h)
        int causticDebug = 0;            // the terrain's caustics debug view (the Water panel)
        // The swimmers riding the waves (SXC/Water/Ride.h): `queryAsk` fills the points where the surface is
        // asked (called at the frame's first capture only), answered a few frames later through `querySink`
        // with the same tag.
        uint32_t (*queryAsk)(float (*points)[4], uint32_t max, uint64_t* tag) = nullptr;
        bool prepareOnly = false;        // the frame's water work without the scene copy (SXC::Water::Frame)
        float queryCamera[4] = {};                       // the camera's world position, the water's clock
        float queryLook[4] = { 0.0f, 1.0f, 0.0f, 1.0f };  // the movement row without a look grid
        void (*querySink)(uint64_t tag, const float (*results)[4], uint32_t count) = nullptr;
        // The ripple field (SXC/Water/Ripples.h -> Vulkan/Ripples): this frame's stamps (world x, y, radius, depth),
        // the time since the last frame, how fast ripples spread (yd / s) and fade (s to a tenth), x their slopes.
        bool ripplesOn = false;
        float rippleElapsed = 0.0f;
        float rippleSpeed = 1.5f;
        float rippleLife = 2.0f;
        float rippleStrength = 1.0f;
        float rippleDistance = 45.0f;   // yd from the camera they show
        uint32_t rippleStamps = 0;
        const float (*rippleStamp)[4] = nullptr;
    };

    // The device's copy of the scene behind the water and its waves, once per frame (later calls the same
    // frame answer as the first); false: the stock water draws this frame.
    // `first`: this is the frame's first call (the near water mesh draws then).
    using CaptureFn = bool (*)(const Waves& waves, bool* first);
    using StatusFn = const char* (*)();

    // The Vulkan device: available once its water programs and the lights' set are up, false when it goes.
    void SetDevice(bool available, CaptureFn capture, StatusFn status);

    // The device's last word on the scene's copy, for the overlay (empty without a device).
    const char* Status();

    // The setting and the device.
    bool Active();

    // Liquid::CRenderer::Flush, for each instance: draws it as ours when it is a stock water material
    // and we're active. False: the instance's own material draws it.
    bool Draw(Liquid::CInstance* instance, const C3Vector& cameraPos);

    // Liquid::CRenderer::Flush's start (the opaque pass), every frame: the water's work that needs no water
    // drawn (the waves, the shore for the terrain, the swimmers), so it goes on with none in view (under
    // water, looking down at the floor).
    void Frame(const C3Vector& cameraPos);

    // The overlay's Water panel: the switches, the look's live values, the debug views.
    void DrawPanel(void* user);
}
