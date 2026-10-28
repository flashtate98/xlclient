// SXC: our water's device side (SXC/Water; Shaders/Water.*): the scene behind the water. At the frame's
// first water draw the open pass ends (frame::BeginEffect), Shaders/WaterCapture.comp copies the target's
// colour and depth (as the view's w) into two images, and the pass resumes; the water draws read them
// through set 2 (with the viewport, to find their pixel in them): refraction, absorption by the water the
// view crosses, screen-space reflections.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::water
{
    struct CaptureInput
    {
        VkViewport viewport = {};      ///< the draws' viewport on the target (pixels, depth range)
        float      proj[16] = {};      ///< the native projection, row-major (row vectors)
    };

    /// The copy, once per frame (later calls the same frame answer as the first). False: nothing to copy
    /// over yet, or the device can't; the water draws the stock way this frame.
    bool Capture(const CaptureInput& in);

    /// Set 2 of the water pipelines (made on first need; null when it can't be).
    VkDescriptorSetLayout SetLayout();

    /// This frame's set 2, or null when the scene wasn't copied this frame.
    VkDescriptorSet FrameSet(uint32_t slot, uint64_t serial);

    const char* Status();

    /// After the device is idle.
    void Shutdown();
}
