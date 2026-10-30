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
        uint32_t shoreVersion = 0;
        int32_t shoreOriginX = 0, shoreOriginY = 0;
        float shoreFoam = 1.0f;          // the shore's foam strength
        float shoreWaveHeight = 0.12f;   // yd
        float shoreWavelength = 7.0f;    // yd
        float shoreReach = 28.0f;        // yd: how far out the shore waves begin
    };

    // The device's copy of the scene behind the water and its waves, once per frame (later calls the same
    // frame answer as the first); false: the stock water draws this frame.
    using CaptureFn = bool (*)(const Waves& waves);
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

    // The overlay's Water panel: the switches, the look's live values, the debug views.
    void DrawPanel(void* user);
}
