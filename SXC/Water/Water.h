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
    // The Vulkan device: true once its water programs and the lights' set are up, false when it goes.
    void SetDevice(bool available);

    // The setting and the device.
    bool Active();

    // Liquid::CRenderer::Flush, for each instance: draws it as ours when it is a stock water material
    // and we're active. False: the instance's own material draws it.
    bool Draw(Liquid::CInstance* instance, const C3Vector& cameraPos);
}
