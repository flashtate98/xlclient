// SXC: the lights' cube shadow maps (Vulkan/Shadows.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Shadows.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Timings.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/ShadowSpirv.inc"

#include <cstring>
#include <vector>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;
    namespace timings = gxvk::vk::timings;
    using gxvk::vk::shadows::Job;
    using gxvk::vk::shadows::Part;
    using gxvk::vk::shadows::kMaxTexturedParts;
    using gxvk::vk::shadows::kFaceSize;
    using gxvk::vk::shadows::kMaxSlots;
    using gxvk::vk::shadows::kMaxVertices;
    using gxvk::vk::shadows::kNear;

    using gxvk::log::Write;
    using gxvk::log::Info;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    constexpr VkFormat kFormat = VK_FORMAT_D16_UNORM;   // depth attachment and sampled: every device
    constexpr uint32_t kSlotsInFlight = frame::kFramesInFlight;
    constexpr VkDeviceSize kVertexBytes = 20;   // xyz, uv

    struct State
    {
        bool                  ready  = false;
        bool                  failed = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;
        uint32_t              slots  = 0;

        VkImage               image  = VK_NULL_HANDLE;
        memory::Block         block;
        VkImageView           view   = VK_NULL_HANDLE;   ///< the whole array
        std::vector<VkImageView>   layerViews;           ///< one per face, for its framebuffer
        std::vector<VkFramebuffer> framebuffers;
        bool                  laidOut = false;           ///< every layer put in its resting layout once

        VkSampler             sampler = VK_NULL_HANDLE;
        VkShaderModule        vert    = VK_NULL_HANDLE;
        VkShaderModule        alphaFrag = VK_NULL_HANDLE;
        VkRenderPass          pass    = VK_NULL_HANDLE;
        VkDescriptorSetLayout textureLayout = VK_NULL_HANDLE;  ///< the alpha-keyed parts' texture
        VkPipelineLayout      layout  = VK_NULL_HANDLE;
        VkPipeline            pipeline = VK_NULL_HANDLE;       ///< solid
        VkPipeline            alphaPipeline = VK_NULL_HANDLE;  ///< alpha-keyed
        VkSampler             textureSampler = VK_NULL_HANDLE;
        memory::Buffer        vertices[kSlotsInFlight];  ///< per frame slot: the triangles drawn in it
        VkDescriptorPool      texturePools[kSlotsInFlight] = {}; ///< per frame slot: its parts' sets, reset each frame
    };
    State s;

    bool Check(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        Write(Error, "vulkan: shadows: %s failed: %s", what, ResultName(r));
        s.failed = true;
        return false;
    }

    bool CreateImage()
    {
        const loader::Device& d = *s.dapi;
        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = kFormat;
        info.extent        = VkExtent3D{ kFaceSize, kFaceSize, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = s.slots * 6;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(d.CreateImage(s.device, &info, nullptr, &s.image), "vkCreateImage")) return false;
        VkMemoryRequirements req = {};
        d.GetImageMemoryRequirements(s.device, s.image, &req);
        if (!memory::Allocate(req, memory::Where::DeviceLocal, s.block))
        {
            Write(Error, "vulkan: shadows: no device memory for %u faces of %u x %u", s.slots * 6, kFaceSize, kFaceSize);
            s.failed = true;
            return false;
        }
        if (!Check(d.BindImageMemory(s.device, s.image, s.block.memory, s.block.offset), "vkBindImageMemory")) return false;

        VkImageViewCreateInfo view = {};
        view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image                       = s.image;
        view.viewType                    = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format                      = kFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = s.slots * 6;
        if (!Check(d.CreateImageView(s.device, &view, nullptr, &s.view), "vkCreateImageView")) return false;

        s.layerViews.assign(s.slots * 6, VK_NULL_HANDLE);
        s.framebuffers.assign(s.slots * 6, VK_NULL_HANDLE);
        for (uint32_t layer = 0; layer < s.slots * 6; ++layer)
        {
            view.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            view.subresourceRange.baseArrayLayer = layer;
            view.subresourceRange.layerCount     = 1;
            if (!Check(d.CreateImageView(s.device, &view, nullptr, &s.layerViews[layer]), "vkCreateImageView (face)")) return false;
            VkFramebufferCreateInfo fb = {};
            fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb.renderPass      = s.pass;
            fb.attachmentCount = 1;
            fb.pAttachments    = &s.layerViews[layer];
            fb.width           = kFaceSize;
            fb.height          = kFaceSize;
            fb.layers          = 1;
            if (!Check(d.CreateFramebuffer(s.device, &fb, nullptr, &s.framebuffers[layer]), "vkCreateFramebuffer")) return false;
        }
        return true;
    }

    /// Depth only: a face is cleared, drawn and left for the pixel shaders to read; before it, the
    /// last frames' reads of it are done.
    bool CreatePass()
    {
        VkAttachmentDescription a = {};
        a.format         = kFormat;
        a.samples        = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.pDepthStencilAttachment = &ref;
        VkSubpassDependency dep[2] = {};
        dep[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass    = 0;
        dep[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep[0].srcAccessMask = 0;
        dep[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass    = 0;
        dep[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo info = {};
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments    = &a;
        info.subpassCount    = 1;
        info.pSubpasses      = &sub;
        info.dependencyCount = 2;
        info.pDependencies   = dep;
        return Check(s.dapi->CreateRenderPass(s.device, &info, nullptr, &s.pass), "vkCreateRenderPass");
    }

    bool CreatePipeline()
    {
        const loader::Device& d = *s.dapi;
        VkShaderModuleCreateInfo module = {};
        module.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module.codeSize = sizeof kLightShadowVert;
        module.pCode    = kLightShadowVert;
        if (!Check(d.CreateShaderModule(s.device, &module, nullptr, &s.vert), "vkCreateShaderModule (LightShadow.vert)")) return false;

        module.codeSize = sizeof kLightShadowAlphaFrag;
        module.pCode    = kLightShadowAlphaFrag;
        if (!Check(d.CreateShaderModule(s.device, &module, nullptr, &s.alphaFrag), "vkCreateShaderModule (LightShadowAlpha.frag)")) return false;

        VkDescriptorSetLayoutBinding texture = {};
        texture.binding         = 0;
        texture.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texture.descriptorCount = 1;
        texture.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo setLayout = {};
        setLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayout.bindingCount = 1;
        setLayout.pBindings    = &texture;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &setLayout, nullptr, &s.textureLayout), "vkCreateDescriptorSetLayout")) return false;

        VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo layout = {};
        layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount         = 1;
        layout.pSetLayouts            = &s.textureLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges    = &push;
        if (!Check(d.CreatePipelineLayout(s.device, &layout, nullptr, &s.layout), "vkCreatePipelineLayout")) return false;

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = s.vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = s.alphaFrag;
        stages[1].pName  = "main";
        VkVertexInputBindingDescription binding = { 0, static_cast<uint32_t>(kVertexBytes), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attributes[2] = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32_SFLOAT, 12 } };
        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount   = 1;
        vertexInput.pVertexBindingDescriptions      = &binding;
        vertexInput.vertexAttributeDescriptionCount = 2;
        vertexInput.pVertexAttributeDescriptions    = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly = {};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport = { 0.0f, 0.0f, static_cast<float>(kFaceSize), static_cast<float>(kFaceSize), 0.0f, 1.0f };
        VkRect2D   scissor  = { { 0, 0 }, { kFaceSize, kFaceSize } };
        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports    = &viewport;
        viewportState.scissorCount  = 1;
        viewportState.pScissors     = &scissor;
        // Both sides (WMO walls are often single faces); a little slope bias against acne on the
        // surfaces the map sees (the pixel side also pushes its point out along the normal).
        VkPipelineRasterizationStateCreateInfo raster = {};
        raster.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode             = VK_POLYGON_MODE_FILL;
        raster.cullMode                = VK_CULL_MODE_NONE;
        raster.frontFace               = VK_FRONT_FACE_CLOCKWISE;
        raster.depthBiasEnable         = VK_TRUE;
        raster.depthBiasConstantFactor = 2.0f;
        raster.depthBiasSlopeFactor    = 2.0f;
        raster.lineWidth               = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth = {};
        depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable  = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkGraphicsPipelineCreateInfo pipeline = {};
        pipeline.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline.stageCount          = 1;
        pipeline.pStages             = stages;
        pipeline.pVertexInputState   = &vertexInput;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState      = &viewportState;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState   = &multisample;
        pipeline.pDepthStencilState  = &depth;
        pipeline.layout              = s.layout;
        pipeline.renderPass          = s.pass;
        if (!Check(d.CreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s.pipeline), "vkCreateGraphicsPipelines (shadows)")) return false;
        pipeline.stageCount = 2;
        return Check(d.CreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s.alphaPipeline), "vkCreateGraphicsPipelines (shadows, alpha key)");
    }

    /// Once: every layer from UNDEFINED to the layout the lights' set names (never-drawn slots are
    /// never read: no light points at them).
    void LayOut(VkCommandBuffer cmd)
    {
        VkImageMemoryBarrier b = {};
        b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask               = 0;
        b.dstAccessMask               = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout                   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.image                       = s.image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = s.slots * 6;
        s.dapi->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        s.laidOut = true;
    }
}

