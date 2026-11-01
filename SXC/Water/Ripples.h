// The ripple field's stamps (kb: water-contact): what touches our water this frame, pressed into the field the
// Vulkan device steps (Vulkan/Ripples.hpp) and our water's pixels read. Units: the client's own ripple events
// (CMap::CreateWaterRipple: wading and swimming units, more often the faster they move), which then make no
// stock decal ring. Rain: drops around the camera by the weather's rain.
#pragma once

#include <cstdint>

class C3Vector;
class CGUnit_C;

namespace SXC::Water {
    struct Waves;

    // CMap::CreateWaterRipple: a unit's ripple at a point on the surface (its size and strength as the client
    // chose them; `moving` the client's directional ripple, else a unit standing in the water). True: ours took
    // it (the stock ring isn't made).
    bool RippleFromUnit(const C3Vector& position, float size, float strength, bool moving);

    // CGUnit_C::CreateRipple, every frame a unit is in the water (not deeper than twice its height), with the
    // surface's height there: its body parts (SXC/Bones) ripple the water where they move through the
    // surface. True: ours took the unit (the client makes no ripple events for it then); false when ripples
    // by body part are off or its body isn't known yet.
    bool UnitInWater(CGUnit_C* unit, float surface);

    // FillWaves: the settings; with `frame` (SXC::Water::Frame, once a frame: the device steps the field with
    // that call's input) also this frame's stamps (the units', the rain's) and the time.
    void FillRipples(Waves& waves, const C3Vector& camera, bool frame);

    // The Water panel's Ripples section.
    void DrawRipplePanel();
}
