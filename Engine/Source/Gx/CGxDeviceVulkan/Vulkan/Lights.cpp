// SXC: our point lights' GPU side (Vulkan/Lights.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Lights.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/DeviceCaps.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Water.hpp"

#include <cstdio>
#include <cstring>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;
    using gxvk::vk::lights::Frame;
    using gxvk::vk::lights::kMaxCells;
    using gxvk::vk::lights::kMaxLights;
    using gxvk::vk::lights::kMaxPerCell;
    using gxvk::vk::lights::kSurfaceCount;
    using gxvk::vk::lights::kSurfaceOff;

    using gxvk::log::Write;
    using gxvk::log::Info;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    constexpr uint32_t kSlots      = frame::kFramesInFlight;
    constexpr uint32_t kLightBytes = 64;
    /// Shaders/Lights.glsl's LightBlock: the grid (origin x, y, 1 / cell, cells), the info (per cell,
    /// lights, on, debug), the view's inverse (three rows), the shadow maps (face size, 1 / it, near),
    /// then the lights.
    constexpr VkDeviceSize kHeaderBytes = 96;
    constexpr uint32_t kBindings = 9;   ///< + the water's shore (4: its values, 5..8: field, cells, waves, ripples)
    constexpr uint32_t kShoreImages = 4;
    constexpr VkDeviceSize kShoreBytes = 6 * 16;   ///< Shaders/Lights.glsl's LightShore
    constexpr VkDeviceSize kLightsBytes = kHeaderBytes + kMaxLights * kLightBytes;
    constexpr VkDeviceSize kCellsBytes  = static_cast<VkDeviceSize>(kMaxCells) * kMaxCells * kMaxPerCell * sizeof(uint32_t);
    /// Binding 2 (LightSurface): one vec4 per surface class, each at its own aligned offset.
    constexpr VkDeviceSize kClassStride = 256;

    struct Slot
    {
        /// Per surface class (Surface); kSurfaceOff's both buffers are `State::offBuffer` (the lights off).
        VkDescriptorSet sets[kSurfaceCount] = {};
        memory::Buffer  lights, cells;
        memory::Buffer  shore;               ///< binding 4
        uint64_t        serial = 0;
    };

    struct State
    {
        bool                  ready  = false;
        bool                  failed = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;
        gxvk::vk::lights::Provider provider = nullptr;
        gxvk::vk::lights::ShadowReport shadowReport = nullptr;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool      pool      = VK_NULL_HANDLE;
        Slot                  slots[kSlots];
        memory::Buffer        offBuffer;          ///< a zero header: the lights off
        memory::Buffer        classes;            ///< binding 2: the surface classes, kClassStride apart
        // Bound in place of the shore's images until the water has them (white, 1 x 1).
        VkImage               white = VK_NULL_HANDLE;
        VkImageView           whiteView = VK_NULL_HANDLE;
        memory::Block         whiteBlock;
        VkSampler             whiteSampler = VK_NULL_HANDLE;
        bool                  whiteReady = false;   ///< cleared and in SHADER_READ_ONLY_OPTIMAL
        char                  status[160] = "no terrain drawn yet";
    };
    State s;

    bool Check(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        Write(Error, "vulkan: lights: %s failed: %s", what, ResultName(r));
        s.failed = true;
        return false;
    }
}

namespace gxvk::vk::lights
{
    void SetProvider(Provider provider) { s.provider = provider; }
    void SetShadowReport(ShadowReport report) { s.shadowReport = report; }
    bool Ready() { return s.ready; }
    VkDescriptorSetLayout SetLayout() { return s.setLayout; }
    const char* Status() { return s.status; }

    bool Init()
    {
        if (s.ready) return true;
        if (s.failed) return false;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi) return false;
        s.device = h->device;
        s.dapi   = h->dapi;
        const loader::Device& d = *s.dapi;

        // The shadow maps are part of the set: without them, no lights.
        if (!gxvk::vk::shadows::Init())
        {
            Write(Error, "vulkan: lights: the shadow maps could not be made (see above)");
            s.failed = true;
            return false;
        }

