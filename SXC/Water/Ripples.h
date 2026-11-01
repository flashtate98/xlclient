// The ripple field's stamps (kb: water-contact): what touches our water this frame, pressed into the field the
// Vulkan device steps (Vulkan/Ripples.hpp) and our water's pixels read. Units: the client's own ripple events
// (CMap::CreateWaterRipple: wading and swimming units, more often the faster they move), which then make no
// stock decal ring. Rain: drops around the camera by the weather's rain.
#pragma once

#include <cstdint>

class C3Vector;

namespace SXC::Water {
    struct Waves;

    // CMap::CreateWaterRipple: a unit's ripple at a point on the surface (its size and strength as the client
    // chose them; `moving` the client's directional ripple, else a unit standing in the water). True: ours took
    // it (the stock ring isn't made).
    bool RippleFromUnit(const C3Vector& position, float size, float strength, bool moving);

    // FillWaves: the settings; with `frame` (SXC::Water::Frame, once a frame: the device steps the field with
    // that call's input) also this frame's stamps (the units', the rain's) and the time.
    void FillRipples(Waves& waves, const C3Vector& camera, bool frame);

    // The Water panel's Ripples section.
    void DrawRipplePanel();
}
