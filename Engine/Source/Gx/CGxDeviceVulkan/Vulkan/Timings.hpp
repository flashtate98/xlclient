// SXC: GPU timestamps for a frame profiler. Two lanes per frame: the main buffer's (the draws, split
// wherever the caller's category changes) and the pre-pass buffer's (uploads, the shadow maps). Each
// frame slot has its own query pool; a slot's stamps are read when the slot opens again, after its fence:
// nothing waits. Categories are the caller's numbers; the tier knows nothing of what they mean.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::timings
{
    /// The main lane's categories: 0 .. kMaxCategories - 1.
    constexpr int kMaxCategories = 128;

    /// What the pre-pass lane tells apart.
    enum class Pre : int
    {
        Uploads,      ///< everything recorded ahead of the draws but the shadow maps
        ShadowMaps,   ///< the point lights' cube shadow maps (Vulkan/Shadows)
        Count
    };

    /**
     * A finished frame's times: per main-lane category and per pre-pass kind, in ms.
     *
     * @param double* main : kMaxCategories entries
     * @param double* pre : Pre::Count entries
     */
    using Sink = void (*)(const double* main, const double* pre);

    /// The pools, after the context exists (false: the graphics queue has no timestamps). Idempotent.
    bool Init();

    /// Everything, after the device is idle.
    void Shutdown();

    /// Who gets the times (null: nobody; the stamps stop).
    void SetSink(Sink sink);

    /// Stamping on or off; takes effect at the next frame.
    void Enable(bool on);

    /// The main lane's category from here on: a stamp in the open frame closes the one before.
    void Mark(int category);

    /// The pre-pass lane's kind from here on (recorded into the pre-pass buffer).
    void PreMark(Pre kind);

    /// The frame: its two buffers have just begun (after the slot's fence: the slot's last stamps are read).
    void FrameOpened(uint32_t slot, VkCommandBuffer pre, VkCommandBuffer main);

    /// The frame is about to end its buffers (outside any render pass).
    void FrameClosing();

    /// One line for the profiler's panel.
    const char* Status();
}
