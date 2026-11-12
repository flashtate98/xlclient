// SXC: the underwater medium (kb: underwater). With the camera in water, over the opaque world and the water's
// surface (SXC::Water's seam in the world render's underwater branch): Medium.comp reads the target's colour
// and depth into our image -- the colour losing its channels by the view's distance through the water, going
// grey, fading into the water's glow -- and MediumCopy.frag puts it back on the target. Not in the original.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::medium
{
    struct Frame
    {
        float      viewProj[16] = {};  ///< view x the native projection (row vectors), relative to the camera
        VkViewport viewport     = {};  ///< the world's, in pixels of the target, with its depth range
        float      loss[3]      = {};  ///< per yd: log2 of each channel's loss (1 / the yd until halved; 0 none)
        float      glow[3]      = {};  ///< the water's glow, what far things fade into
        float      desaturate   = 0.0f; ///< yd until grey; 0 never
        float      fade         = 0.0f; ///< per yd: things fade into the glow as 1 - exp(-fade x distance)
        int        debug        = 0;    ///< 1 the fade, 2 distance bands
        float      origin[3]    = {};   ///< the camera's world position
        float      halo         = 0.0f; ///< our lamps lighting the water: per yd scattered (0 off)
        float      haloRange    = 96.0f; ///< yd: how far out the lamps' light in the water is gathered
        float      haloGlow     = 0.5f;  ///< forward scattering (Henyey-Greenstein g): brighter looking toward a lamp
        float      line[4]      = { 1000.0f, 0.0f, 0.0f, 1.0f }; ///< the waterline: the surface relative to the camera (yd), its slopes, 1 on
        float      lens[2]      = {};    ///< seconds, how wet the lens is (1 just surfaced)
        float      still        = 0.0f;  ///< the still surface at the camera (world z), for the surface's movement
    };

    /// Records the medium over the current target. False when nothing was drawn; Status() says why.
    bool Record(const Frame& frame);

    /// This frame's medium image, for the fog drawn after it: its alpha says how far each pixel's view starts
    /// under the water (1 under, 0 in the air). False when the medium didn't draw this frame.
    bool UnderMask(uint64_t serial, VkImageView& view);

    /// One line: what the medium did last, or why it didn't.
    const char* Status();

    /// Everything, with the device idle (before frame::Destroy()).
    void Shutdown();
}
