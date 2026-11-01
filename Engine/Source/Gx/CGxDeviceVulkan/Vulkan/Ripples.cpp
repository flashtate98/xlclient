// SXC: the ripple field (Vulkan/Ripples.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Ripples.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/WaterSpirv.inc"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;
    namespace ripples = gxvk::vk::ripples;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    constexpr uint32_t kSlots      = frame::kFramesInFlight;
    constexpr uint32_t kDispatches = 6;      // a clear and up to four steps per frame, one spare
    constexpr uint32_t kRegion     = 1280;   // bytes per dispatch's parameters (a multiple of 256)
    constexpr float    kStepRate   = 60.0f;  // Hz

    /// Shaders/RippleStep.comp's RippleStep (std140).
    struct Params
    {
        int32_t grid[4];
        int32_t previous[4];
        int32_t layers[4];
        float   wave[4];
        float   stamps[ripples::kStamps][4];
    };
    static_assert(sizeof(Params) <= kRegion, "the ripple step's parameters outgrew their region");

    struct State
    {
        bool                  ready  = false;
        bool                  failed = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;

        VkShaderModule        module    = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout      layout    = VK_NULL_HANDLE;
        VkDescriptorPool      pool      = VK_NULL_HANDLE;
        VkPipeline            pipeline  = VK_NULL_HANDLE;
        VkSampler             sampler   = VK_NULL_HANDLE;
        VkDescriptorSet       sets[kSlots] = {};
        memory::Buffer        params[kSlots];

        VkImage       image      = VK_NULL_HANDLE;
        VkImageView   view       = VK_NULL_HANDLE;   // the three layers, for the step (storage) and the draws
        memory::Block block;
        bool          cleared    = false;

        int32_t  firstX = 0, firstY = 0;   // the window's first cell
        bool     placed = false;
        uint32_t before = 0, current = 1;  // the layers
        float    pending = 0.0f;           // s not stepped yet
        uint64_t steppedSerial = 0;
        uint32_t lastStamps = 0, lastSteps = 0;

        char status[160] = "not stepped yet";
    };
    State s;

    bool Check(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        Write(Error, "vulkan: ripples: %s failed: %s", what, ResultName(r));
        s.failed = true;
        return false;
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

        VkShaderModuleCreateInfo moduleInfo = {};
        moduleInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = sizeof kRippleStepComp;
        moduleInfo.pCode    = kRippleStepComp;
        if (!Check(d.CreateShaderModule(s.device, &moduleInfo, nullptr, &s.module), "vkCreateShaderModule (RippleStep.comp)")) return false;

        // 0 the step's parameters (a region per dispatch), 1 the field.
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo setLayout = {};
        setLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayout.bindingCount = 2;
        setLayout.pBindings    = bindings;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &setLayout, nullptr, &s.setLayout), "vkCreateDescriptorSetLayout")) return false;

        VkPipelineLayoutCreateInfo layout = {};
        layout.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts    = &s.setLayout;
        if (!Check(d.CreatePipelineLayout(s.device, &layout, nullptr, &s.layout), "vkCreatePipelineLayout")) return false;

        VkComputePipelineCreateInfo compute = {};
        compute.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compute.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = s.module;
        compute.stage.pName  = "main";
        compute.layout       = s.layout;
        if (!Check(d.CreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &compute, nullptr, &s.pipeline), "vkCreateComputePipelines")) return false;

        // The field: three layers of r32f.
        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = VK_FORMAT_R32_SFLOAT;
        info.extent        = VkExtent3D{ ripples::kSize, ripples::kSize, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 3;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(d.CreateImage(s.device, &info, nullptr, &s.image), "vkCreateImage")) return false;
        VkMemoryRequirements req = {};
        d.GetImageMemoryRequirements(s.device, s.image, &req);
        if (!memory::Allocate(req, memory::Where::DeviceLocal, s.block) || !Check(d.BindImageMemory(s.device, s.image, s.block.memory, s.block.offset), "vkBindImageMemory"))
        {
            s.failed = true;
            return false;
        }
        VkImageViewCreateInfo view = {};
        view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image                       = s.image;
        view.viewType                    = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format                      = VK_FORMAT_R32_SFLOAT;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 3;
        if (!Check(d.CreateImageView(s.device, &view, nullptr, &s.view), "vkCreateImageView")) return false;

        VkSamplerCreateInfo sampler = {};
        sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter    = VK_FILTER_LINEAR;
        sampler.minFilter    = VK_FILTER_LINEAR;
        sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!Check(d.CreateSampler(s.device, &sampler, nullptr, &s.sampler), "vkCreateSampler")) return false;

        VkDescriptorPoolSize sizes[2] = {};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        sizes[0].descriptorCount = kSlots;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sizes[1].descriptorCount = kSlots;
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = kSlots;
        pool.poolSizeCount = 2;
        pool.pPoolSizes    = sizes;
        if (!Check(d.CreateDescriptorPool(s.device, &pool, nullptr, &s.pool), "vkCreateDescriptorPool")) return false;

        for (uint32_t i = 0; i < kSlots; ++i)
        {
            VkDescriptorSetAllocateInfo alloc = {};
            alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool     = s.pool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts        = &s.setLayout;
            if (!Check(d.AllocateDescriptorSets(s.device, &alloc, &s.sets[i]), "vkAllocateDescriptorSets")) return false;
            if (!memory::CreateBuffer(kRegion * kDispatches, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, s.params[i]) || !s.params[i].block.mapped)
            {
                Write(Error, "vulkan: ripples: no buffer for the parameters");
                s.failed = true;
                return false;
            }
            VkDescriptorBufferInfo buffer = { s.params[i].handle, 0, sizeof(Params) };
            VkDescriptorImageInfo image = { VK_NULL_HANDLE, s.view, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet writes[2] = {};
            writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet          = s.sets[i];
            writes[0].dstBinding      = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[0].pBufferInfo     = &buffer;
            writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet          = s.sets[i];
            writes[1].dstBinding      = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo      = &image;
            d.UpdateDescriptorSets(s.device, 2, writes, 0, nullptr);
        }

        s.ready = true;
        Write(Debug, "vulkan: ripples: ready (%ux%u cells of %.3f yd)", ripples::kSize, ripples::kSize, ripples::kCell);
        return true;
    }

    void Barrier(VkCommandBuffer cmd, VkImageLayout from, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier b = {};
        b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask               = srcAccess;
        b.dstAccessMask               = dstAccess;
        b.oldLayout                   = from;
        b.newLayout                   = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.image                       = s.image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 3;
        s.dapi->CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    int32_t FloorDiv(float v, float cell)
    {
        return static_cast<int32_t>(std::floor(v / cell));
    }
}