namespace gxvk::vk::shadows
{
    bool Ready() { return s.ready; }
    uint32_t Slots() { return s.slots; }
    VkImageView View() { return s.view; }
    VkSampler Sampler() { return s.sampler; }

    bool Init()
    {
        if (s.ready) return true;
        if (s.failed) return false;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi || !h->iapi) return false;
        s.device = h->device;
        s.dapi   = h->dapi;
        const loader::Device& d = *s.dapi;

        VkPhysicalDeviceProperties props = {};
        h->iapi->GetPhysicalDeviceProperties(h->physical, &props);
        s.slots = props.limits.maxImageArrayLayers / 6;
        if (s.slots > kMaxSlots) s.slots = kMaxSlots;
        if (s.slots == 0)
        {
            Write(Error, "vulkan: shadows: image arrays too short (%u layers)", props.limits.maxImageArrayLayers);
            s.failed = true;
            return false;
        }

        VkSamplerCreateInfo sampler = {};
        sampler.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter     = VK_FILTER_LINEAR;
        sampler.minFilter     = VK_FILTER_LINEAR;
        sampler.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.compareEnable = VK_TRUE;
        sampler.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
        sampler.maxLod        = 0.0f;
        if (!Check(d.CreateSampler(s.device, &sampler, nullptr, &s.sampler), "vkCreateSampler")) return false;

