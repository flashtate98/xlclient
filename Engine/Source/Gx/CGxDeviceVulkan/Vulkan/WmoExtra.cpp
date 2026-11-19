// SXC: our WMO shader's extra vertex data, the GPU side (Vulkan/WmoExtra.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/WmoExtra.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
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

    struct State
    {
        bool                  ready  = false;
        bool                  failed = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool      pool      = VK_NULL_HANDLE;
        VkDescriptorSet       set       = VK_NULL_HANDLE;
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
    VkDescriptorSetLayout SetLayout() { return s.setLayout; }
    VkDescriptorSet Set() { return s.set; }

    bool Init()
    {
        if (s.ready) return true;
        if (s.failed) return false;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi) return false;
        s.device = h->device;
        s.dapi   = h->dapi;
        const loader::Device& d = *s.dapi;

        VkDescriptorSetLayoutBinding binding = {};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo layout = {};
        layout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout.bindingCount = 1;
        layout.pBindings    = &binding;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &layout, nullptr, &s.setLayout), "vkCreateDescriptorSetLayout")) return false;

        VkDescriptorPoolSize size = {};
        size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        size.descriptorCount = 1;
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes    = &size;
        if (!Check(d.CreateDescriptorPool(s.device, &pool, nullptr, &s.pool), "vkCreateDescriptorPool")) return false;

        VkDescriptorSetAllocateInfo alloc = {};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = s.pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &s.setLayout;
        if (!Check(d.AllocateDescriptorSets(s.device, &alloc, &s.set), "vkAllocateDescriptorSets")) return false;

        if (!memory::CreateBuffer(kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory::Where::Stream, s.buffer) || !s.buffer.block.mapped)
        {
            Write(Error, "vulkan: WMO extra data: no buffer");
            s.failed = true;
            return false;
        }

        VkDescriptorBufferInfo info = { s.buffer.handle, 0, kBytes };
        VkWriteDescriptorSet write = {};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = s.set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo     = &info;
        d.UpdateDescriptorSets(s.device, 1, &write, 0, nullptr);

        s.ready = true;
        Write(Info, "vulkan: WMO extra data: ready (%u KiB)", static_cast<uint32_t>(kBytes / 1024));
        return true;
    }

    bool Offset(const void* group, uint32_t& vec4Index)
    {
        const SXC::Wmo::GroupExtra* extra = s.ready ? SXC::Wmo::Extra(group) : nullptr;

        if (!extra || !extra->vertices || extra->vertices > kVec4s)
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
        if (s.cursor + extra->vertices > kVec4s)
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
        const size_t uv3 = extra->uv3.size() / 2;

        for (uint32_t v = 0; v < extra->vertices; ++v, out += 4)
        {
            const bool hasUv = v < uv3;
            const uint32_t weights = v < extra->weights.size() ? extra->weights[v] : 0u;
            out[0] = hasUv ? extra->uv3[2 * static_cast<size_t>(v)] : 0.0f;
            out[1] = hasUv ? extra->uv3[2 * static_cast<size_t>(v) + 1] : 0.0f;
            std::memcpy(&out[2], &weights, sizeof weights);
            out[3] = 1.0f;
        }

        memory::FlushHost(s.buffer.block);
        fresh.serial = extra->serial;
        fresh.index = s.cursor;
        s.cursor += extra->vertices;
        vec4Index = fresh.index;
        return true;
    }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        memory::DestroyBuffer(s.buffer);
        if (s.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        s = State();
    }
}