namespace gxvk::vk::ripples
{
    bool Step(VkCommandBuffer cmd, const Input& in)
    {
        if (!in.on || !cmd)
        {
            std::snprintf(s.status, sizeof s.status, "off");
            return false;
        }
        if (!Init())
        {
            std::snprintf(s.status, sizeof s.status, "off: the field could not be made (see the log)");
            return false;
        }

        const loader::Device& d = *s.dapi;
        const uint32_t slot = frame::Slot();
        uint8_t* mapped = static_cast<uint8_t*>(s.params[slot].block.mapped);
        constexpr VkPipelineStageFlags kReaders = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

        // The first time: the layers zeroed.
        if (!s.cleared)
        {
            Barrier(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue zero = {};
            VkImageSubresourceRange range = {};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 3;
            d.CmdClearColorImage(cmd, s.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            Barrier(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            s.cleared = true;
        }
        else
        {
            // The last frame's draws (and steps) done with the field.
            Barrier(cmd, VK_IMAGE_LAYOUT_GENERAL, kReaders, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }

        d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
        const uint32_t groups = (kSize + 7) / 8;
        uint32_t region = 0;
        auto dispatch = [&](const Params& p) {
            std::memcpy(mapped + region * kRegion, &p, sizeof p);
            const uint32_t offset = region * kRegion;
            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &s.sets[slot], 1, &offset);
            d.CmdDispatch(cmd, groups, groups, 1);
            Barrier(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            ++region;
        };

        // The window: centred on the camera, moved once it drifts an eighth off (the new cells cleared); a
        // jump (a teleport) clears it all.
        const int32_t centreX = FloorDiv(in.camera[0], kCell) - static_cast<int32_t>(kSize / 2);
        const int32_t centreY = FloorDiv(in.camera[1], kCell) - static_cast<int32_t>(kSize / 2);
        const int32_t drift = static_cast<int32_t>(kSize / 8);
        const bool move = !s.placed || std::abs(centreX - s.firstX) > drift || std::abs(centreY - s.firstY) > drift;

        if (move)
        {
            Params p = {};
            p.grid[0]     = centreX;
            p.grid[1]     = centreY;
            p.grid[2]     = static_cast<int32_t>(kSize);
            p.grid[3]     = 1;
            p.previous[0] = s.placed ? s.firstX : centreX - 4 * static_cast<int32_t>(kSize);
            p.previous[1] = s.placed ? s.firstY : centreY - 4 * static_cast<int32_t>(kSize);
            dispatch(p);
            s.firstX = centreX;
            s.firstY = centreY;
            s.placed = true;
        }

        // The steps the time asks for (a fixed rate, at most four a frame); the stamps in the first.
        s.pending += in.elapsed > 0.0f && in.elapsed < 1.0f ? in.elapsed : 0.0f;
        uint32_t steps = static_cast<uint32_t>(s.pending * kStepRate);
        steps = steps > 4 ? 4 : steps;
        s.pending -= static_cast<float>(steps) / kStepRate;
        s.pending = s.pending > 4.0f / kStepRate ? 0.0f : s.pending;
        if (steps == 0 && in.stamps > 0) steps = 1;

        const float dt = 1.0f / kStepRate;
        float courant = in.speed * dt / kCell;
        courant = courant > 0.6f ? 0.6f : courant;
        // Damped to a tenth over `life`: per step d^(life * rate) = 0.1.
        const float life = in.life > 0.1f ? in.life : 0.1f;
        const float damping = std::pow(0.1f, 1.0f / (life * kStepRate));

        for (uint32_t i = 0; i < steps; ++i)
        {
            Params p = {};
            p.grid[0]     = s.firstX;
            p.grid[1]     = s.firstY;
            p.grid[2]     = static_cast<int32_t>(kSize);
            p.grid[3]     = 0;
            p.previous[2] = i == 0 ? static_cast<int32_t>(in.stamps < kStamps ? in.stamps : kStamps) : 0;
            const uint32_t next = 3 - s.before - s.current;
            p.layers[0]   = static_cast<int32_t>(s.before);
            p.layers[1]   = static_cast<int32_t>(s.current);
            p.layers[2]   = static_cast<int32_t>(next);
            p.wave[0]     = courant * courant;
            p.wave[1]     = damping;
            p.wave[2]     = kCell;
            if (i == 0) std::memcpy(p.stamps, in.stamp, sizeof p.stamps);
            dispatch(p);
            s.before  = s.current;
            s.current = next;
        }

        memory::FlushHost(s.params[slot].block);

        // The field to the draws' fragment stage.
        Barrier(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, kReaders, VK_ACCESS_SHADER_READ_BIT);

        s.steppedSerial = frame::Serial();
        s.lastStamps    = in.stamps;
        s.lastSteps     = steps;
        std::snprintf(s.status, sizeof s.status, "stepped: %u step(s), %u stamp(s), window at (%d, %d)", steps, in.stamps, s.firstX, s.firstY);
        return true;
    }

    VkImageView View() { return s.ready ? s.view : VK_NULL_HANDLE; }
    VkSampler   Sampler() { return s.ready ? s.sampler : VK_NULL_HANDLE; }

    void Look(float grid[4], float look[4])
    {
        grid[0] = static_cast<float>(s.firstX);
        grid[1] = static_cast<float>(s.firstY);
        grid[2] = static_cast<float>(kSize);
        grid[3] = kCell;
        look[0] = static_cast<float>(s.current);
        look[1] = s.ready && s.steppedSerial == frame::Serial() ? 1.0f : 0.0f;
        look[2] = 0.0f;
        look[3] = 0.0f;
    }

    const char* Status() { return s.status; }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        if (s.pipeline != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.pipeline, nullptr);
        if (s.layout != VK_NULL_HANDLE) d.DestroyPipelineLayout(s.device, s.layout, nullptr);
        if (s.setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        if (s.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.module != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.module, nullptr);
        if (s.sampler != VK_NULL_HANDLE) d.DestroySampler(s.device, s.sampler, nullptr);
        if (s.view != VK_NULL_HANDLE) d.DestroyImageView(s.device, s.view, nullptr);
        if (s.image != VK_NULL_HANDLE) d.DestroyImage(s.device, s.image, nullptr);
        memory::Free(s.block);
        for (memory::Buffer& b : s.params) memory::DestroyBuffer(b);
        s = State();
    }
}
