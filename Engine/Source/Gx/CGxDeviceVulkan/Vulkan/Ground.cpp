// SXC: the terrain under the camera as a table, the GPU side (Vulkan/Ground.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Ground.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Cover.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/DeviceCaps.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines/Internal.hpp"   // the white image, the sampler cache

#include <cstdio>
#include <cstring>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;
    namespace detail  = gxvk::vk::pipelines::detail;
    using namespace gxvk::vk::ground;

    using gxvk::log::Write;
    using gxvk::log::Info;
    using gxvk::log::Warn;
    using gxvk::log::Error;

    constexpr uint32_t     kSlotsInFlight = frame::kFramesInFlight;
    constexpr VkDeviceSize kHeaderBytes   = 144;  ///< viewInv (3 vec4), the window (ivec4), (chunk size, the map's half, 0, 0), the light (4 vec4)
    // The table: the header, the grid, the holes, the vertex colours, heights and normals, the entries (Ground.glsl).
    constexpr VkDeviceSize kGridBytes     = kGrid * kGrid * 4;
    constexpr VkDeviceSize kColourBytes   = static_cast<VkDeviceSize>(kGrid) * kGrid * kColours * 4;
    constexpr VkDeviceSize kHolesAt       = kHeaderBytes + kGridBytes;
    constexpr VkDeviceSize kColoursAt     = kHolesAt + kGridBytes;
    constexpr VkDeviceSize kHeightsAt     = kColoursAt + kColourBytes;
    constexpr VkDeviceSize kNormalsAt     = kHeightsAt + kColourBytes;
    constexpr VkDeviceSize kEntriesAt     = kNormalsAt + kColourBytes;
    constexpr VkDeviceSize kTableBytes    = kEntriesAt + static_cast<VkDeviceSize>(kMaxEntries) * kEntryBytes;
    static_assert(kEntriesAt % 16 == 0, "the entries must start on 16 bytes (std430)");
    constexpr float        kChunk         = 533.33333f / 16.0f;
    constexpr float        kHalf          = 32.0f * 533.33333f;

    struct Slot
    {
        memory::Buffer table;
        uint32_t       writtenId[kSlots] = {};      ///< the image each element holds (0: white, ~0: never written)
        uint32_t       writtenSampler[kSlots] = {};
        bool           written = false;
    };

    struct State
    {
        bool                  ready = false;
        bool                  failed = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi = nullptr;
        Provider              provider = nullptr;
        Slot                  slots[kSlotsInFlight];
        char                  status[160] = "not drawn yet";
    };
    State s;
}

namespace gxvk::vk::ground
{
    void SetProvider(Provider provider) { s.provider = provider; }
    bool Ready() { return s.ready; }
    const char* Status() { return s.status; }

    bool Possible()
    {
        const context::Handles* h = context::Get();
        if (!h || !h->iapi || !(context::Caps().enabled & gxvk::vk::kCapSampledArrayNonUniform)) return false;
        VkPhysicalDeviceProperties props = {};
        h->iapi->GetPhysicalDeviceProperties(h->physical, &props);
        const VkPhysicalDeviceLimits& lim = props.limits;
        // The ground's wetness on 16 reads the covers' maps from their own set, bound as the WMO's set 4 (Wmo.frag's
        // GROUND build declares it): no covers, no table. Made here if not yet (Init is idempotent): the WMO pixel module
        // is chosen before Pipelines makes the covers' set.
        // Wmo.frag declares the covers' textures as their indexed array (COVER_BINDLESS): not with their 16 slots.
        if (props.limits.maxBoundDescriptorSets < 5 || !gxvk::vk::cover::Init() || !gxvk::vk::cover::Bindless()) return false;
        // The WMO pixel stage's own samplers (16 stages, the lights' few) beside the array, and the covers' set
        // (its maps, textures, marks and shore; declared in the layout though Wmo.frag reads only the maps).
        const uint32_t samplers = kSlots + 32 + gxvk::vk::cover::TextureSlots() + 16;
        return lim.maxPerStageDescriptorSamplers >= samplers && lim.maxPerStageDescriptorSampledImages >= samplers &&
               lim.maxPerStageResources >= samplers + 8 && lim.maxDescriptorSetSamplers >= samplers && lim.maxDescriptorSetSampledImages >= samplers;
    }

    bool Init()
    {
        if (s.ready) return true;
        if (s.failed || !Possible()) return false;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi) return false;
        s.device = h->device;
        s.dapi   = h->dapi;

        for (Slot& slot : s.slots)
        {
            if (!memory::CreateBuffer(kTableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory::Where::Stream, slot.table) || !slot.table.block.mapped)
            {
                Write(Error, "vulkan: ground: no buffer for the table");
                s.failed = true;
                return false;
            }
            std::memset(slot.table.block.mapped, 0, kTableBytes);
            memory::FlushHost(slot.table.block);
        }

