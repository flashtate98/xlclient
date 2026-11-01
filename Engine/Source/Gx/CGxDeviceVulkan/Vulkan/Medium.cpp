// SXC: the underwater medium (Vulkan/Medium.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Medium.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Fog.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Water.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/MediumSpirv.inc"
#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/PresentSpirv.inc"

#include <cstdio>
#include <cstring>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    constexpr uint32_t kSlots    = frame::kFramesInFlight;
    constexpr uint32_t kBindings = 6;
    constexpr VkFormat kFormat   = VK_FORMAT_R16G16B16A16_SFLOAT;

    /// Shaders/Medium.comp's MediumParams (std140).
    struct Params
    {
        float invViewProj[16];
        float viewport[4];
        float depth[4];
        float loss[4];
        float glow[4];
        float misc[4];
        float halo[4];
        float line[4];
        float lens[4];
        float origin[4];
        float move[4];
        float moveLook[4];
    };

    struct Slot
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        memory::Buffer  params;
    };

    struct State
    {
        bool                  ready  = false;
        bool                  failed = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;

        VkShaderModule        comp = VK_NULL_HANDLE, vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout      layout    = VK_NULL_HANDLE;
        VkDescriptorPool      pool      = VK_NULL_HANDLE;
        VkPipeline            compute   = VK_NULL_HANDLE;

        // The variant reading the water's movement (Medium.comp WAVES 1): the water's set 2 as set 2, set 1
        // empty. Made at the first frame the water's set is there; `wavesTried` once.
        VkShaderModule        wavesComp   = VK_NULL_HANDLE;
        VkDescriptorSetLayout emptyLayout = VK_NULL_HANDLE;
        VkPipelineLayout      wavesLayout = VK_NULL_HANDLE;
        VkPipeline            wavesPipe   = VK_NULL_HANDLE;
        bool                  wavesTried  = false;
        VkSampler             nearestClamp = VK_NULL_HANDLE;
        Slot                  slots[kSlots];


        // The copy back's pass and pipeline, for one colour format (remade when it changes).
        VkFormat     format = VK_FORMAT_UNDEFINED;
        VkRenderPass pass   = VK_NULL_HANDLE;
        VkPipeline   copy   = VK_NULL_HANDLE;

        // Our image (Medium.comp writes it, MediumCopy.frag reads it; it stays in GENERAL), the target's size.
        VkImage       image  = VK_NULL_HANDLE;
        VkImageView   view   = VK_NULL_HANDLE;
        memory::Block block;
        VkExtent2D    extent = { 0, 0 };
        VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint64_t      lastUsed = 0;

        char status[160] = "not drawn yet";
    };
    State s;

    void SetStatus(const char* text)
    {
        std::snprintf(s.status, sizeof s.status, "%s", text);
    }

    bool Check(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        Write(Error, "vulkan: medium: %s failed: %s", what, ResultName(r));
        s.failed = true;
        return false;
    }

    bool CreateModule(const uint32_t* words, size_t bytes, VkShaderModule& out, const char* what)
    {
        VkShaderModuleCreateInfo info = {};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes;
        info.pCode    = words;
        return Check(s.dapi->CreateShaderModule(s.device, &info, nullptr, &out), what);
    }

    void DestroyImage()
    {
        if (s.view != VK_NULL_HANDLE) s.dapi->DestroyImageView(s.device, s.view, nullptr);
        if (s.image != VK_NULL_HANDLE) s.dapi->DestroyImage(s.device, s.image, nullptr);
        memory::Free(s.block);
        s.view        = VK_NULL_HANDLE;
        s.image       = VK_NULL_HANDLE;
        s.block       = memory::Block();
        s.extent      = { 0, 0 };
        s.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    /// Our image at the target's size; another size remakes it once the frames that used it are done.
    bool EnsureImage(VkExtent2D extent)
    {
        if (s.image != VK_NULL_HANDLE && s.extent.width == extent.width && s.extent.height == extent.height) return true;
        if (s.image != VK_NULL_HANDLE)
        {
            if (s.lastUsed) frame::WaitSerial(s.lastUsed);
            DestroyImage();
        }

        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = kFormat;
        info.extent        = VkExtent3D{ extent.width, extent.height, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(s.dapi->CreateImage(s.device, &info, nullptr, &s.image), "vkCreateImage")) return false;
        VkMemoryRequirements req = {};
        s.dapi->GetImageMemoryRequirements(s.device, s.image, &req);
        if (!memory::Allocate(req, memory::Where::DeviceLocal, s.block))
        {
            Write(Error, "vulkan: medium: no device memory for a %ux%u image", extent.width, extent.height);
            s.dapi->DestroyImage(s.device, s.image, nullptr);
            s.image  = VK_NULL_HANDLE;
            s.failed = true;
            return false;
        }
        VkImageViewCreateInfo view = {};
        view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image                       = s.image;
        view.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        view.format                      = kFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (!Check(s.dapi->BindImageMemory(s.device, s.image, s.block.memory, s.block.offset), "vkBindImageMemory")
            || !Check(s.dapi->CreateImageView(s.device, &view, nullptr, &s.view), "vkCreateImageView"))
        {
            DestroyImage();
            return false;
        }
        s.extent      = extent;
        s.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspects, VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier b = {};
        b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask               = srcAccess;
        b.dstAccessMask               = dstAccess;
        b.oldLayout                   = from;
        b.newLayout                   = to;
        b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.image                       = image;
        b.subresourceRange.aspectMask = aspects;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        s.dapi->CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
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

        if (!CreateModule(kMediumComp, sizeof kMediumComp, s.comp, "vkCreateShaderModule (Medium.comp)")
            || !CreateModule(kPresentVert, sizeof kPresentVert, s.vert, "vkCreateShaderModule (Present.vert)")
            || !CreateModule(kMediumCopyFrag, sizeof kMediumCopyFrag, s.frag, "vkCreateShaderModule (MediumCopy.frag)"))
            return false;

        // One set for both: 0 the parameters, 1 the target's colour, 2 its depth, 3 our image to write,
        // 4 our image to read, 5 our lamps' light volume (the fog's).
        const VkShaderStageFlags both = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkDescriptorType types[kBindings] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
        VkDescriptorSetLayoutBinding bindings[kBindings] = {};
        for (uint32_t i = 0; i < kBindings; ++i)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = types[i];
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = both;
        }
        VkDescriptorSetLayoutCreateInfo setLayout = {};
        setLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayout.bindingCount = kBindings;
        setLayout.pBindings    = bindings;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &setLayout, nullptr, &s.setLayout), "vkCreateDescriptorSetLayout")) return false;

        VkPipelineLayoutCreateInfo layout = {};
        layout.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts    = &s.setLayout;
        if (!Check(d.CreatePipelineLayout(s.device, &layout, nullptr, &s.layout), "vkCreatePipelineLayout")) return false;

        VkDescriptorPoolSize sizes[3] = {};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = kSlots;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = 4 * kSlots;
        sizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sizes[2].descriptorCount = kSlots;
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = kSlots;
        pool.poolSizeCount = 3;
        pool.pPoolSizes    = sizes;
        if (!Check(d.CreateDescriptorPool(s.device, &pool, nullptr, &s.pool), "vkCreateDescriptorPool")) return false;

        for (Slot& slot : s.slots)
        {
            VkDescriptorSetAllocateInfo alloc = {};
            alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool     = s.pool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts        = &s.setLayout;
            if (!Check(d.AllocateDescriptorSets(s.device, &alloc, &slot.set), "vkAllocateDescriptorSets")) return false;
            if (!memory::CreateBuffer(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, slot.params) || !slot.params.block.mapped)
            {
                Write(Error, "vulkan: medium: no buffer for the parameters");
                s.failed = true;
                return false;
            }
        }

        VkComputePipelineCreateInfo compute = {};
        compute.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compute.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = s.comp;
        compute.stage.pName  = "main";
        compute.layout       = s.layout;
        if (!Check(d.CreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &compute, nullptr, &s.compute), "vkCreateComputePipelines")) return false;

        VkSamplerCreateInfo sampler = {};
        sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter    = VK_FILTER_NEAREST;
        sampler.minFilter    = VK_FILTER_NEAREST;
        sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!Check(d.CreateSampler(s.device, &sampler, nullptr, &s.nearestClamp), "vkCreateSampler")) return false;

        s.ready = true;
        Write(Debug, "vulkan: medium: ready");
        return true;
    }

    /// The variant with the water's movement; tried once, when the water's set layout is there.
    bool EnsureWavesPipeline()
    {
        if (s.wavesPipe != VK_NULL_HANDLE) return true;
        const VkDescriptorSetLayout water = gxvk::vk::water::SetLayout();
        if (s.wavesTried || water == VK_NULL_HANDLE) return false;
        s.wavesTried = true;
        const loader::Device& d = *s.dapi;
        if (!CreateModule(kMediumWavesComp, sizeof kMediumWavesComp, s.wavesComp, "vkCreateShaderModule (Medium.comp, waves)")) return false;
        VkDescriptorSetLayoutCreateInfo empty = {};
        empty.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &empty, nullptr, &s.emptyLayout), "vkCreateDescriptorSetLayout (empty)")) return false;
        const VkDescriptorSetLayout sets[3] = { s.setLayout, s.emptyLayout, water };
        VkPipelineLayoutCreateInfo layout = {};
        layout.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 3;
        layout.pSetLayouts    = sets;
        if (!Check(d.CreatePipelineLayout(s.device, &layout, nullptr, &s.wavesLayout), "vkCreatePipelineLayout (medium, waves)")) return false;
        VkComputePipelineCreateInfo compute = {};
        compute.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compute.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = s.wavesComp;
        compute.stage.pName  = "main";
        compute.layout       = s.wavesLayout;
        if (!Check(d.CreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &compute, nullptr, &s.wavesPipe), "vkCreateComputePipelines (medium, waves)"))
        {
            s.wavesPipe = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    /// The copy back's pass over a colour format: the target is loaded, written over (rgb), left in its
    /// resting layout; the same dependencies as the fog's composite.
    bool EnsureCopy(VkFormat format)
    {
        if (s.format == format && s.copy != VK_NULL_HANDLE) return true;
        const loader::Device& d = *s.dapi;
        if (s.copy != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.copy, nullptr);
        if (s.pass != VK_NULL_HANDLE) d.DestroyRenderPass(s.device, s.pass, nullptr);
        s.copy   = VK_NULL_HANDLE;
        s.pass   = VK_NULL_HANDLE;
        s.format = format;

        VkAttachmentDescription a = {};
        a.format         = format;
        a.samples        = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        a.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;
        VkSubpassDependency dep[2] = {};
        dep[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass    = 0;
        dep[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                               | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dep[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass    = 0;
        dep[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                               | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo info = {};
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments    = &a;
        info.subpassCount    = 1;
        info.pSubpasses      = &sub;
        info.dependencyCount = 2;
        info.pDependencies   = dep;
        if (!Check(d.CreateRenderPass(s.device, &info, nullptr, &s.pass), "vkCreateRenderPass")) return false;

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = s.vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = s.frag;
        stages[1].pName  = "main";
        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly = {};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport = {};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;
        VkPipelineRasterizationStateCreateInfo raster = {};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth   = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // Our image over the target, the target's alpha kept.
        VkPipelineColorBlendAttachmentState blend = {};
        blend.blendEnable    = VK_FALSE;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo blending = {};
        blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blending.attachmentCount = 1;
        blending.pAttachments    = &blend;
        const VkDynamicState dynamics[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = {};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamics;
        VkGraphicsPipelineCreateInfo pipeline = {};
        pipeline.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline.stageCount          = 2;
        pipeline.pStages             = stages;
        pipeline.pVertexInputState   = &vertexInput;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState      = &viewport;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState   = &multisample;
        pipeline.pColorBlendState    = &blending;
        pipeline.pDynamicState       = &dynamic;
        pipeline.layout              = s.layout;
        pipeline.renderPass          = s.pass;
        return Check(d.CreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s.copy), "vkCreateGraphicsPipelines (medium)");
    }

    /// A 4 x 4 inverse (row vectors as stored); false when singular. Fog.cpp's.
    bool Invert(const float in[16], float out[16])
    {
        double m[4][8];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                m[r][c]     = in[r * 4 + c];
                m[r][c + 4] = r == c ? 1.0 : 0.0;
            }
        for (int c = 0; c < 4; ++c)
        {
            int pivot = c;
            for (int r = c + 1; r < 4; ++r)
                if ((m[r][c] < 0 ? -m[r][c] : m[r][c]) > (m[pivot][c] < 0 ? -m[pivot][c] : m[pivot][c])) pivot = r;
            if (m[pivot][c] == 0.0) return false;
            if (pivot != c)
                for (int k = 0; k < 8; ++k) { const double t = m[c][k]; m[c][k] = m[pivot][k]; m[pivot][k] = t; }
            const double inv = 1.0 / m[c][c];
            for (int k = 0; k < 8; ++k) m[c][k] *= inv;
            for (int r = 0; r < 4; ++r)
            {
                if (r == c) continue;
                const double f = m[r][c];
                for (int k = 0; k < 8; ++k) m[r][k] -= f * m[c][k];
            }
        }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<float>(m[r][c + 4]);
        return true;
    }
}

namespace gxvk::vk::medium
{
    bool Record(const Frame& f)
    {
        if (!frame::DepthSampled())
        {
            SetStatus("off: the depth buffers can't be sampled on this device");
            return false;
        }
        if (!Init())
        {
            SetStatus("off: the medium's objects could not be made (see the log)");
            return false;
        }

        Params p = {};
        if (!Invert(f.viewProj, p.invViewProj))
        {
            SetStatus("skipped: the view could not be inverted");
            return false;
        }

        frame::EffectTarget t;
        if (!frame::BeginEffect(t))
        {
            char line[128];
            std::snprintf(line, sizeof line, "skipped: %s", frame::EffectRefusal());
            SetStatus(line);
            return false;
        }
        pipelines::ForgetBound();
        if (!EnsureCopy(t.format))
        {
            SetStatus("off: the copy pipeline could not be made");
            return false;
        }
        if (!EnsureImage(t.extent))
        {
            SetStatus("off: no memory for the medium's image");
            return false;
        }

        const loader::Device& d   = *s.dapi;
        VkCommandBuffer       cmd = t.cmd;
        Slot&                 slot = s.slots[frame::Slot()];

        p.viewport[0] = f.viewport.x;
        p.viewport[1] = f.viewport.y;
        p.viewport[2] = f.viewport.width;
        p.viewport[3] = f.viewport.height;
        p.depth[0]    = f.viewport.minDepth;
        p.depth[1]    = f.viewport.maxDepth;
        for (int k = 0; k < 3; ++k)
        {
            p.loss[k] = f.loss[k] > 0.0f ? f.loss[k] : 0.0f;
            p.glow[k] = f.glow[k];
        }
        p.glow[3] = f.desaturate > 0.0f ? f.desaturate : 0.0f;
        p.misc[0] = static_cast<float>(f.debug);
        p.misc[1] = f.fade > 0.0f ? f.fade : 0.0f;
        std::memcpy(p.line, f.line, sizeof p.line);
        p.lens[0] = f.lens[0];
        p.lens[1] = f.lens[1];
        std::memcpy(slot.params.block.mapped, &p, sizeof p);
        memory::FlushHost(slot.params.block);

        VkDescriptorBufferInfo params = { slot.params.handle, 0, sizeof(Params) };
        VkDescriptorImageInfo images[kBindings] = {
            {},
            { s.nearestClamp, t.colour, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { s.nearestClamp, t.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
            { VK_NULL_HANDLE, s.view, VK_IMAGE_LAYOUT_GENERAL },
            { s.nearestClamp, s.view, VK_IMAGE_LAYOUT_GENERAL },
        };
        VkWriteDescriptorSet writes[kBindings] = {};
        for (uint32_t i = 0; i < kBindings - 1; ++i)   // 5, the light volume, below
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = slot.set;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = &images[i];
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pImageInfo     = nullptr;
        writes[0].pBufferInfo    = &params;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        d.UpdateDescriptorSets(s.device, kBindings - 1, writes, 0, nullptr);

        // The world's colour writes done before we read the target; the last frame's copy back done with
        // our image before we write it; the depth from attachment to read-only.
        VkMemoryBarrier colourDone = {};
        colourDone.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        colourDone.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        colourDone.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &colourDone, 0, nullptr, 0, nullptr);
        Barrier(cmd, s.image, VK_IMAGE_ASPECT_COLOR_BIT, s.imageLayout, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        s.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        Barrier(cmd, t.depth, t.depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

        // Our lamps' light in the water: the fog's light volume, filled for this view.
        gxvk::vk::fog::LightVolumeInput lv;
        std::memcpy(lv.viewProj, f.viewProj, sizeof lv.viewProj);
        std::memcpy(lv.origin, f.origin, sizeof lv.origin);
        lv.viewport = f.viewport;
        lv.start    = 0.5f;
        lv.range    = f.haloRange;
        lv.glow     = f.haloGlow;
        lv.strength = 1.0f;
        gxvk::vk::fog::LightVolumeOutput volume;
        const bool filled = f.halo > 0.0f && gxvk::vk::fog::LightVolume(cmd, t.serial, t.depthView, lv, volume);
        if (volume.view == VK_NULL_HANDLE)
        {
            SetStatus("off: the fog's light volume isn't there (see the log)");
            return false;
        }
        p.halo[0] = filled ? f.halo : 0.0f;
        p.halo[1] = lv.start;
        p.halo[2] = lv.range > lv.start + 1.0f ? lv.range : lv.start + 1.0f;
        p.halo[3] = filled ? 1.0f : 0.0f;
        p.origin[0] = f.origin[0];
        p.origin[1] = f.origin[1];
        p.origin[2] = f.origin[2];
        float moveTime = 0.0f;
        const VkDescriptorSet moveSet = EnsureWavesPipeline() ? gxvk::vk::water::MoveSet(frame::Slot(), t.serial, moveTime, p.moveLook) : VK_NULL_HANDLE;
        p.move[0] = f.still;
        p.move[1] = moveTime;
        p.move[2] = moveSet != VK_NULL_HANDLE && f.line[3] > 0.5f ? 1.0f : 0.0f;
        std::memcpy(slot.params.block.mapped, &p, sizeof p);
        memory::FlushHost(slot.params.block);
        const VkDescriptorImageInfo volumeInfo = { volume.sampler, volume.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet volumeWrite = {};
        volumeWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        volumeWrite.dstSet          = slot.set;
        volumeWrite.dstBinding      = 5;
        volumeWrite.descriptorCount = 1;
        volumeWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        volumeWrite.pImageInfo      = &volumeInfo;
        d.UpdateDescriptorSets(s.device, 1, &volumeWrite, 0, nullptr);
        pipelines::ForgetBound();

        if (moveSet != VK_NULL_HANDLE)
        {
            d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.wavesPipe);
            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.wavesLayout, 0, 1, &slot.set, 0, nullptr);
            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.wavesLayout, 2, 1, &moveSet, 0, nullptr);
        }
        else
        {
            d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.compute);
            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &slot.set, 0, nullptr);
        }
        d.CmdDispatch(cmd, (t.extent.width + 7) / 8, (t.extent.height + 7) / 8, 1);

        // Our image to the copy back; the depth back for the world's draws (the compute's reads done).
        Barrier(cmd, s.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        Barrier(cmd, t.depth, t.depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkFramebuffer fb = VK_NULL_HANDLE;
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = s.pass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &t.colour;
        fbInfo.width           = t.extent.width;
        fbInfo.height          = t.extent.height;
        fbInfo.layers          = 1;
        const bool drawn = Check(d.CreateFramebuffer(s.device, &fbInfo, nullptr, &fb), "vkCreateFramebuffer (medium)");

        if (drawn)
        {
            // The pass's own dependency waits for the compute's reads of the target before writing it.
            VkRenderPassBeginInfo begin = {};
            begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass        = s.pass;
            begin.framebuffer       = fb;
            begin.renderArea.extent = t.extent;
            d.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
            d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.copy);
            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &slot.set, 0, nullptr);
            VkViewport viewport = f.viewport;
            viewport.minDepth   = 0.0f;
            viewport.maxDepth   = 1.0f;
            VkRect2D scissor = {};
            scissor.offset = VkOffset2D{ static_cast<int32_t>(f.viewport.x), static_cast<int32_t>(f.viewport.y) };
            scissor.extent = VkExtent2D{ static_cast<uint32_t>(f.viewport.width), static_cast<uint32_t>(f.viewport.height) };
            d.CmdSetViewport(cmd, 0, 1, &viewport);
            d.CmdSetScissor(cmd, 0, 1, &scissor);
            d.CmdDraw(cmd, 3, 1, 0, 0);
            d.CmdEndRenderPass(cmd);
            frame::RetireFramebuffer(fb, t.serial);
        }

        s.lastUsed = t.serial;
        char line[160];
        std::snprintf(line, sizeof line, "drawn: %ux%u target, lamps in the water %s", t.extent.width, t.extent.height,
                      p.halo[3] > 0.5f ? "in" : (f.halo > 0.0f ? "unavailable (our lights off?)" : "off"));
        SetStatus(drawn ? line : "skipped: the framebuffer could not be made");
        return drawn;
    }

    bool UnderMask(uint64_t serial, VkImageView& view)
    {
        view = s.view;
        return s.ready && s.view != VK_NULL_HANDLE && s.lastUsed == serial;
    }

    const char* Status() { return s.status; }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        if (s.copy != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.copy, nullptr);
        if (s.pass != VK_NULL_HANDLE) d.DestroyRenderPass(s.device, s.pass, nullptr);
        if (s.compute != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.compute, nullptr);
        if (s.wavesPipe != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.wavesPipe, nullptr);
        if (s.wavesLayout != VK_NULL_HANDLE) d.DestroyPipelineLayout(s.device, s.wavesLayout, nullptr);
        if (s.emptyLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.emptyLayout, nullptr);
        if (s.wavesComp != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.wavesComp, nullptr);
        DestroyImage();
        for (Slot& slot : s.slots) memory::DestroyBuffer(slot.params);
        if (s.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.nearestClamp != VK_NULL_HANDLE) d.DestroySampler(s.device, s.nearestClamp, nullptr);
        if (s.layout != VK_NULL_HANDLE) d.DestroyPipelineLayout(s.device, s.layout, nullptr);
        if (s.setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        if (s.comp != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.comp, nullptr);
        if (s.vert != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.vert, nullptr);
        if (s.frag != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.frag, nullptr);
        s = State();
    }
}