        // Read where the pixels are lit (and where the vertices or the tessellation may light later), and by
        // the fog's light volume (FogLights.comp).
        VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        if (context::Caps().enabled & gxvk::vk::kCapTessellation)
            stages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        VkDescriptorSetLayoutBinding bindings[kBindings] = {};
        const VkDescriptorType types[kBindings] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
        for (uint32_t i = 0; i < kBindings; ++i)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = types[i];
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = stages;
        }
        VkDescriptorSetLayoutCreateInfo setLayout = {};
        setLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayout.bindingCount = kBindings;
        setLayout.pBindings    = bindings;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &setLayout, nullptr, &s.setLayout), "vkCreateDescriptorSetLayout")) return false;

        VkDescriptorPoolSize sizes[3] = {};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[0].descriptorCount = 2 * kSurfaceCount * kSlots;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[1].descriptorCount = 2 * kSurfaceCount * kSlots;
        sizes[2].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[2].descriptorCount = (1 + kShoreImages) * kSurfaceCount * kSlots;
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = kSurfaceCount * kSlots;
        pool.poolSizeCount = 3;
        pool.pPoolSizes    = sizes;
        if (!Check(d.CreateDescriptorPool(s.device, &pool, nullptr, &s.pool), "vkCreateDescriptorPool")) return false;

        if (!memory::CreateBuffer(kHeaderBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory::Where::Stream, s.offBuffer) || !s.offBuffer.block.mapped
            || !memory::CreateBuffer(kClassStride * kSurfaceCount, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, s.classes) || !s.classes.block.mapped)
        {
            Write(Error, "vulkan: lights: no buffers for the lights off and the surface classes");
            s.failed = true;
            return false;
        }
        std::memset(s.offBuffer.block.mapped, 0, static_cast<size_t>(kHeaderBytes));
        memory::FlushHost(s.offBuffer.block);
        std::memset(s.classes.block.mapped, 0, static_cast<size_t>(kClassStride * kSurfaceCount));
        for (uint32_t c = 0; c < kSurfaceCount; ++c)
            reinterpret_cast<float*>(static_cast<uint8_t*>(s.classes.block.mapped) + c * kClassStride)[0] = static_cast<float>(c);
        memory::FlushHost(s.classes.block);

        // The white stand-in for the shore's images (cleared at the first frame: FrameSet).
        {
            VkImageCreateInfo info = {};
            info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType     = VK_IMAGE_TYPE_2D;
            info.format        = VK_FORMAT_R8G8B8A8_UNORM;
            info.extent        = VkExtent3D{ 1, 1, 1 };
            info.mipLevels     = 1;
            info.arrayLayers   = 1;
            info.samples       = VK_SAMPLE_COUNT_1_BIT;
            info.tiling        = VK_IMAGE_TILING_OPTIMAL;
            info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!Check(d.CreateImage(s.device, &info, nullptr, &s.white), "vkCreateImage (white)")) return false;
            VkMemoryRequirements req = {};
            d.GetImageMemoryRequirements(s.device, s.white, &req);
            if (!memory::Allocate(req, memory::Where::DeviceLocal, s.whiteBlock)
                || !Check(d.BindImageMemory(s.device, s.white, s.whiteBlock.memory, s.whiteBlock.offset), "vkBindImageMemory (white)"))
            {
                s.failed = true;
                return false;
            }
            VkImageViewCreateInfo view = {};
            view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image                       = s.white;
            view.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
            view.format                      = VK_FORMAT_R8G8B8A8_UNORM;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            if (!Check(d.CreateImageView(s.device, &view, nullptr, &s.whiteView), "vkCreateImageView (white)")) return false;
            VkSamplerCreateInfo sampler = {};
            sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sampler.magFilter    = VK_FILTER_NEAREST;
            sampler.minFilter    = VK_FILTER_NEAREST;
            sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            if (!Check(d.CreateSampler(s.device, &sampler, nullptr, &s.whiteSampler), "vkCreateSampler (white)")) return false;
        }

        auto write = [&](VkDescriptorSet set, VkBuffer lights, VkDeviceSize lightsBytes, VkBuffer cells, VkDeviceSize cellsBytes, uint32_t surface, VkBuffer shore) {
            VkDescriptorBufferInfo infos[4] = { { lights, 0, lightsBytes }, { cells, 0, cellsBytes }, { s.classes.handle, surface * kClassStride, 16 },
                                                { shore, 0, kShoreBytes } };
            VkDescriptorImageInfo shadowMaps = { gxvk::vk::shadows::Sampler(), gxvk::vk::shadows::View(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo white = { s.whiteSampler, s.whiteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet writes[kBindings] = {};
            for (uint32_t i = 0; i < kBindings; ++i)
            {
                writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet          = set;
                writes[i].dstBinding      = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType  = types[i];
                if (i < 3) writes[i].pBufferInfo = &infos[i];
                else if (i == 3) writes[i].pImageInfo = &shadowMaps;
                else if (i == 4) writes[i].pBufferInfo = &infos[3];
                else writes[i].pImageInfo = &white;
            }
            d.UpdateDescriptorSets(s.device, kBindings, writes, 0, nullptr);
        };

        for (Slot& slot : s.slots)
        {
            VkDescriptorSetLayout layouts[kSurfaceCount];
            for (VkDescriptorSetLayout& l : layouts) l = s.setLayout;
            VkDescriptorSetAllocateInfo alloc = {};
            alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool     = s.pool;
            alloc.descriptorSetCount = kSurfaceCount;
            alloc.pSetLayouts        = layouts;
            if (!Check(d.AllocateDescriptorSets(s.device, &alloc, slot.sets), "vkAllocateDescriptorSets")) return false;
            if (!memory::CreateBuffer(kLightsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory::Where::Stream, slot.lights) || !slot.lights.block.mapped
                || !memory::CreateBuffer(kCellsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory::Where::Stream, slot.cells) || !slot.cells.block.mapped
                || !memory::CreateBuffer(kShoreBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, slot.shore) || !slot.shore.block.mapped)
            {
                Write(Error, "vulkan: lights: no buffers for the lights");
                s.failed = true;
                return false;
            }
            std::memset(slot.lights.block.mapped, 0, static_cast<size_t>(kHeaderBytes));
            memory::FlushHost(slot.lights.block);
            std::memset(slot.shore.block.mapped, 0, static_cast<size_t>(kShoreBytes));
            memory::FlushHost(slot.shore.block);

            // The buffers never change: the sets are written once.
            for (uint32_t c = 0; c < kSurfaceCount; ++c)
            {
                if (c == kSurfaceOff)
                    write(slot.sets[c], s.offBuffer.handle, kHeaderBytes, s.offBuffer.handle, kHeaderBytes, 0, slot.shore.handle);
                else
                    write(slot.sets[c], slot.lights.handle, kLightsBytes, slot.cells.handle, kCellsBytes, c, slot.shore.handle);
            }
        }

        s.ready = true;
        Write(Info, "vulkan: lights: ready (up to %u lights, %u x %u cells of up to %u)", kMaxLights, kMaxCells, kMaxCells, kMaxPerCell);
        return true;
    }

    VkDescriptorSet FrameSet(uint32_t slotIndex, uint64_t serial, uint32_t surface)
    {
        if (!s.ready || slotIndex >= kSlots || surface >= kSurfaceCount) return VK_NULL_HANDLE;
        Slot& slot = s.slots[slotIndex];
        if (slot.serial == serial) return slot.sets[surface];

        Frame f;
        const bool on = s.provider && s.provider(f) && f.on && f.cells > 0 && f.cells <= kMaxCells && f.perCell > 0 && f.perCell <= kMaxPerCell
                        && f.cellLights && f.lightCount <= kMaxLights && (f.lightCount == 0 || f.lights);

        // The shadow maps first, into the pre-pass (ahead of every draw of the frame); the first frame
        // also lays out every layer, lights or none.
        const uint32_t drawn = gxvk::vk::shadows::Record(on ? f.shadowJobs : nullptr, on && f.shadowJobs ? f.shadowJobCount : 0);
        if (on && s.shadowReport) s.shadowReport(drawn);

        float header[24] = {};
        header[20] = static_cast<float>(gxvk::vk::shadows::kFaceSize);
        header[21] = 1.0f / static_cast<float>(gxvk::vk::shadows::kFaceSize);
        header[22] = gxvk::vk::shadows::kNear;
        if (on)
        {
            header[0] = f.originX;
            header[1] = f.originY;
            header[2] = 1.0f / f.cellSize;
            header[3] = static_cast<float>(f.cells);
            header[4] = static_cast<float>(f.perCell);
            header[5] = static_cast<float>(f.lightCount);
            header[6] = 1.0f;
            header[7] = static_cast<float>(f.debug);
            std::memcpy(header + 8, f.viewInv, sizeof f.viewInv);
            uint8_t* out = static_cast<uint8_t*>(slot.lights.block.mapped);
            std::memcpy(out + kHeaderBytes, f.lights, static_cast<size_t>(f.lightCount) * kLightBytes);
            std::memcpy(slot.cells.block.mapped, f.cellLights, static_cast<size_t>(f.cells) * f.cells * f.perCell * sizeof(uint32_t));
            memory::FlushHost(slot.cells.block);
            std::snprintf(s.status, sizeof s.status, "%u lights on a %u x %u grid (up to %u per cell)", f.lightCount, f.cells, f.cells, f.perCell);
        }
        else
            std::snprintf(s.status, sizeof s.status, "lights off");
        std::memcpy(slot.lights.block.mapped, header, sizeof header);
        memory::FlushHost(slot.lights.block);

        // The water's shore for the models' caustics (Lights.glsl's LightShore): its values, and its images
        // (or the white stand-in, cleared at the first frame) into every set of this slot.
        if (!s.whiteReady)
        {
            if (VkCommandBuffer pre = frame::PrePass())
            {
                const loader::Device& d = *s.dapi;
                VkImageMemoryBarrier b = {};
                b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
                b.image                       = s.white;
                b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                b.subresourceRange.levelCount = 1;
                b.subresourceRange.layerCount = 1;
                b.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
                d.CmdPipelineBarrier(pre, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                VkClearColorValue one = {};
                one.float32[0] = one.float32[1] = one.float32[2] = one.float32[3] = 1.0f;
                d.CmdClearColorImage(pre, s.white, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &b.subresourceRange);
                b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                d.CmdPipelineBarrier(pre, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                s.whiteReady = true;
            }
        }
        {
            const gxvk::vk::water::ShoreShared shore = gxvk::vk::water::Shore();
            const bool have = shore.valid && s.whiteReady;
            float values[24] = {};
            if (have)
            {
                std::memcpy(values + 0, shore.grid, 16);
                std::memcpy(values + 4, shore.caustic, 16);
                std::memcpy(values + 8, shore.sun, 16);
                std::memcpy(values + 12, shore.sunColour, 16);
                std::memcpy(values + 16, shore.wave, 12);
                values[19] = shore.wind[3];
                std::memcpy(values + 20, shore.camera, 16);
            }
            std::memcpy(slot.shore.block.mapped, values, sizeof values);
            memory::FlushHost(slot.shore.block);

            if (s.whiteReady)
            {
                const VkDescriptorImageInfo white = { s.whiteSampler, s.whiteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo images[kShoreImages] = {
                    have ? VkDescriptorImageInfo{ shore.fieldSampler, shore.field, VK_IMAGE_LAYOUT_GENERAL } : white,
                    have ? VkDescriptorImageInfo{ shore.cellsSampler, shore.cells, VK_IMAGE_LAYOUT_GENERAL } : white,
                    have ? VkDescriptorImageInfo{ shore.wavesSampler, shore.waves, VK_IMAGE_LAYOUT_GENERAL } : white,
                    have ? VkDescriptorImageInfo{ shore.wavesSampler, shore.ripples, VK_IMAGE_LAYOUT_GENERAL } : white,
                };
                VkWriteDescriptorSet writes[kSurfaceCount * kShoreImages] = {};
                uint32_t n = 0;
                for (uint32_t c = 0; c < kSurfaceCount; ++c)
                    for (uint32_t k = 0; k < kShoreImages; ++k)
                    {
                        VkWriteDescriptorSet& w = writes[n++];
                        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        w.dstSet          = slot.sets[c];
                        w.dstBinding      = 5 + k;
                        w.descriptorCount = 1;
                        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        w.pImageInfo      = &images[k];
                    }
                s.dapi->UpdateDescriptorSets(s.device, n, writes, 0, nullptr);
            }
        }

        slot.serial = serial;
        return slot.sets[surface];
    }

    void Shutdown()
    {
        if (!s.dapi || !s.device)
        {
            gxvk::vk::shadows::Shutdown();
            const Provider provider = s.provider;
            const ShadowReport report = s.shadowReport;
            s = State();
            s.provider = provider;
            s.shadowReport = report;
            return;
        }
        const loader::Device& d = *s.dapi;
        for (Slot& slot : s.slots)
        {
            memory::DestroyBuffer(slot.lights);
            memory::DestroyBuffer(slot.cells);
            memory::DestroyBuffer(slot.shore);
        }
        memory::DestroyBuffer(s.offBuffer);
        memory::DestroyBuffer(s.classes);
        if (s.whiteSampler != VK_NULL_HANDLE) d.DestroySampler(s.device, s.whiteSampler, nullptr);
        if (s.whiteView != VK_NULL_HANDLE) d.DestroyImageView(s.device, s.whiteView, nullptr);
        if (s.white != VK_NULL_HANDLE) d.DestroyImage(s.device, s.white, nullptr);
        memory::Free(s.whiteBlock);
        if (s.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        gxvk::vk::shadows::Shutdown();
        const Provider provider = s.provider;
        const ShadowReport report = s.shadowReport;
        s = State();
        s.provider = provider;   // the device's, kept for the next bring-up
        s.shadowReport = report;
    }
}
