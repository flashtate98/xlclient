#include "SXC/Wmo/Wmo.h"

#include <unordered_map>
#include "SXC/Settings/Settings.h"

namespace {
    bool s_drawing = false;
    uint32_t s_shader = 0;
    uint32_t s_materialBlend = 0;
    const void* s_group = nullptr;
    uint32_t s_layers = 0;
    std::unordered_map<const void*, SXC::Wmo::GroupExtra> s_extra;   // main thread
    uint64_t s_serial = 0;

    SXC::Settings::Bool& Setting() {
        static SXC::Settings::Bool s_setting("Wmo", "OurShader", false, "Our WMO shader (Vulkan)",
            "Draw WMOs with our own shader instead of the translated stock and Cataclysm ones (kb: our-shaders). "
            "It aims at the same look; off by default until it matches. Shader 8 and the liquid windows stay translated.");
        return s_setting;
    }

    // The goblin city's windows (retail's 22 with six textures; our reconstruction, kb: our-shaders): retail's
    // depths and scales aren't in the material. The defaults are Seyri's tuning in game (2026-11-19), likely a
    // little stronger than retail's.
    struct ParallaxSettings {
        SXC::Settings::Float depth1{"Wmo", "ParallaxDepth1", 0.25f, 0.0f, 1.0f, "Windows: curtains' depth",
            "How far behind the glass the curtains hang, in the window's UV units (goblin windows, shader 22)."};
        SXC::Settings::Float depth2{"Wmo", "ParallaxDepth2", 0.15f, 0.0f, 1.0f, "Windows: room's depth",
            "How far behind the glass the lit room lies, in the window's UV units."};
        SXC::Settings::Float scale1{"Wmo", "ParallaxScale1", 1.0f, 0.05f, 16.0f, "Windows: curtains' UV scale",
            "How often the curtain fabric and its mask repeat per window UV."};
        SXC::Settings::Float scale2{"Wmo", "ParallaxScale2", 1.0f, 0.05f, 16.0f, "Windows: room's UV scale",
            "How often the lit room repeats per window UV (1: it lines up with the panes)."};
        SXC::Settings::Bool invert{"Wmo", "ParallaxMaskInvert", true, "Windows: invert the curtain mask",
            "The curtain mask's alpha read the other way round (Seyri, 2026-11-19: inverted is right, else the chains "
            "are cut into the fabric)."};
        SXC::Settings::Int glassMode{"Wmo", "ParallaxGlassMode", 1, 0, 2, "Windows: how the glass sits",
            "0: the glass's alpha covers what's behind (retail's parallax); 1: its alpha is how much shows through "
            "(the curtains faint); 2: the glass added over all of it."};
        SXC::Settings::Float glow{"Wmo", "ParallaxRoomGlow", 1.0f, 0.0f, 4.0f, "Windows: room glow",
            "How brightly the lit room behind the curtains glows (added light; 0 = lit like a wall)."};
    };

    ParallaxSettings& Parallax() {
        static ParallaxSettings s_parallax;
        return s_parallax;
    }
}

namespace SXC::Wmo {
    bool OurShader() {
        return Setting().Get();
    }

    void BeginDraw(uint32_t shader, uint32_t materialBlend, const void* group, uint32_t layers) {
        s_drawing = true;
        s_shader = shader;
        s_materialBlend = materialBlend;
        s_group = group;
        s_layers = layers;
    }

    uint32_t DrawingLayers() {
        return s_layers;
    }

    void EndDraw() {
        s_drawing = false;
    }

    bool Drawing(uint32_t& shader, uint32_t& materialBlend, const void*& group) {
        shader = s_shader;
        materialBlend = s_materialBlend;
        group = s_group;
        return s_drawing;
    }

    bool Covers(uint32_t shader) {
        // The liquid windows (10, 14) aren't ported yet. Past Cataclysm's: 15 and 19 (two UV sets, two
        // textures, as their fallback 6 loads them), 18 and 20 (and a third texture through the third UV set:
        // CreateMaterial loads it, Vulkan/WmoExtra carries the set); 23 wants its extra textures, 22 a
        // tangent-space view, 16 the terrain under it; 21 is the LOD groups'. 22 and 23 (our reconstructions):
        // their further textures on stages 8-13 (CreateMaterial loads them from SXMT); 22 its tangent space from
        // the pixel's derivatives, 23 the fourth UV set and MOC2 from Vulkan/WmoExtra.
        return (shader <= 13 && shader != 10) || (shader >= 15 && shader <= 20 && shader != 16 && shader != 17) || shader == 22 ||
               shader == 23;
    }

    void ParallaxParams(float out[16]) {
        const ParallaxSettings& p = Parallax();
        const float s1 = p.scale1.Get(), s2 = p.scale2.Get();
        const float values[16] = {
            p.depth1.Get(), p.depth2.Get(), p.invert.Get() ? 1.0f : 0.0f, p.glow.Get(),
            1.0f, 1.0f, static_cast<float>(p.glassMode.Get()), 0.0f,   // the glass as it is; how it sits
            s1, s1, 0.0f, 0.0f,
            s2, s2, 0.0f, 0.0f,
        };
        for (int i = 0; i < 16; i++) out[i] = values[i];
    }

    void SetGroupExtra(const void* group, uint32_t vertices, const float* uv3, uint32_t uv3Count, const float* uv4, uint32_t uv4Count,
                       const uint32_t* weights, uint32_t weightCount) {
        if (!uv3Count && !uv4Count && !weightCount) {
            s_extra.erase(group);
            return;
        }

        GroupExtra& e = s_extra[group];
        e.vertices = vertices;
        e.uv3.assign(uv3, uv3 + 2 * static_cast<size_t>(uv3Count));
        e.uv4.assign(uv4, uv4 + 2 * static_cast<size_t>(uv4Count));
        e.weights.assign(weights, weights + weightCount);
        e.serial = ++s_serial;
    }

    void ReleaseGroup(const void* group) {
        s_extra.erase(group);
    }

    const GroupExtra* Extra(const void* group) {
        const auto found = s_extra.find(group);
        return found == s_extra.end() ? nullptr : &found->second;
    }
}
