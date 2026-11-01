// SXC: the ripple field (kb: water-contact). A height field of the water's small ripples around the camera
// (1024 x 1024 cells of 0.125 yd, world aligned, toroidal), stepped on the GPU by the wave equation at a fixed
// 60 Hz in the frame's pre-pass, stamped by what touches the water (units wading and swimming, raindrops);
// our water's pixels read its slopes (Water.frag, set 2 binding 7). Not in the original.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::ripples
{
    constexpr uint32_t kSize   = 1024;    ///< cells per side (128 yd)
    constexpr float    kCell   = 0.125f;  ///< yd
    constexpr uint32_t kStamps = 256;     ///< per frame (RippleStep.comp's kStamps)

    struct Input
    {
        bool     on        = false;
        float    camera[2] = {};     ///< world x, y: the window follows it
        float    elapsed   = 0.0f;   ///< s since the last frame
        float    speed     = 1.5f;   ///< yd / s the ripples spread at
        float    life      = 2.0f;   ///< s until a ripple has faded to a tenth
        uint32_t stamps    = 0;
        float    stamp[kStamps][4] = {};   ///< world x, y, radius (yd), depth (yd)
    };

    /// Steps the field into `cmd` (outside a pass, before the water draws): the window moved with the
    /// camera (new cells cleared), the frame's stamps pressed in, as many 60 Hz steps as the time asks
    /// (at most 4). False when off or the field could not be made.
    bool Step(VkCommandBuffer cmd, const Input& in);

    /// The field for the water's set 2 (a 2D array, GENERAL; sample with Sampler(): linear, repeating, so
    /// the window wraps): null until the field is made (Step's first call).
    VkImageView View();
    VkSampler   Sampler();

    /// As the draws read it this frame: `grid` the window's first cell (x, y), cells per side, the cell (yd);
    /// `look` the current layer, 1 when stepped this frame, 0, 0.
    void Look(float grid[4], float look[4]);

    /// One line for the Water panel.
    const char* Status();

    /// Everything, with the device idle.
    void Shutdown();
}
