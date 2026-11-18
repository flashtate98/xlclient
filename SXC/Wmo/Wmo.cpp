#include "SXC/Wmo/Wmo.h"

#include "SXC/Settings/Settings.h"

namespace {
    bool s_drawing = false;
    uint32_t s_shader = 0;

    SXC::Settings::Bool& Setting() {
        static SXC::Settings::Bool s_setting("Wmo", "OurShader", false, "Our WMO shader (Vulkan)",
            "Draw WMOs with our own shader instead of the translated stock and Cataclysm ones (kb: our-shaders). "
            "It aims at the same look; off by default until it matches. Shader 8 and the liquid windows stay translated.");
        return s_setting;
    }
}

namespace SXC::Wmo {
    bool OurShader() {
        return Setting().Get();
    }

    void BeginDraw(uint32_t shader) {
        s_drawing = true;
        s_shader = shader;
    }

    void EndDraw() {
        s_drawing = false;
    }

    bool Drawing(uint32_t& shader) {
        shader = s_shader;
        return s_drawing;
    }

    bool Covers(uint32_t shader) {
        // The liquid windows (10, 14) aren't ported yet. Past Cataclysm's: 15 and 19 (two UV sets, two
        // textures, as their fallback 6 loads them); 18 and 20 want a third UV set the reader drops, 23 the
        // extra textures, 22 a tangent-space view, 16 the terrain under it; 21 is the LOD groups'.
        return (shader <= 13 && shader != 10) || shader == 15 || shader == 19;
    }
}
