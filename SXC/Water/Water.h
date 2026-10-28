// SXC: our water (kb: features/render/water-shader). The stock water materials (LiquidMaterial 1, the
// old water, and 3, the unfinished "Water 2.0") are drawn by our own shaders while [Liquid] OurShader is
// on and the Vulkan device draws them (Shaders/Water.vert, Water.frag); magma and slime, the D3D9 device
// and the setting off keep the stock materials, untouched.
#pragma once

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
