// SXC: our WMO shader's extra vertex data, the GPU side (Vulkan/WmoExtra.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/WmoExtra.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Ground.hpp"   // SXC: the terrain's table, bindings 1 and 2
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "SXC/Wmo/Wmo.h"

#include <cstring>
#include <unordered_map>

namespace
{
    namespace context = gxvk::vk::context;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;

    using gxvk::log::Write;
    using gxvk::log::Info;
    using gxvk::log::Warn;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    constexpr uint32_t     kVec4s = 2u * 1024u * 1024u;   ///< 32 MiB: about 2 million vertices
    constexpr VkDeviceSize kBytes = static_cast<VkDeviceSize>(kVec4s) * 16;

    struct Entry
    {
        uint64_t serial = 0;
        uint32_t index  = 0;
    };

    constexpr uint32_t kSets = gxvk::vk::frame::kFramesInFlight;

    struct State
    {
        bool                  ready  = false;
        bool                  failed = false;
        bool                  ground = false;   ///< the set carries the terrain's table (Vulkan/Ground)
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool      pool      = VK_NULL_HANDLE;
        VkDescriptorSet       sets[kSets] = {};     ///< one per frame slot (the table changes each frame)
        uint64_t              serials[kSets] = {};
        memory::Buffer        buffer;
        uint32_t              cursor = 0;
        uint32_t              restarts = 0;
        std::unordered_map<const void*, Entry> groups;
    };
    State s;

    bool Check(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        Write(Error, "vulkan: WMO extra data: %s failed: %s", what, ResultName(r));
        s.failed = true;
        return false;
    }
}

namespace gxvk::vk::wmoextra
{
    bool Ready() { return s.ready; }
    bool Ground() { return s.ready && s.ground; }
    VkDescriptorSetLayout SetLayout() { return s.setLayout; }

    VkDescriptorSet FrameSet(uint32_t slot, uint64_t serial)
    {
        if (!s.ready || slot >= kSets) return VK_NULL_HANDLE;
        // The terrain's table once per frame, at the frame's first WMO draw (a failed fill keeps the last).
        if (s.ground && s.serials[slot] != serial)
        {
            gxvk::vk::ground::Fill(slot, s.sets[slot]);
            s.serials[slot] = serial;
        }
        return s.sets[slot];
    }

    bool Init()
    {
        if (s.ready) return true;
        if (s.failed) return false;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi) return false;
        s.device = h->device;
        s.dapi   = h->dapi;
        const loader::Device& d = *s.dapi;

        // Binding 0 the extra vertex data; with the terrain's table (where the device can take it, the same test
        // that chose the WMO pixel module) 1 the table and 2 its textures, for the pixel stage.
        s.ground = gxvk::vk::ground::Possible();
        if (s.ground && !gxvk::vk::ground::Init())
        {
            // The pixel module was chosen to read the table: without it the set can't match it (our WMO shader off).
            Write(Error, "vulkan: WMO extra data: the terrain's table could not be made; our WMO shader stays off");
            s.failed = true;
            return false;
        }
        VkDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[1].binding         = gxvk::vk::ground::kTableBinding;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding         = gxvk::vk::ground::kTexturesBinding;
        bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = gxvk::vk::ground::kSlots;
        bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layout = {};
        layout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout.bindingCount = s.ground ? 3u : 1u;
        layout.pBindings    = bindings;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &layout, nullptr, &s.setLayout), "vkCreateDescriptorSetLayout")) return false;

        VkDescriptorPoolSize sizes[2] = {};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[0].descriptorCount = (s.ground ? 2u : 1u) * kSets;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = gxvk::vk::ground::kSlots * kSets;
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = kSets;
        pool.poolSizeCount = s.ground ? 2u : 1u;
        pool.pPoolSizes    = sizes;
        if (!Check(d.CreateDescriptorPool(s.device, &pool, nullptr, &s.pool), "vkCreateDescriptorPool")) return false;

        if (!memory::CreateBuffer(kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory::Where::Stream, s.buffer) || !s.buffer.block.mapped)
        {
            Write(Error, "vulkan: WMO extra data: no buffer");
            s.failed = true;
            return false;
        }

        VkDescriptorBufferInfo info = { s.buffer.handle, 0, kBytes };

        for (uint32_t k = 0; k < kSets; ++k)
        {
            VkDescriptorSetAllocateInfo alloc = {};
            alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool     = s.pool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts        = &s.setLayout;
            if (!Check(d.AllocateDescriptorSets(s.device, &alloc, &s.sets[k]), "vkAllocateDescriptorSets")) return false;

            VkWriteDescriptorSet write = {};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = s.sets[k];
            write.dstBinding      = 0;
            write.descriptorCount = 1;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo     = &info;
            d.UpdateDescriptorSets(s.device, 1, &write, 0, nullptr);
        }

        s.ready = true;
        Write(Info, "vulkan: WMO extra data: ready (%u KiB%s)", static_cast<uint32_t>(kBytes / 1024), s.ground ? ", with the terrain's table" : ", without the terrain's table");
        return true;
    }

    bool Offset(const void* group, uint32_t& vec4Index)
    {
        const SXC::Wmo::GroupExtra* extra = s.ready ? SXC::Wmo::Extra(group) : nullptr;

        // Two vec4 a vertex: (the third UV set, MOC2's bits, 1), (the fourth UV set, 0, 0).
        const uint32_t vec4s = extra ? 2u * extra->vertices : 0u;

        if (!extra || !extra->vertices || vec4s > kVec4s)
        {
            return false;
        }

        Entry& e = s.groups[group];

        if (e.serial == extra->serial)
        {
            vec4Index = e.index;
            return true;
        }

        // A fresh region; when the buffer is full, start over (the groups upload again at their next draw).
        if (s.cursor + vec4s > kVec4s)
        {
            if (s.restarts++ == 0)
            {
                Write(Warn, "vulkan: WMO extra data: the buffer is full; starting over (logged once)");
            }

            s.groups.clear();
            s.cursor = 0;
        }

        Entry& fresh = s.groups[group];
        float* out = reinterpret_cast<float*>(static_cast<uint8_t*>(s.buffer.block.mapped) + static_cast<size_t>(s.cursor) * 16);
        const size_t uv3 = extra->uv3.size() / 2, uv4 = extra->uv4.size() / 2;

        for (uint32_t v = 0; v < extra->vertices; ++v, out += 8)
        {
            const bool hasUv = v < uv3, hasUv4 = v < uv4;
            const uint32_t weights = v < extra->weights.size() ? extra->weights[v] : 0u;
            out[0] = hasUv ? extra->uv3[2 * static_cast<size_t>(v)] : 0.0f;
            out[1] = hasUv ? extra->uv3[2 * static_cast<size_t>(v) + 1] : 0.0f;
            std::memcpy(&out[2], &weights, sizeof weights);
            out[3] = 1.0f;
            out[4] = hasUv4 ? extra->uv4[2 * static_cast<size_t>(v)] : 0.0f;
            out[5] = hasUv4 ? extra->uv4[2 * static_cast<size_t>(v) + 1] : 0.0f;
            out[6] = 0.0f;
            out[7] = 0.0f;
        }

        memory::FlushHost(s.buffer.block);
        fresh.serial = extra->serial;
        fresh.index = s.cursor;
        s.cursor += vec4s;
        vec4Index = fresh.index;
        return true;
    }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        gxvk::vk::ground::Shutdown();
        memory::DestroyBuffer(s.buffer);
        if (s.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        s = State();
    }
}
