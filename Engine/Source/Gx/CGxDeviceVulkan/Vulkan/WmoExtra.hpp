// SXC: our WMO shader's vertex data past 3.3.5's formats (kb: our-shaders): a group's third and fourth UV sets
// and Dragonflight's MOC2, which the reader keeps (Retail::PrepareWmoGroup) and SXC::Wmo holds per group. One
// persistent storage buffer, two vec4 a vertex ((u, v of the third set; MOC2 as uint bits; 1), (u, v of the
// fourth set; 0, 0)),
// each group written once into a fresh region (never over one a frame in flight may read); when it's full it
// starts over and the groups upload again at their next draw. Set 2 of our WMO pipelines (the vertex stage), one
// per frame slot: with the terrain's table (Vulkan/Ground) at bindings 1 and 2 for the pixel stage where the device
// can take it.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::wmoextra
{
    bool Init();
    bool Ready();
    void Shutdown();

    /// The set's layout (null before Init()).
    VkDescriptorSetLayout SetLayout();

    /// Whether the set carries the terrain's table (Vulkan/Ground).
    bool Ground();

    /// This frame slot's set: the whole buffer, and the terrain's table filled at the frame's first call (null
    /// before Init()).
    VkDescriptorSet FrameSet(uint32_t slot, uint64_t serial);

    /// Where a group's data starts in the buffer (in vec4s), uploaded first when it's new or changed. False
    /// when the group has none (or there's no room even in an empty buffer).
    bool Offset(const void* group, uint32_t& vec4Index);
}
