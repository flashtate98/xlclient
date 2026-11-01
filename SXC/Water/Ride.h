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

    // The terrain water's surface at the camera as it stands (its still height plus the waves there, a few
    // frames late, smoothed like a rider's), its slopes, its LiquidType; false when the camera isn't within a
    // few yards of terrain water (the waterline: SXC/Water/Medium.h).
    // `still`: the still surface there; `reach`: how far the surface has lately risen or fallen from it there
    // (the largest over the last seconds, at least a little).
    bool CameraWater(float& height, float& slopeX, float& slopeY, uint32_t& liquidType, float* still = nullptr, float* reach = nullptr);

    // The camera passes through the water's surface while our water draws ([Liquid] CameraThroughSurface): not
    // bumped off it, the water no wall between the target and the camera (as cameraWaterCollision 0), no
    // pitch snapped on surfacing or diving. The waterline draws it half in. False: the stock camera.
    bool CameraPassesSurface();
}
