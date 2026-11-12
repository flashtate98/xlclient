// SXC: a second view of the world (kb: second-camera): while the world's opaque part is drawn, every draw
// that goes to the scene is copied into a target of ours, through another projection; the copies run in a
// pass of their own right after, in frame order, so they read what the frame made before them (shadow maps,
// the covers, uploads) as the draws they copy do. The world isn't culled or walked again: what the main
// camera drew is what the view has (the mirror of the water: Seyri, 2026-11-04).
//
// The other projection works for every kind of draw because each ends in the frame's projection: the
// fixed-function draws push World x View x Proj (taken times Proj^-1 x Mirror), the shader-effect and
// terrain draws carry Proj in their constant registers (found there as it lies, or transposed, and
// replaced). A draw whose projection isn't found isn't copied (left out rather than drawn wrong). The
// mirror flips the winding (the cull turns over); the terrain's copies write nothing into the covers'
// grids. Not in the original.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::pipelines { struct Draw; }

namespace gxvk::vk::view
{
    struct BeginInput
    {
        /// The view (row-major, row vectors, as the device's matrices): view space to the mirrored view's
        /// space, and the water plane there (kept where plane . (x, y, z, 1) >= 0, the camera on the other
        /// side). Each draw's own projection becomes the view's with the plane as its near plane (the oblique
        /// near plane), so draws of another projection (the distant terrain's farther one) mirror too.
        float    mirror[16] = {};
        float    plane[4]   = {};
        /// The sky's draws (between Sky(true) and Sky(false)): its dome and skyboxes are small models around
        /// the camera, pushed back by their depth range: mirrored through the camera, nothing clipped.
        float    sky[16]    = {};
        float    proj[16]   = {};   ///< the frame's projection now (the shader effects' constants carry it)
        uint32_t divisor    = 2;    ///< the target is the scene's size divided by this
        bool     grass      = false;   ///< the instanced grass copied too: its plants (not their density copies)
        float    grassReach = 200.0f;  ///< out to this far from the player (yd)
        float    clear[4]   = {};   ///< the target's colour before the copies
    };

    /// From now on, Copy() takes the scene's draws. False (and nothing copied) when the frame, the target
    /// or its buffer can't be had.
    bool Begin(const BeginInput& in);

    /// Whether Begin() holds.
    bool Recording();

    /// The sky is being drawn (true) or no longer (false): its copies take BeginInput::sky.
    void Sky(bool on);

    /// A draw just recorded into the scene's pass, copied into the view (the scene's, not another target's:
    /// the caller asks).
    void Copy(const pipelines::Draw& draw, const float proj[16]);   ///< `proj`: the device's projection at the draw

    /// Runs the copies (their own pass over the target, before what the frame records next).
    void End();

    /// The target as drawn this frame (`serial`: frame::Serial()), in SHADER_READ_ONLY_OPTIMAL; null when no
    /// view was drawn this frame.
    VkImageView Image(uint64_t serial);

    /// The target's view once it has been drawn (it rests in SHADER_READ_ONLY_OPTIMAL from then on), for a
    /// descriptor written before this frame's copies run; null before. Image() says whether it is this frame's.
    VkImageView Kept();

    struct Stats
    {
        uint32_t copied       = 0;   ///< the last view's draws
        uint32_t left         = 0;   ///< of its kinds left out (interface, projected textures, water)
        uint32_t noProjection = 0;   ///< draws whose projection wasn't found in their constants
        uint32_t width = 0, height = 0;
    };
    Stats GetStats();

    /// The target, after the device is idle.
    void Shutdown();
}
