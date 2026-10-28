// SXC: our water's device side (Vulkan/Water.hpp): the scene behind the water.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Water.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"

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

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    constexpr uint32_t kSlots = frame::kFramesInFlight;
    constexpr VkFormat kColourFormat = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkFormat kDepthFormat  = VK_FORMAT_R32_SFLOAT;

    /// Water.frag's WaterScene block (std140): the viewport on the target (x, y, width, height), the
    /// target's size (width, height, 1 / width, 1 / height), the wave cascades' sizes (yd; w 1 when the
    /// waves are on), the waves' look (how far cascades 1 and 2 reach, the foam's strength, 0).
    struct Params
    {
        float viewport[4];
        float extent[4];
        float waveSizes[4];
        float waveLook[4];
        float depthProj[4];    ///< the capture's projection terms (P22, P32, P23, P33), for the debug view's self-test
        float depthRange[4];   ///< and its depth range (min, max); w: how many sky bodies follow
        float bodyDir[4][4];   ///< the sky's bodies: direction toward each (unit), its angular radius (rad)
        float bodyColour[4][4];
    };

    // The waves (Shaders/WaterWaves.glsl): N x N per cascade, three cascades.
    constexpr uint32_t kWaveN = 256;
    constexpr uint32_t kCascades = 3;
    constexpr uint32_t kWaveImages = 7;   // h0, spectrum A and B, rows A and B, displacement, slopes
    constexpr float    kWaveBounds[2] = { 6.2831853f / 8.0f, 6.2831853f / 1.5f };  // cascades 0 / 1 at 8 yd, 1 / 2 at 1.5 yd
    constexpr float    kGravity = 10.73f;  // yd / s^2

    /// WaterWaves.glsl's WavePush.
    struct WavePush
    {
        float sizes[4];
        float bounds[4];
        float wind[4];
        float misc[4];
    };

    /// WaterCapture.comp's push constants.
    struct Push
    {
        float   depthRange[4];
        float   proj[4];
        int32_t size[4];
    };

    struct Image
    {
        VkImage       image  = VK_NULL_HANDLE;
        VkImageView   view   = VK_NULL_HANDLE;
        memory::Block block;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    // Per frame slot: rewritten only when the slot comes round again, its previous frame complete.
    struct Slot
    {
        VkDescriptorSet captureSet = VK_NULL_HANDLE; ///< the copy's
        VkDescriptorSet drawSet = VK_NULL_HANDLE;   ///< set 2 of the water draws
        memory::Buffer  params;
        uint64_t        captured = 0;               ///< the serial whose scene the images hold
        bool            written  = false;           ///< drawSet points at the current images
    };

    struct State
    {
        bool                  ready  = false;
        bool                  failed = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;

        VkShaderModule        comp          = VK_NULL_HANDLE;
        VkDescriptorSetLayout captureLayout = VK_NULL_HANDLE;   ///< the copy's set
        VkPipelineLayout      pipeLayout    = VK_NULL_HANDLE;
        VkPipeline            pipeline      = VK_NULL_HANDLE;
        VkDescriptorSetLayout drawLayout    = VK_NULL_HANDLE;   ///< set 2 of the water draws
        VkDescriptorPool      pool          = VK_NULL_HANDLE;
        VkSampler            linearClamp   = VK_NULL_HANDLE, nearestClamp = VK_NULL_HANDLE;
        Slot                  slots[kSlots];

        // One pair for every frame: the queue runs the frames in order, and each copy waits for the
        // previous frames' reads (its barrier). Remade when the target's size changes.
        Image      colour, depth;
        VkExtent2D extent = { 0, 0 };
        uint64_t   lastUsed = 0;

        uint64_t lastCapture = 0;    ///< the serial of the last attempt
        bool     lastOk      = false;

        // The waves: made at the first frame that wants them; `wavesTried` once.
        bool                  wavesTried  = false;
        bool                  wavesReady  = false;
        VkShaderModule        waveModules[5] = {};      ///< spectrum init, spectrum, rows, columns, derive
        VkPipeline            wavePipes[5]   = {};
        VkDescriptorSetLayout waveLayout  = VK_NULL_HANDLE;
        VkPipelineLayout      wavePipeLayout = VK_NULL_HANDLE;
        VkDescriptorPool      wavePool    = VK_NULL_HANDLE;
        VkDescriptorSet       waveSet     = VK_NULL_HANDLE;   ///< never rewritten: the images stay
        VkSampler             linearRepeat = VK_NULL_HANDLE;
        Image                 waveImages[kWaveImages];
        bool                  wavesFresh  = true;             ///< the images still undefined
        bool                  wavesRan    = false;            ///< the slopes hold waves
        float                 spectrumKey[5] = { -1.0f, 0.0f, 0.0f, 0.0f, 0.0f };   ///< the wind the h0 was made for

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
        Write(Error, "vulkan: water: %s failed: %s", what, ResultName(r));
        s.failed = true;
        return false;
    }

    VkSampler MakeSampler(VkFilter filter)
    {
        VkSamplerCreateInfo info = {};
        info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter    = filter;
        info.minFilter    = filter;
        info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSampler sampler = VK_NULL_HANDLE;
        Check(s.dapi->CreateSampler(s.device, &info, nullptr, &sampler), "vkCreateSampler");
        return sampler;
    }

    void DestroyImage(Image& img)
    {
        if (img.view != VK_NULL_HANDLE) s.dapi->DestroyImageView(s.device, img.view, nullptr);
        if (img.image != VK_NULL_HANDLE) s.dapi->DestroyImage(s.device, img.image, nullptr);
        memory::Free(img.block);
        img = Image();
    }

    bool CreateImage(VkFormat format, VkExtent2D extent, Image& out, uint32_t layers = 1)
    {
        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = format;
        info.extent        = VkExtent3D{ extent.width, extent.height, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = layers;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(s.dapi->CreateImage(s.device, &info, nullptr, &out.image), "vkCreateImage")) return false;
        VkMemoryRequirements req = {};
        s.dapi->GetImageMemoryRequirements(s.device, out.image, &req);
        if (!memory::Allocate(req, memory::Where::DeviceLocal, out.block))
        {
            Write(Error, "vulkan: water: no device memory for a %ux%u image", extent.width, extent.height);
            s.dapi->DestroyImage(s.device, out.image, nullptr);
            out = Image();
            s.failed = true;
            return false;
        }
        VkImageViewCreateInfo view = {};
        view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image                       = out.image;
        view.viewType                    = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        view.format                      = format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = layers;
        if (!Check(s.dapi->BindImageMemory(s.device, out.image, out.block.memory, out.block.offset), "vkBindImageMemory")
            || !Check(s.dapi->CreateImageView(s.device, &view, nullptr, &out.view), "vkCreateImageView"))
        {
            DestroyImage(out);
            return false;
        }
        return true;
    }

    void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspects, VkImageLayout from, VkImageLayout to,
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
        b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        s.dapi->CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    /// Everything the compute stage wrote before, visible to what comes after (a whole-memory barrier).
    void GlobalBarrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
    {
        VkMemoryBarrier b = {};
        b.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        s.dapi->CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &b, 0, nullptr, 0, nullptr);
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

        VkShaderModuleCreateInfo module = {};
        module.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module.codeSize = sizeof kWaterCaptureComp;
        module.pCode    = kWaterCaptureComp;
        if (!Check(d.CreateShaderModule(s.device, &module, nullptr, &s.comp), "vkCreateShaderModule (WaterCapture.comp)")) return false;

        // The copy: 0 the target's colour, 1 its depth, 2 and 3 our images to write.
        VkDescriptorSetLayoutBinding capture[4] = {};
        for (uint32_t i = 0; i < 4; ++i)
        {
            capture[i].binding         = i;
            capture[i].descriptorType  = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            capture[i].descriptorCount = 1;
            capture[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo captureInfo = {};
        captureInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        captureInfo.bindingCount = 4;
        captureInfo.pBindings    = capture;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &captureInfo, nullptr, &s.captureLayout), "vkCreateDescriptorSetLayout (capture)")) return false;

        // Set 2 of the draws: 0 the scene's colour, 1 its depth (w), 2 where the draws lie on it and the
        // waves' parameters, 3 the waves' slopes (a layer per cascade), 4 their displacement (the vertex
        // stage moves the surface with it, step 3b).
        VkDescriptorSetLayoutBinding draw[5] = {};
        for (uint32_t i = 0; i < 5; ++i)
        {
            draw[i].binding         = i;
            draw[i].descriptorType  = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            draw[i].descriptorCount = 1;
            draw[i].stageFlags      = i == 2 ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                                    : i == 4 ? VK_SHADER_STAGE_VERTEX_BIT
                                             : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo drawInfo = {};
        drawInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        drawInfo.bindingCount = 5;
        drawInfo.pBindings    = draw;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &drawInfo, nullptr, &s.drawLayout), "vkCreateDescriptorSetLayout (set 2)")) return false;

        VkPushConstantRange push = {};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size       = sizeof(Push);
        VkPipelineLayoutCreateInfo layout = {};
        layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount         = 1;
        layout.pSetLayouts            = &s.captureLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges    = &push;
        if (!Check(d.CreatePipelineLayout(s.device, &layout, nullptr, &s.pipeLayout), "vkCreatePipelineLayout")) return false;

        VkComputePipelineCreateInfo compute = {};
        compute.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compute.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = s.comp;
        compute.stage.pName  = "main";
        compute.layout       = s.pipeLayout;
        if (!Check(d.CreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &compute, nullptr, &s.pipeline), "vkCreateComputePipelines")) return false;

        VkDescriptorPoolSize sizes[3] = {};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = 6 * kSlots;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sizes[1].descriptorCount = 2 * kSlots;
        sizes[2].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[2].descriptorCount = kSlots;
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = 2 * kSlots;
        pool.poolSizeCount = 3;
        pool.pPoolSizes    = sizes;
        if (!Check(d.CreateDescriptorPool(s.device, &pool, nullptr, &s.pool), "vkCreateDescriptorPool")) return false;

        VkDescriptorSetAllocateInfo alloc = {};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = s.pool;
        alloc.descriptorSetCount = 1;
        for (Slot& slot : s.slots)
        {
            alloc.pSetLayouts = &s.captureLayout;
            if (!Check(d.AllocateDescriptorSets(s.device, &alloc, &slot.captureSet), "vkAllocateDescriptorSets (capture)")) return false;
            alloc.pSetLayouts = &s.drawLayout;
            if (!Check(d.AllocateDescriptorSets(s.device, &alloc, &slot.drawSet), "vkAllocateDescriptorSets (set 2)")) return false;
            if (!memory::CreateBuffer(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, slot.params) || !slot.params.block.mapped)
            {
                Write(Error, "vulkan: water: no buffer for the parameters");
                s.failed = true;
                return false;
            }
        }

        s.linearClamp  = MakeSampler(VK_FILTER_LINEAR);
        s.nearestClamp = MakeSampler(VK_FILTER_NEAREST);
        VkSamplerCreateInfo repeat = {};
        repeat.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        repeat.magFilter    = VK_FILTER_LINEAR;
        repeat.minFilter    = VK_FILTER_LINEAR;
        repeat.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        repeat.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        repeat.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        repeat.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Check(d.CreateSampler(s.device, &repeat, nullptr, &s.linearRepeat), "vkCreateSampler (waves)");

        // The waves' slopes are bound whether the waves run or not (zeroed then: a calm surface), so
        // the images are made here; the compute side comes with the first frame that wants waves.
        const VkExtent2D waveExtent = { kWaveN, kWaveN };
        const VkFormat waveFormats[kWaveImages] = { VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                    VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                    VK_FORMAT_R16G16B16A16_SFLOAT };
        for (uint32_t i = 0; i < kWaveImages && !s.failed; ++i) CreateImage(waveFormats[i], waveExtent, s.waveImages[i], kCascades);
        if (s.failed) return false;

        s.ready = true;
        Write(Debug, "vulkan: water: ready");
        return true;
    }

    /// The pair at the target's size, made or remade (the previous pair's last reads waited for).
    bool EnsureImages(VkExtent2D extent)
    {
        if (s.colour.image != VK_NULL_HANDLE && s.extent.width == extent.width && s.extent.height == extent.height) return true;
        if (s.colour.image != VK_NULL_HANDLE || s.depth.image != VK_NULL_HANDLE)
        {
            if (s.lastUsed) frame::WaitSerial(s.lastUsed);
            DestroyImage(s.colour);
            DestroyImage(s.depth);
        }
        for (Slot& slot : s.slots) slot.written = false;
        s.extent = extent;
        return CreateImage(kColourFormat, extent, s.colour) && CreateImage(kDepthFormat, extent, s.depth);
    }

    void WriteDrawSet(Slot& slot)
    {
        VkDescriptorImageInfo images[5] = {
            { s.linearClamp, s.colour.view, VK_IMAGE_LAYOUT_GENERAL },
            { s.nearestClamp, s.depth.view, VK_IMAGE_LAYOUT_GENERAL },
            {},
            { s.linearRepeat, s.waveImages[6].view, VK_IMAGE_LAYOUT_GENERAL },
            { s.linearRepeat, s.waveImages[5].view, VK_IMAGE_LAYOUT_GENERAL },
        };
        VkDescriptorBufferInfo params = { slot.params.handle, 0, sizeof(Params) };
        VkWriteDescriptorSet writes[5] = {};
        for (uint32_t i = 0; i < 5; ++i)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = slot.drawSet;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (i == 2) writes[i].pBufferInfo = &params;
            else writes[i].pImageInfo = &images[i];
        }
        s.dapi->UpdateDescriptorSets(s.device, 5, writes, 0, nullptr);
        slot.written = true;
    }

    /// The waves' compute side, once: the device must run 256 threads a group with the FFT's shared memory.
    bool InitWaves()
    {
        if (s.wavesReady) return true;
        if (s.wavesTried) return false;
        s.wavesTried = true;

        const context::Handles* h = context::Get();
        VkPhysicalDeviceProperties props = {};
        h->iapi->GetPhysicalDeviceProperties(h->physical, &props);
        const VkPhysicalDeviceLimits& lim = props.limits;
        if (lim.maxComputeWorkGroupInvocations < kWaveN || lim.maxComputeWorkGroupSize[0] < kWaveN || lim.maxComputeSharedMemorySize < 3 * 2 * kWaveN * 8)
        {
            Write(gxvk::log::Warn, "vulkan: water: the device can't run the FFT's %u-thread groups; the water keeps its ripples", kWaveN);
            return false;
        }

        const loader::Device& d = *s.dapi;
        const struct { const uint32_t* words; size_t bytes; const char* what; } modules[5] = {
            { kWaterSpectrumInitComp, sizeof kWaterSpectrumInitComp, "WaterSpectrum.comp (init)" },
            { kWaterSpectrumComp, sizeof kWaterSpectrumComp, "WaterSpectrum.comp" },
            { kWaterFftRowsComp, sizeof kWaterFftRowsComp, "WaterFft.comp (rows)" },
            { kWaterFftColumnsComp, sizeof kWaterFftColumnsComp, "WaterFft.comp (columns)" },
            { kWaterDeriveComp, sizeof kWaterDeriveComp, "WaterDerive.comp" },
        };
        for (uint32_t i = 0; i < 5; ++i)
        {
            VkShaderModuleCreateInfo module = {};
            module.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            module.codeSize = modules[i].bytes;
            module.pCode    = modules[i].words;
            if (!Check(d.CreateShaderModule(s.device, &module, nullptr, &s.waveModules[i]), modules[i].what)) return false;
        }

        VkDescriptorSetLayoutBinding bindings[kWaveImages] = {};
        for (uint32_t i = 0; i < kWaveImages; ++i)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setInfo = {};
        setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = kWaveImages;
        setInfo.pBindings    = bindings;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &setInfo, nullptr, &s.waveLayout), "vkCreateDescriptorSetLayout (waves)")) return false;

        VkPushConstantRange push = {};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size       = sizeof(WavePush);
        VkPipelineLayoutCreateInfo layout = {};
        layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount         = 1;
        layout.pSetLayouts            = &s.waveLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges    = &push;
        if (!Check(d.CreatePipelineLayout(s.device, &layout, nullptr, &s.wavePipeLayout), "vkCreatePipelineLayout (waves)")) return false;

        for (uint32_t i = 0; i < 5; ++i)
        {
            VkComputePipelineCreateInfo compute = {};
            compute.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            compute.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            compute.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            compute.stage.module = s.waveModules[i];
            compute.stage.pName  = "main";
            compute.layout       = s.wavePipeLayout;
            if (!Check(d.CreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &compute, nullptr, &s.wavePipes[i]), modules[i].what)) return false;
        }

        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kWaveImages };
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes    = &size;
        if (!Check(d.CreateDescriptorPool(s.device, &pool, nullptr, &s.wavePool), "vkCreateDescriptorPool (waves)")) return false;
        VkDescriptorSetAllocateInfo alloc = {};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = s.wavePool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &s.waveLayout;
        if (!Check(d.AllocateDescriptorSets(s.device, &alloc, &s.waveSet), "vkAllocateDescriptorSets (waves)")) return false;

        VkDescriptorImageInfo infos[kWaveImages] = {};
        VkWriteDescriptorSet writes[kWaveImages] = {};
        for (uint32_t i = 0; i < kWaveImages; ++i)
        {
            infos[i] = { VK_NULL_HANDLE, s.waveImages[i].view, VK_IMAGE_LAYOUT_GENERAL };
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = s.waveSet;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo      = &infos[i];
        }
        d.UpdateDescriptorSets(s.device, kWaveImages, writes, 0, nullptr);

        s.wavesReady = true;
        Write(Debug, "vulkan: water: waves ready (%u cascades of %ux%u)", kCascades, kWaveN, kWaveN);
        return true;
    }

    /// The images out of UNDEFINED into GENERAL, zeroed (a calm surface until waves run), once.
    void ReadyWaveImages(VkCommandBuffer cmd)
    {
        if (!s.wavesFresh) return;
        const loader::Device& d = *s.dapi;
        VkClearColorValue zero = {};
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kCascades };
        for (Image& img : s.waveImages)
        {
            ImageBarrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            d.CmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            img.layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        GlobalBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        s.wavesFresh = false;
    }

    /// This frame's waves into the pre-pass (ahead of every draw of the frame): the spectrum again if the
    /// wind changed, then the spectrum at the time, the FFT's two passes, the slopes.
    void RecordWaves(const gxvk::vk::water::WaveInput& w)
    {
        VkCommandBuffer cmd = frame::PrePass();
        if (!cmd) return;
        ReadyWaveImages(cmd);
        if (!w.on || !InitWaves()) return;

        const loader::Device& d = *s.dapi;
        WavePush p = {};
        for (uint32_t c = 0; c < kCascades; ++c) p.sizes[c] = gxvk::vk::water::kWaveSizes[c];
        p.sizes[3]  = static_cast<float>(kWaveN);
        p.bounds[0] = kWaveBounds[0];
        p.bounds[1] = kWaveBounds[1];
        p.bounds[2] = kGravity;
        p.bounds[3] = w.period;
        p.wind[0]   = w.windX;
        p.wind[1]   = w.windY;
        p.wind[2]   = w.speed;
        p.wind[3]   = w.spread;
        p.misc[0]   = w.amplitude;
        p.misc[1]   = w.chop;
        p.misc[2]   = w.time;

        // The last frame's water has read the slopes before this frame writes anything.
        GlobalBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.wavePipeLayout, 0, 1, &s.waveSet, 0, nullptr);
        d.CmdPushConstants(cmd, s.wavePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        const auto step = [&](uint32_t pipe, uint32_t x, uint32_t y) {
            d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.wavePipes[pipe]);
            d.CmdDispatch(cmd, x, y, kCascades);
            GlobalBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        };

        // The spectrum again when the wind (or the look) moved enough to see.
        const float key[5] = { w.speed, w.windX, w.windY, w.spread, w.amplitude };
        bool changed = w.rebuild;
        for (uint32_t i = 0; i < 5; ++i) changed |= std::fabs(key[i] - s.spectrumKey[i]) > (i == 0 ? 0.2f : 0.02f);
        if (changed)
        {
            step(0, kWaveN / 8, kWaveN / 8);
            std::memcpy(s.spectrumKey, key, sizeof key);
        }

        step(1, kWaveN / 8, kWaveN / 8);
        step(2, 1, kWaveN);
        step(3, 1, kWaveN);
        step(4, kWaveN / 8, kWaveN / 8);
        GlobalBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT);
        s.wavesRan = true;
    }
}

