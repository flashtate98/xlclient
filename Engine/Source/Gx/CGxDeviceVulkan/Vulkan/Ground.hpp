// SXC: the terrain under the camera as a table, the GPU side (SXC/Ground; kb: our-shaders, 16's terrain blend).
// Per frame slot a storage buffer (the view back to the world, the window of map chunks, the chunk entries) and
// an array of the textures the entries name, indexed per pixel; both are bindings 1 and 2 of our WMO pipelines'
// set 2 (Vulkan/WmoExtra), where the device can index a sampler array that large per pixel (Possible()); else
// WMO shader 16 draws without the terrain.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::ground
{
    constexpr uint32_t kSlots       = 2048;  ///< textures (SXC::Ground::kMaxTextures)
    constexpr uint32_t kGrid        = 32;    ///< map chunks per side of the window (SXC::Ground::kGrid)
    constexpr uint32_t kMaxEntries  = 1024;  ///< chunk entries (SXC::Ground::kMaxEntries)
    constexpr uint32_t kEntryBytes  = 112;   ///< one entry (SXC::Ground::Entry; Shaders/Ground.glsl)
    constexpr uint32_t kColours     = 145;   ///< a map chunk's vertex colours (SXC::Ground::kColours)
    constexpr uint32_t kTableBinding    = 1; ///< set 2 of our WMO pipelines
    constexpr uint32_t kTexturesBinding = 2;

    struct Frame
    {
        float           viewInv[12] = {};      ///< the view back to the world, a row per world axis
        float           light[16] = {};        ///< the terrain's light: toward the sun (world), ambient, diffuse (xyz, w 0), highlight (colour, exponent)
        int32_t         firstI = 0, firstJ = 0; ///< the window's first map chunk
        const uint32_t* grid = nullptr;        ///< kGrid x kGrid: entry + 1
        const uint32_t* holes = nullptr;       ///< kGrid x kGrid: each cell's holes (4 x 4 bits)
        const uint32_t* colours = nullptr;     ///< kGrid x kGrid x kColours: each cell's vertex colours (D3DCOLOR)
        const float*    heights = nullptr;     ///< kGrid x kGrid x kColours: each cell's vertex heights (world z)
        const uint32_t* normals = nullptr;     ///< kGrid x kGrid x kColours: each cell's vertex normals (signed bytes x, y, z)
        const void*     entries = nullptr;     ///< kEntryBytes each
        uint32_t        entryCount = 0;
        pipelines::Texture textures[kSlots];   ///< slot k (entries name k + 1); view null: white
        uint32_t        textureCount = 0;
    };

    /// Fills this frame's table; false when there is none (no world).
    using Provider = bool (*)(Frame& out);
    void SetProvider(Provider provider);

    /// Whether the device can take the table: per-pixel indexing of a sampler array (Vulkan 1.2), the limits
    /// for kSlots more samplers in the WMO pixel stage, and the covers' set as a fifth set (the ground's wetness;
    /// makes it if not yet). The WMO pixel module is chosen by it before the WMO sets exist.
    bool Possible();

    bool Init();
    bool Ready();
    void Shutdown();

    /// Writes this frame's table into frame slot `slot` and its textures into `set` (bindings 1 and 2; only the
    /// elements that changed since this slot's last write). False when it can't (the set then holds the last).
    bool Fill(uint32_t slot, VkDescriptorSet set);

    /// One line: what the table held last frame.
    const char* Status();
}
