// SXC: our WMO shader's vertex data past 3.3.5's formats (kb: our-shaders): a group's third UV set and
// Dragonflight's MOC2, which the reader keeps (Retail::PrepareWmoGroup) and SXC::Wmo holds per group. One
// persistent storage buffer, a vec4 a vertex (u, v of the third set; MOC2 as uint bits; 1 where present),
// each group written once into a fresh region (never over one a frame in flight may read); when it's full it
// starts over and the groups upload again at their next draw. Set 2 of our WMO pipelines (the vertex stage).
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

    /// The set over the whole buffer (null before Init()).
    VkDescriptorSet Set();

    /// Where a group's data starts in the buffer (in vec4s), uploaded first when it's new or changed. False
    /// when the group has none (or there's no room even in an empty buffer).
    bool Offset(const void* group, uint32_t& vec4Index);
}