        s.ready = true;
        Write(Info, "vulkan: ground: ready (%u texture slots indexed per pixel, a %u-chunk window, %u KiB a frame)", kSlots, kGrid,
              static_cast<uint32_t>(kTableBytes / 1024));
        return true;
    }

    bool Fill(uint32_t slotIndex, VkDescriptorSet set)
    {
        if (!s.ready || slotIndex >= kSlotsInFlight || set == VK_NULL_HANDLE) return false;
        Slot& slot = s.slots[slotIndex];

        static Frame f;   // ~40 KB: off the stack
        f = Frame();
        const bool on = s.provider && s.provider(f) && f.grid && (f.entryCount == 0 || f.entries);
        const uint32_t entries = on ? (f.entryCount < kMaxEntries ? f.entryCount : kMaxEntries) : 0u;

        // The table: the header, the grid, the entries (none: every cell empty, the shader keeps the WMO's own).
        uint8_t* out = static_cast<uint8_t*>(slot.table.block.mapped);
        float header[36] = {};
        std::memcpy(header, f.viewInv, sizeof f.viewInv);
        const int32_t window[4] = { f.firstI, f.firstJ, static_cast<int32_t>(kGrid), static_cast<int32_t>(entries) };
        std::memcpy(header + 12, window, sizeof window);
        header[16] = kChunk;
        header[17] = kHalf;
        std::memcpy(header + 20, f.light, sizeof f.light);
        std::memcpy(out, header, sizeof header);
        if (on)
            std::memcpy(out + kHeaderBytes, f.grid, kGridBytes);
        else
            std::memset(out + kHeaderBytes, 0, kGridBytes);
        if (on && f.holes) std::memcpy(out + kHolesAt, f.holes, kGridBytes);
        if (on && f.colours) std::memcpy(out + kColoursAt, f.colours, kColourBytes);
        if (on && f.heights) std::memcpy(out + kHeightsAt, f.heights, kColourBytes);
        if (on && f.normals) std::memcpy(out + kNormalsAt, f.normals, kColourBytes);
        if (entries) std::memcpy(out + kEntriesAt, f.entries, static_cast<size_t>(entries) * kEntryBytes);
        memory::FlushHost(slot.table.block);

        // The textures: only the elements whose image or sampler changed since this slot's last write; empty ones
        // hold the white image (an array indexed per pixel counts as used whole).
        const gxvk::vk::texture::Image* white = detail::White(false);
        const VkSampler whiteSampler = detail::Sampler(detail::kWhiteSamplerKey);
        if (!white || whiteSampler == VK_NULL_HANDLE) return false;

        static VkDescriptorImageInfo infos[kSlots];
        static VkWriteDescriptorSet  writes[kSlots + 1];
        uint32_t count = 0, changed = 0, missing = 0;

        for (uint32_t k = 0; k < kSlots; ++k)
        {
            const bool have = on && k < f.textureCount && f.textures[k].view != VK_NULL_HANDLE && !f.textures[k].cube;
            const VkSampler sampler = have ? detail::Sampler(f.textures[k].samplerKey) : whiteSampler;
            const bool usable = have && sampler != VK_NULL_HANDLE;
            if (on && k < f.textureCount && !usable) ++missing;
            const uint32_t id = usable ? f.textures[k].imageId : 0u;
            const uint32_t key = usable ? f.textures[k].samplerKey : detail::kWhiteSamplerKey;
            if (slot.written && slot.writtenId[k] == id && slot.writtenSampler[k] == key) continue;
            slot.writtenId[k] = id;
            slot.writtenSampler[k] = key;
            infos[k] = { usable ? sampler : whiteSampler, usable ? f.textures[k].view : white->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet& w = writes[count++];
            w = {};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = set;
            w.dstBinding      = kTexturesBinding;
            w.dstArrayElement = k;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo      = &infos[k];
            ++changed;
        }

        // The table's binding once per set.
        VkDescriptorBufferInfo table = { slot.table.handle, 0, kTableBytes };
        if (!slot.written)
        {
            VkWriteDescriptorSet& w = writes[count++];
            w = {};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = set;
            w.dstBinding      = kTableBinding;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo     = &table;
        }
        if (count) s.dapi->UpdateDescriptorSets(s.device, count, writes, 0, nullptr);
        slot.written = true;

        if (on)
        {
            std::snprintf(s.status, sizeof s.status, "%u chunks, %u textures (%u not on the GPU yet), %u slots rewritten", entries, f.textureCount, missing, changed);
            // The first table with chunks, and how its textures resolved (once).
            static bool s_logged = false;
            if (entries && !s_logged)
            {
                s_logged = true;
                Write(Info, "vulkan: ground: first table: %s", s.status);
            }
        }
        else
            std::snprintf(s.status, sizeof s.status, "no table (no world)");
        return true;
    }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        for (Slot& slot : s.slots) memory::DestroyBuffer(slot.table);
        const Provider provider = s.provider;
        s = State();
        s.provider = provider;
    }
}
