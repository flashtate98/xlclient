#pragma once

#include <cstdint>

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
    void BeginDraw(uint32_t shader, uint32_t materialBlend);
    void EndDraw();

    // The batch being drawn now, if any: its shader id and the material's blend mode.
    bool Drawing(uint32_t& shader, uint32_t& materialBlend);

    // Whether our program draws this shader id (the ids not covered yet stay with the translated records).
    bool Covers(uint32_t shader);
}
