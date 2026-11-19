#pragma once

#include <cstdint>
#include <vector>

// Our WMO shader (kb: our-shaders): the seam between the map object code and the Vulkan device. CMapObj::
// UnifiedRender announces the batch it draws (its material's shader id); the device draws it with our own
// WMO program pair (Shaders/Wmo.vert, Wmo.frag) instead of the translated records when the setting is on
// and the program covers that id. Main thread.
namespace SXC::Wmo {
    // The setting Wmo/OurShader (off until our shader matches the look of the translated ones).
    bool OurShader();

    // Around one draw of a unified WMO batch: the shader id the material is drawn with (after
    // CMapObj::CreateMaterial's fallbacks) and the material's own blend mode (a transition batch is drawn in
    // two passes with fixed blend modes 9 and 7, whatever the material's: an alpha-key one is cut all the same).
    void BeginDraw(uint32_t shader, uint32_t materialBlend, const void* group);
    void EndDraw();

    // The batch being drawn now, if any: its shader id, the material's blend mode, its group.
    bool Drawing(uint32_t& shader, uint32_t& materialBlend, const void*& group);

    // A group's vertex data past 3.3.5's formats (Retail::PrepareWmoGroup's SXT3 and SXC2; CMapObjGroup::
    // Create hands them over, CMap::FreeMapObjGroup releases them): the third UV set (2 floats a vertex)
    // and Dragonflight's MOC2 (BGRA a vertex), copied. `serial` changes with every set, so the device knows
    // to upload again.
    struct GroupExtra {
        std::vector<float> uv3;
        std::vector<uint32_t> weights;
        uint32_t vertices = 0;
        uint64_t serial = 0;
    };
    void SetGroupExtra(const void* group, uint32_t vertices, const float* uv3, uint32_t uv3Count, const uint32_t* weights, uint32_t weightCount);
    void ReleaseGroup(const void* group);
    const GroupExtra* Extra(const void* group);

    // Whether our program draws this shader id (the ids not covered yet stay with the translated records).
    bool Covers(uint32_t shader);
}
