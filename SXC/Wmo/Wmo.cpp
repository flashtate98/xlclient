#include "SXC/Wmo/Wmo.h"

#include <unordered_map>
#include "SXC/Settings/Settings.h"

namespace {
    bool s_drawing = false;
    uint32_t s_shader = 0;
    uint32_t s_materialBlend = 0;
    const void* s_group = nullptr;
    std::unordered_map<const void*, SXC::Wmo::GroupExtra> s_extra;   // main thread
    uint64_t s_serial = 0;

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

    void BeginDraw(uint32_t shader, uint32_t materialBlend, const void* group) {
        s_drawing = true;
        s_shader = shader;
        s_materialBlend = materialBlend;
        s_group = group;
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
        // tangent-space view, 16 the terrain under it; 21 is the LOD groups'.
        return (shader <= 13 && shader != 10) || (shader >= 15 && shader <= 20 && shader != 16 && shader != 17);
    }

    void SetGroupExtra(const void* group, uint32_t vertices, const float* uv3, uint32_t uv3Count, const uint32_t* weights, uint32_t weightCount) {
        if (!uv3Count && !weightCount) {
            s_extra.erase(group);
            return;
        }

        GroupExtra& e = s_extra[group];
        e.vertices = vertices;
        e.uv3.assign(uv3, uv3 + 2 * static_cast<size_t>(uv3Count));
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