        if (!CreatePass() || !CreateImage() || !CreatePipeline()) return false;

        VkSamplerCreateInfo repeat = {};
        repeat.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        repeat.magFilter    = VK_FILTER_NEAREST;
        repeat.minFilter    = VK_FILTER_NEAREST;
        repeat.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        repeat.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        repeat.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        repeat.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        repeat.maxLod       = 0.0f;
        if (!Check(d.CreateSampler(s.device, &repeat, nullptr, &s.textureSampler), "vkCreateSampler (textures)")) return false;

        for (VkDescriptorPool& pool : s.texturePools)
        {
            VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTexturedParts };
            VkDescriptorPoolCreateInfo info = {};
            info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            info.maxSets       = kMaxTexturedParts;
            info.poolSizeCount = 1;
            info.pPoolSizes    = &size;
            if (!Check(d.CreateDescriptorPool(s.device, &info, nullptr, &pool), "vkCreateDescriptorPool")) return false;
        }

        for (memory::Buffer& b : s.vertices)
        {
            if (!memory::CreateBuffer(kMaxVertices * kVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory::Where::Stream, b) || !b.block.mapped)
            {
                Write(Error, "vulkan: shadows: no buffer for the triangles");
                s.failed = true;
                return false;
            }
        }

        s.ready = true;
        Write(Info, "vulkan: shadows: ready (%u lights, faces of %u x %u)", s.slots, kFaceSize, kFaceSize);
        return true;
    }

    uint32_t Record(const Job* jobs, uint32_t count)
    {
        if (!s.ready) return 0;
        const bool first = !s.laidOut;
        if (!first && count == 0) return 0;
        VkCommandBuffer cmd = frame::PrePass();
        if (!cmd) return 0;
        timings::PreMark(timings::Pre::ShadowMaps);   // SXC: the profiler's GPU times
        if (first) LayOut(cmd);

        const loader::Device& d = *s.dapi;
        memory::Buffer& buffer = s.vertices[frame::Slot()];
        uint8_t* out = static_cast<uint8_t*>(buffer.block.mapped);
        uint32_t used = 0, drawn = 0, textured = 0;
        const VkDescriptorPool pool = s.texturePools[frame::Slot()];
        d.ResetDescriptorPool(s.device, pool, 0);   // this slot's last frame has completed
        std::vector<VkDescriptorSet> sets;

        for (; drawn < count; ++drawn)
        {
            const Job& job = jobs[drawn];
            if (job.slot >= s.slots || job.radius <= kNear) continue;
            if (used + job.vertexCount > kMaxVertices) break;
            const uint32_t firstVertex = used;
            if (job.vertexCount)
            {
                std::memcpy(out + static_cast<size_t>(used) * kVertexBytes, job.vertices, static_cast<size_t>(job.vertexCount) * kVertexBytes);
                used += job.vertexCount;
            }

            // A job without parts is solid; an alpha-keyed part gets a set for its texture (past the
            // frame's sets, it casts nothing this time).
            const Part whole = { 0, job.vertexCount, VK_NULL_HANDLE };
            const Part* parts = job.partCount ? job.parts : &whole;
            const uint32_t partCount = job.partCount ? job.partCount : 1;
            sets.assign(partCount, VK_NULL_HANDLE);
            for (uint32_t p = 0; p < partCount; ++p)
            {
                if (parts[p].texture == VK_NULL_HANDLE || textured >= kMaxTexturedParts) continue;
                VkDescriptorSetAllocateInfo alloc = {};
                alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                alloc.descriptorPool     = pool;
                alloc.descriptorSetCount = 1;
                alloc.pSetLayouts        = &s.textureLayout;
                if (d.AllocateDescriptorSets(s.device, &alloc, &sets[p]) != VK_SUCCESS) continue;
                ++textured;
                VkDescriptorImageInfo image = { s.textureSampler, parts[p].texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                VkWriteDescriptorSet write = {};
                write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet          = sets[p];
                write.descriptorCount = 1;
                write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo      = &image;
                d.UpdateDescriptorSets(s.device, 1, &write, 0, nullptr);
            }

            // depth = A + B / distance along the face's axis: 0 at the near plane, 1 at the radius.
            const float reach = job.radius;   // the far plane (Windows' `far` is a macro)
            const float face[2] = { reach / (reach - kNear), -reach * kNear / (reach - kNear) };
            for (uint32_t f = 0; f < 6; ++f)
            {
                VkClearValue clear = {};
                clear.depthStencil.depth = 1.0f;
                VkRenderPassBeginInfo begin = {};
                begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                begin.renderPass        = s.pass;
                begin.framebuffer       = s.framebuffers[job.slot * 6 + f];
                begin.renderArea.extent = VkExtent2D{ kFaceSize, kFaceSize };
                begin.clearValueCount   = 1;
                begin.pClearValues      = &clear;
                d.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
                if (job.vertexCount)
                {
                    const float push[4] = { static_cast<float>(f / 2), (f & 1) ? -1.0f : 1.0f, face[0], face[1] };
                    const VkDeviceSize offset = 0;
                    d.CmdBindVertexBuffers(cmd, 0, 1, &buffer.handle, &offset);
                    VkPipeline bound = VK_NULL_HANDLE;
                    for (uint32_t p = 0; p < partCount; ++p)
                    {
                        const Part& part = parts[p];
                        if (!part.vertexCount || part.firstVertex + part.vertexCount > job.vertexCount) continue;
                        if (part.texture != VK_NULL_HANDLE && sets[p] == VK_NULL_HANDLE) continue;
                        const VkPipeline want = part.texture != VK_NULL_HANDLE ? s.alphaPipeline : s.pipeline;
                        if (want != bound)
                        {
                            d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
                            d.CmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
                            bound = want;
                        }
                        if (part.texture != VK_NULL_HANDLE)
                            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &sets[p], 0, nullptr);
                        d.CmdDraw(cmd, part.vertexCount, 1, firstVertex + part.firstVertex, 0);
                    }
                }
                d.CmdEndRenderPass(cmd);
            }
        }

        if (used) memory::FlushHost(buffer.block);
        timings::PreMark(timings::Pre::Uploads);
        return drawn;
    }

    void Shutdown()
    {
        if (!s.dapi || !s.device)
        {
            s = State();
            return;
        }
        const loader::Device& d = *s.dapi;
        for (memory::Buffer& b : s.vertices) memory::DestroyBuffer(b);
        for (VkFramebuffer fb : s.framebuffers)
            if (fb != VK_NULL_HANDLE) d.DestroyFramebuffer(s.device, fb, nullptr);
        for (VkImageView v : s.layerViews)
            if (v != VK_NULL_HANDLE) d.DestroyImageView(s.device, v, nullptr);
        if (s.view != VK_NULL_HANDLE) d.DestroyImageView(s.device, s.view, nullptr);
        if (s.image != VK_NULL_HANDLE) d.DestroyImage(s.device, s.image, nullptr);
        memory::Free(s.block);
        if (s.pipeline != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.pipeline, nullptr);
        if (s.alphaPipeline != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.alphaPipeline, nullptr);
        for (VkDescriptorPool pool : s.texturePools)
            if (pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, pool, nullptr);
        if (s.textureLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.textureLayout, nullptr);
        if (s.alphaFrag != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.alphaFrag, nullptr);
        if (s.textureSampler != VK_NULL_HANDLE) d.DestroySampler(s.device, s.textureSampler, nullptr);
        if (s.layout != VK_NULL_HANDLE) d.DestroyPipelineLayout(s.device, s.layout, nullptr);
        if (s.pass != VK_NULL_HANDLE) d.DestroyRenderPass(s.device, s.pass, nullptr);
        if (s.vert != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.vert, nullptr);
        if (s.sampler != VK_NULL_HANDLE) d.DestroySampler(s.device, s.sampler, nullptr);
        s = State();
    }
}
