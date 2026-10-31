// Swimmers riding the waves (Seyri, 2026-10-31; docs/kb water-shader): units swimming at the surface are drawn
// moved with the wave under them, rising, sinking and tilting with it, and the camera rides along with its
// target. Only how they're drawn: their real position stays on the still surface the server knows, so nobody
// swims in the air in a trough and nothing changes for the server.
//
// Where the surface is comes from the GPU (Shaders/WaterQuery.comp runs Water.vert's own movement at each
// swimmer), asked when the frame's water is set up and answered a few frames later.
#pragma once

#include <cstddef>
#include <cstdint>

class C3Vector;
class C44Matrix;
class CGObject_C;
class CGUnit_C;

namespace SXC::Water {
    struct Waves;

    // The swimmers near the camera to ask about (into `waves`, whose queryCamera and queryLook are set), at
    // the frame's water set-up; the device asks once a frame.
    void RideAsk(Waves& waves);

    // The unit's model moved and tilted with the wave under it (CGUnit_C::Animate's two paths).
    void RideModel(CGUnit_C* unit, C44Matrix& transform);

    // How far the camera's target rises with the wave under it (CGCamera::GetSafeWorldPos).
    float RideLift(CGObject_C* target);

    // A line for the Water panel: how many swimmers were asked and answered, and the player's state.
    void RideStatus(char* text, size_t size);
}