namespace gxvk::vk::water
{
    bool Capture(const CaptureInput& in)
    {
        const uint64_t serial = frame::Serial();

        if (s.lastCapture == serial) return s.lastOk;
        s.lastCapture = serial;
        s.lastOk      = false;

        if (!Init())
        {
            SetStatus("off: the copy could not be made");
            return false;
        }

        // The waves first, into the pre-pass (they don't need the pass ended).
        RecordWaves(in.waves);

        frame::EffectTarget t;
        if (!frame::BeginEffect(t))
        {
            SetStatus("skipped: nothing to copy yet");
            return false;
        }
        gxvk::vk::pipelines::ForgetBound();

        if (!EnsureImages(t.extent))
        {
            SetStatus("off: no memory for the scene's copy");
            return false;
        }

        const loader::Device& d   = *s.dapi;
        VkCommandBuffer       cmd = t.cmd;
        Slot&                 slot = s.slots[frame::Slot()];

        // The copy's set: this frame's target (its view can change with the target) and our pair.
        VkDescriptorImageInfo images[4] = {
            { s.nearestClamp, t.colour, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { s.nearestClamp, t.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
            { VK_NULL_HANDLE, s.colour.view, VK_IMAGE_LAYOUT_GENERAL },
            { VK_NULL_HANDLE, s.depth.view, VK_IMAGE_LAYOUT_GENERAL },
        };
        VkWriteDescriptorSet writes[4] = {};
        for (uint32_t i = 0; i < 4; ++i)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = slot.captureSet;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo      = &images[i];
        }
        d.UpdateDescriptorSets(s.device, 4, writes, 0, nullptr);

        if (!slot.written) WriteDrawSet(slot);

        Params p = {};
        p.viewport[0] = in.viewport.x;
        p.viewport[1] = in.viewport.y;
        p.viewport[2] = in.viewport.width;
        p.viewport[3] = in.viewport.height;
        p.extent[0]   = static_cast<float>(t.extent.width);
        p.extent[1]   = static_cast<float>(t.extent.height);
        p.extent[2]   = 1.0f / static_cast<float>(t.extent.width);
        p.extent[3]   = 1.0f / static_cast<float>(t.extent.height);
        for (uint32_t c = 0; c < kCascades; ++c) p.waveSizes[c] = gxvk::vk::water::kWaveSizes[c];
        p.waveSizes[3] = in.waves.on && s.wavesRan ? 1.0f : 0.0f;
        p.waveLook[0]  = 220.0f;   // cascade 1 fades out by this distance (yd), cascade 2 by the next
        p.waveLook[1]  = 45.0f;
        p.waveLook[2]  = in.waves.foam;
        p.waveLook[3]  = in.waves.move;
        p.depthProj[0] = in.proj[2 * 4 + 2];
        p.depthProj[1] = in.proj[3 * 4 + 2];
        p.depthProj[2] = in.proj[2 * 4 + 3];
        p.depthProj[3] = in.proj[3 * 4 + 3];
        p.depthRange[0] = in.viewport.minDepth;
        p.depthRange[1] = in.viewport.maxDepth;
        p.depthRange[3] = static_cast<float>(in.waves.bodies);
        for (uint32_t b = 0; b < in.waves.bodies && b < 4; ++b)
        {
            std::memcpy(p.bodyDir[b], in.waves.body[b], 4 * sizeof(float));
            std::memcpy(p.bodyColour[b], &in.waves.body[b][4], 3 * sizeof(float));
        }
        std::memcpy(slot.params.block.mapped, &p, sizeof p);
        memory::FlushHost(slot.params.block);

        // The world's colour writes done before we read the target; the earlier frames' water reads of
        // our pair done before we write it.
        VkMemoryBarrier colourDone = {};
        colourDone.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        colourDone.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        colourDone.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &colourDone, 0, nullptr, 0, nullptr);
        ImageBarrier(cmd, s.colour.image, VK_IMAGE_ASPECT_COLOR_BIT, s.colour.layout, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        ImageBarrier(cmd, s.depth.image, VK_IMAGE_ASPECT_COLOR_BIT, s.depth.layout, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        s.colour.layout = VK_IMAGE_LAYOUT_GENERAL;
        s.depth.layout  = VK_IMAGE_LAYOUT_GENERAL;
        ImageBarrier(cmd, t.depth, t.depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

        Push push = {};
        push.depthRange[0] = in.viewport.minDepth;
        push.depthRange[1] = in.viewport.maxDepth;
        push.proj[0] = in.proj[2 * 4 + 2];   // P22
        push.proj[1] = in.proj[3 * 4 + 2];   // P32
        push.proj[2] = in.proj[2 * 4 + 3];   // P23
        push.proj[3] = in.proj[3 * 4 + 3];   // P33
        push.size[0] = static_cast<int32_t>(t.extent.width);
        push.size[1] = static_cast<int32_t>(t.extent.height);

        d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
        d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeLayout, 0, 1, &slot.captureSet, 0, nullptr);
        d.CmdPushConstants(cmd, s.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        d.CmdDispatch(cmd, (t.extent.width + 7) / 8, (t.extent.height + 7) / 8, 1);

        // Our pair to the water's pixels; the depth back for the world's draws; the target's colour
        // read before the resumed pass writes it again.
        ImageBarrier(cmd, s.colour.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        ImageBarrier(cmd, s.depth.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        ImageBarrier(cmd, t.depth, t.depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);

        slot.captured = serial;
        s.lastUsed    = serial;
        s.lastOk      = true;
        char line[96];
        std::snprintf(line, sizeof line, "drawn: the scene copied (%ux%u)", t.extent.width, t.extent.height);
        SetStatus(line);
        return true;
    }

    VkDescriptorSetLayout SetLayout()
    {
        return Init() ? s.drawLayout : VK_NULL_HANDLE;
    }

    VkDescriptorSet FrameSet(uint32_t slotIndex, uint64_t serial)
    {
        if (!s.ready || slotIndex >= kSlots) return VK_NULL_HANDLE;
        const Slot& slot = s.slots[slotIndex];
        return slot.captured == serial && slot.written ? slot.drawSet : VK_NULL_HANDLE;
    }

    const char* Status() { return s.status; }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        DestroyImage(s.colour);
        DestroyImage(s.depth);
        for (Image& img : s.waveImages) DestroyImage(img);
        for (VkPipeline& p : s.wavePipes)
            if (p != VK_NULL_HANDLE) d.DestroyPipeline(s.device, p, nullptr);
        for (VkShaderModule& m : s.waveModules)
            if (m != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, m, nullptr);
        if (s.wavePipeLayout != VK_NULL_HANDLE) d.DestroyPipelineLayout(s.device, s.wavePipeLayout, nullptr);
        if (s.wavePool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.wavePool, nullptr);
        if (s.waveLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.waveLayout, nullptr);
        if (s.linearRepeat != VK_NULL_HANDLE) d.DestroySampler(s.device, s.linearRepeat, nullptr);
        for (Slot& slot : s.slots) memory::DestroyBuffer(slot.params);
        if (s.pipeline != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.pipeline, nullptr);
        if (s.pipeLayout != VK_NULL_HANDLE) d.DestroyPipelineLayout(s.device, s.pipeLayout, nullptr);
        if (s.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.captureLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.captureLayout, nullptr);
        if (s.drawLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.drawLayout, nullptr);
        if (s.linearClamp != VK_NULL_HANDLE) d.DestroySampler(s.device, s.linearClamp, nullptr);
        if (s.nearestClamp != VK_NULL_HANDLE) d.DestroySampler(s.device, s.nearestClamp, nullptr);
        if (s.comp != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.comp, nullptr);
        s = State();
    }
}
