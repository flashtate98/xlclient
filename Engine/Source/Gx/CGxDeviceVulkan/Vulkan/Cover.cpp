// SXC: surface covers in our terrain shader (Vulkan/Cover.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Cover.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/DeviceCaps.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Water.hpp"

#include "SXC/Settings/Settings.h"  // [Terrain] CoverSlots16

#include <cstdio>
#include <cstring>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;
    using gxvk::vk::cover::Frame;
    using gxvk::vk::cover::kBindlessSlots;
    using gxvk::vk::cover::kFallbackSlots;
    using gxvk::vk::cover::kMaxEntries;
    using gxvk::vk::cover::kMaxMap;
    using gxvk::vk::cover::kMaxTextures;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Info;
    using gxvk::log::Warn;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    constexpr uint32_t kSlots      = frame::kFramesInFlight;
    /// Bindings: 0 parameters, 1 near map, 2 far map, 3 and 4 the trails (fine, coarse), 5 the surface
    /// grid and 6 the terrain's depth per pixel (storage images the pixels write; the decals read 6),
    /// then the textures: one array at 7 (bindless), or one binding per slot from 7.
    constexpr uint32_t kSurfaceBinding = 5;
    constexpr uint32_t kDepthBinding = 6;
    constexpr uint32_t kFirstTex   = 7;
    constexpr uint32_t kMapSamplers = 9;                         ///< bindings 1..4, the marks (kMarksBinding), the shore field, cells, waves, ripples
    constexpr uint32_t kMaxSurface = gxvk::vk::cover::kMaxSurface;
    constexpr VkImageUsageFlags kSurfaceUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr uint32_t kEntryRows  = 5;                          ///< vec4 per entry (sxc_cover_entry, 80 bytes)
    constexpr uint32_t kMaxBindings = kFirstTex + kFallbackSlots + 5;   ///< + the marks (kMarksBinding), the shore field, cells, waves, ripples
    constexpr uint32_t kTerrainSamplers = 13;                    ///< set 0 of the terrain modules: 4 layers, the alpha map, 4 cascades, 4 height maps
    constexpr VkFormat kMapFormat  = VK_FORMAT_R32G32B32A32_SFLOAT;
    constexpr float    kNoWater    = -100000.0f;

    /// Shaders/Terrain.frag's CoverBlock (std140).
    struct Params
    {
        float enable[4];     ///< on, debug view, wet darkening, plain brightness
        float nearMap[4];    ///< cell (yd), size, first i, first j
        float farMap[4];
        float outside[4];    ///< fallen snow, kept share, moisture above rest, near map present
        float tuning[4];     ///< thin depth (yd), 1 / texture tile (1/yd), depth scale, far map present
        float tuning2[4];    ///< breakup scale, drift scale, fallen snow on, entries
        float tess[4];       ///< raised chunks: yd per segment per yd of distance, reach (yd), most segments per edge, on
        float trailFine[4];  ///< trails: cell (yd), size, first i, first j (size 0: none)
        float trailCoarse[4];
        float trail[4];      ///< now (s), persist (s), slur (yd per yd of depth), on
        float trail2[4];     ///< rim scale, 0, 0, 0
        float surface[4];    ///< the surface grid: cell (yd), size (0 none), first i, first j
        float shoreGrid[4];  ///< the water's shore field (Vulkan/Water.hpp's ShoreShared): first cell x, y, 1 when there, cell (yd)
        float shoreLook[4];  ///< foam, wave height (yd), wavelength (yd), swash run-up (yd)
        float shoreWind[4];  ///< the waves' direction (x, y), the wind speed (yd / s), the water's clock (s)
        float shoreWave[4];  ///< the long waves: movement x wave scale, 1 / cascade 0's size (1 / yd), 1 when they run, 0
        float shoreCamera[4]; ///< the camera's world position (the waves' movement fades with its distance)
        float shoreCaustic[4]; ///< the caustics on the floor: strength, yd per cell, yd they reach, 0
        float shoreSun[4];     ///< toward the sun (world, unit), how cloudy (0..1)
        float sunShadow[4];    ///< the cascades fade on ground turned from the sun: on, the fade's width (in N.L), 0, 0
        float texInfo[kMaxTextures / 4][4]; ///< per texture k (at [k / 4][k % 4]): its slot x 4 (0 none), + 2 gloss mask
        float entries[kMaxEntries * kEntryRows][4];
    };

    struct Image
    {
        VkImage       image  = VK_NULL_HANDLE;
        VkImageView   view   = VK_NULL_HANDLE;
        memory::Block block;
        uint32_t      size   = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct Slot
    {
        VkDescriptorSet set    = VK_NULL_HANDLE;
        memory::Buffer  params;
        uint64_t        serial = 0;   ///< the frame the set was written for
        uint32_t        written[kBindlessSlots] = {};  ///< the image IDs its texture bindings hold (0 the white one; never reused, unlike view handles)
        bool            texturesWritten = false;
        /// The surface grid as this slot's frames copied it (Readback memory), two in turn: the one handed
        /// to the client is read in place until the next hand-over, so the next copy goes to the other.
        memory::Buffer  surfaceRead[2];
        int             surfacePending = -1;       ///< 0 / 1: a copy was recorded into that buffer (handed over when the slot comes round again); 2 + k: buffer k was handed over; -1 none
        float           surfaceCell = 0.0f;        ///< that copy's cell size
    };

    struct State
    {
        bool                  ready  = false;
        bool                  failed = false;
        bool                  bindless = false;
        uint32_t              textureSlots = kFallbackSlots;
        bool                  tessellation = false;
        float                 maxTessLevel = 1.0f;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;
        gxvk::vk::cover::Provider provider = nullptr;

        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool      pool      = VK_NULL_HANDLE;
        VkSampler             mapSampler = VK_NULL_HANDLE, texSampler = VK_NULL_HANDLE;
        Slot                  slots[kSlots];

        Image    nearMap, farMap, white;
        uint32_t nearVersion = 0, farVersion = 0;
        bool     nearPresent = false, farPresent = false;
        Image    trailMaps[2];                ///< fine, coarse
        Image    marks;                       ///< the ground marks (the grass's vertex stage reads them)
        uint32_t marksVersion = 0;
        bool     marksPresent = false;
        bool     trailPresent[2] = {};
        Image    surface;                     ///< the surface grid (storage, GENERAL once cleared; 1 x 1 until sized)
        /// The terrain's depth per pixel of the target (R32_UINT storage, GENERAL; 1 x 1 until sized):
        /// cleared at each frame's first terrain draw, written by the terrain, read by the decals.
        VkImage        depth = VK_NULL_HANDLE;
        VkImageView    depthView = VK_NULL_HANDLE;
        memory::Block  depthBlock;
        VkExtent2D     depthExtent = { 0, 0 };
        VkImageLayout  depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        void   (*surfaceSink)(const float*, uint32_t, float) = nullptr; ///< the last one given (told "none" before buffers go)
        VkPipelineStageFlags readers = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; ///< the stages that read the maps (vertex: the grass, the marks)

        char status[200] = "no terrain drawn yet";
    };
    State s;

    bool Check(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        Write(Error, "vulkan: cover: %s failed: %s", what, ResultName(r));
        s.failed = true;
        return false;
    }

    void DestroyImage(Image& img)
    {
        if (img.view != VK_NULL_HANDLE) s.dapi->DestroyImageView(s.device, img.view, nullptr);
        if (img.image != VK_NULL_HANDLE) s.dapi->DestroyImage(s.device, img.image, nullptr);
        memory::Free(img.block);
        img = Image();
    }

    bool CreateImage(uint32_t size, VkFormat format, Image& out,
                     VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
    {
        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = format;
        info.extent        = VkExtent3D{ size, size, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = usage;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(s.dapi->CreateImage(s.device, &info, nullptr, &out.image), "vkCreateImage")) return false;
        VkMemoryRequirements req = {};
        s.dapi->GetImageMemoryRequirements(s.device, out.image, &req);
        if (!memory::Allocate(req, memory::Where::DeviceLocal, out.block))
        {
            Write(Error, "vulkan: cover: no device memory for a %ux%u map", size, size);
            s.dapi->DestroyImage(s.device, out.image, nullptr);
            out = Image();
            s.failed = true;
            return false;
        }
        VkImageViewCreateInfo view = {};
        view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image                       = out.image;
        view.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        view.format                      = format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (!Check(s.dapi->BindImageMemory(s.device, out.image, out.block.memory, out.block.offset), "vkBindImageMemory")
            || !Check(s.dapi->CreateImageView(s.device, &view, nullptr, &out.view), "vkCreateImageView"))
        {
            DestroyImage(out);
            return false;
        }
        out.size   = size;
        out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    void DestroyDepth()
    {
        if (s.depthView != VK_NULL_HANDLE) s.dapi->DestroyImageView(s.device, s.depthView, nullptr);
        if (s.depth != VK_NULL_HANDLE) s.dapi->DestroyImage(s.device, s.depth, nullptr);
        memory::Free(s.depthBlock);
        s.depth = VK_NULL_HANDLE;
        s.depthView = VK_NULL_HANDLE;
        s.depthBlock = memory::Block();
        s.depthExtent = { 0, 0 };
        s.depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    /// The terrain's depth image at the target's size (step 5e).
    bool CreateDepth(VkExtent2D extent)
    {
        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = VK_FORMAT_R32_UINT;
        info.extent        = VkExtent3D{ extent.width, extent.height, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(s.dapi->CreateImage(s.device, &info, nullptr, &s.depth), "vkCreateImage (terrain depth)")) return false;
        VkMemoryRequirements req = {};
        s.dapi->GetImageMemoryRequirements(s.device, s.depth, &req);
        if (!memory::Allocate(req, memory::Where::DeviceLocal, s.depthBlock))
        {
            s.dapi->DestroyImage(s.device, s.depth, nullptr);
            s.depth = VK_NULL_HANDLE;
            return false;
        }
        VkImageViewCreateInfo view = {};
        view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image                       = s.depth;
        view.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        view.format                      = VK_FORMAT_R32_UINT;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (!Check(s.dapi->BindImageMemory(s.device, s.depth, s.depthBlock.memory, s.depthBlock.offset), "vkBindImageMemory (terrain depth)")
            || !Check(s.dapi->CreateImageView(s.device, &view, nullptr, &s.depthView), "vkCreateImageView (terrain depth)"))
        {
            DestroyDepth();
            return false;
        }
        s.depthExtent = extent;
        s.depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    void Barrier(VkCommandBuffer cmd, Image& img, VkImageLayout to, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                 VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier b = {};
        b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask               = srcAccess;
        b.dstAccessMask               = dstAccess;
        b.oldLayout                   = img.layout;
        b.newLayout                   = to;
        b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.image                       = img.image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        s.dapi->CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        img.layout = to;
    }

    /// An image never filled: cleared to `value` once, then left for the shader.
    void Clear(VkCommandBuffer cmd, Image& img, const float value[4])
    {
        if (img.layout != VK_IMAGE_LAYOUT_UNDEFINED) return;
        Barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue colour = {};
        std::memcpy(colour.float32, value, sizeof colour.float32);
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        s.dapi->CmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
        Barrier(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, s.readers,
                VK_ACCESS_SHADER_READ_BIT);
    }

    /// A map's cells staged and copied into its image (remade for another size). Ordered after the
    /// frames before (their terrain read it) by the barrier: same queue, earlier submissions.
    bool Upload(VkCommandBuffer cmd, Image& img, const float* cells, uint32_t size)
    {
        if (img.size != size)
        {
            // Once per map size (normally once a session): the frames in flight may still read it.
            s.dapi->DeviceWaitIdle(s.device);
            DestroyImage(img);
            if (!CreateImage(size, kMapFormat, img)) return false;
        }

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(size) * size * 16;
        VkBuffer     buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        void*        mapped = nullptr;
        if (!frame::Stage(bytes, buffer, offset, mapped)) return false;
        std::memcpy(mapped, cells, static_cast<size_t>(bytes));

        Barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, s.readers, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy = {};
        copy.bufferOffset                = offset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent                 = VkExtent3D{ size, size, 1 };
        s.dapi->CmdCopyBufferToImage(cmd, buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, s.readers,
                VK_ACCESS_SHADER_READ_BIT);
        return true;
    }

    /// A trail layer: the whole of it (a new window, a new image), else only the rectangles that changed.
    bool UploadTrail(VkCommandBuffer cmd, Image& img, const Frame::TrailLayer& t, bool& present)
    {
        if (!t.cells || t.size == 0 || t.size > gxvk::vk::cover::kMaxTrail)
        {
            present = false;
            return true;
        }
        if (!present || t.full || img.size != t.size)
        {
            present = Upload(cmd, img, t.cells, t.size);
            return present;
        }
        if (!t.rectCount || !t.rects) return true;

        VkDeviceSize bytes = 0;
        for (uint32_t r = 0; r < t.rectCount; ++r) bytes += static_cast<VkDeviceSize>(t.rects[r * 4 + 2]) * t.rects[r * 4 + 3] * 16;
        VkBuffer     buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        void*        mapped = nullptr;
        // Not staged: the changes are gone (the provider forgot them), so the whole layer next frame.
        if (!frame::Stage(bytes, buffer, offset, mapped))
        {
            present = false;
            return false;
        }

        static VkBufferImageCopy copies[256];
        const uint32_t count = t.rectCount < 256 ? t.rectCount : 256;
        VkDeviceSize at = 0;
        for (uint32_t r = 0; r < count; ++r)
        {
            const int32_t x = t.rects[r * 4], y = t.rects[r * 4 + 1], w = t.rects[r * 4 + 2], h = t.rects[r * 4 + 3];
            for (int32_t row = 0; row < h; ++row)
                std::memcpy(static_cast<uint8_t*>(mapped) + at + static_cast<VkDeviceSize>(row) * w * 16, t.cells + (static_cast<size_t>(y + row) * t.size + x) * 4,
                            static_cast<size_t>(w) * 16);
            VkBufferImageCopy& c = copies[r];
            c = VkBufferImageCopy{};
            c.bufferOffset                = offset + at;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageOffset                 = VkOffset3D{ x, y, 0 };
            c.imageExtent                 = VkExtent3D{ static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
            at += static_cast<VkDeviceSize>(w) * h * 16;
        }

        Barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, s.readers, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        s.dapi->CmdCopyBufferToImage(cmd, buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, copies);
        Barrier(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, s.readers, VK_ACCESS_SHADER_READ_BIT);
        return true;
    }

    VkSampler MakeSampler(bool texture, float aniso, VkFilter filter)
    {
        VkSamplerCreateInfo info = {};
        info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter    = filter;
        info.minFilter    = filter;
        info.mipmapMode   = texture ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;   // the maps are toroidal, the textures tile
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.anisotropyEnable = texture && aniso > 1.0f ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy    = texture && aniso > 1.0f ? aniso : 1.0f;
        info.maxLod       = texture ? 1000.0f : 0.0f;
        VkSampler sampler = VK_NULL_HANDLE;
        Check(s.dapi->CreateSampler(s.device, &info, nullptr, &sampler), "vkCreateSampler");
        return sampler;
    }
}

namespace gxvk::vk::cover
{
    void SetProvider(Provider provider) { s.provider = provider; }

    bool Ready() { return s.ready; }
    bool Bindless() { return s.ready && s.bindless; }
    uint32_t TextureSlots() { return s.ready ? s.textureSlots : 0u; }
    bool Tessellation() { return s.ready && s.tessellation; }
    float MaxTessLevel() { return s.maxTessLevel; }

    VkDescriptorSetLayout SetLayout() { return s.setLayout; }

    bool Init()
    {
        if (s.ready) return true;
        if (s.failed) return false;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi || !h->iapi) return false;
        s.device = h->device;
        s.dapi   = h->dapi;
        const loader::Device& d = *s.dapi;

        // The terrain's pixel stage samples its own 9, the two maps and the texture slots: 1024 slots with
        // per-pixel indexing where the device has it and the limits allow, else 16 (more than Vulkan's
        // guaranteed 16 samplers per stage either way).
        VkPhysicalDeviceProperties props = {};
        h->iapi->GetPhysicalDeviceProperties(h->physical, &props);
        const VkPhysicalDeviceLimits& lim = props.limits;
        auto fits = [&](uint32_t slots) {
            const uint32_t samplers = kTerrainSamplers + kMapSamplers + slots;
            return lim.maxPerStageDescriptorSamplers >= samplers && lim.maxPerStageDescriptorSampledImages >= samplers && lim.maxPerStageResources >= samplers + 3 &&
                   lim.maxDescriptorSetSamplers >= samplers && lim.maxDescriptorSetSampledImages >= samplers;
        };

        // The terrain's pixels write the surface grid (step 5e): every desktop GPU can; without it the
        // terrain stays on the stock shaders, like a device short of samplers.
        if (!(context::Caps().enabled & gxvk::vk::kCapFragmentStores))
        {
            Write(Warn, "vulkan: cover: the device can't store from pixel shaders (fragmentStoresAndAtomics); the terrain stays on the stock shaders, without covers");
            s.failed = true;
            return false;
        }
        static SXC::Settings::Bool s_force16("Terrain", "CoverSlots16", false, "Cover textures: 16 slots",
                                             "Use the 16 texture slots of GPUs without per-pixel indexing even where it's there (testing the older path). "
                                             "Takes effect at the next start.");
        const bool indexing = (context::Caps().enabled & gxvk::vk::kCapSampledArrayNonUniform) != 0 && !s_force16.Get();
        s.bindless     = indexing && fits(kBindlessSlots);
        s.textureSlots = s.bindless ? kBindlessSlots : kFallbackSlots;
        if (!fits(kFallbackSlots) || lim.maxUniformBufferRange < sizeof(Params))
        {
            Write(Warn, "vulkan: cover: the device takes %u samplers per stage (%u needed); the terrain stays on the stock shaders, without covers",
                  lim.maxPerStageDescriptorSamplers, kTerrainSamplers + kMapSamplers + kFallbackSlots);
            s.failed = true;
            return false;
        }
        if (indexing && !s.bindless)
            Write(Debug, "vulkan: cover: per-pixel texture indexing is there, but %u samplers per stage aren't (%u): 16 texture slots",
                  kTerrainSamplers + kMapSamplers + kBindlessSlots, lim.maxPerStageDescriptorSamplers);

        // The raised chunks (step 5c): where the device tessellates (the caps probe enabled it).
        s.tessellation = (context::Caps().enabled & gxvk::vk::kCapTessellation) != 0 && lim.maxTessellationGenerationLevel >= 8;
        s.maxTessLevel = s.tessellation ? static_cast<float>(lim.maxTessellationGenerationLevel < 32 ? lim.maxTessellationGenerationLevel : 32) : 1.0f;
        s.readers = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    (s.tessellation ? VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT : 0u);

        const uint32_t bindingCount = s.bindless ? kFirstTex + 1 : kFirstTex + kFallbackSlots;
        VkDescriptorSetLayoutBinding bindings[kMaxBindings] = {};
        for (uint32_t i = 0; i < bindingCount; ++i)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = i == 0                                          ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                          : i == kSurfaceBinding || i == kDepthBinding ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                                          : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = s.bindless && i == kFirstTex ? kBindlessSlots : 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            // The parameters and the maps are read where the triangles are subdivided and where the
            // raised points are made (both take the covers' height).
            if (s.tessellation && i < kSurfaceBinding)
                bindings[i].stageFlags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        }
        // The ground marks: the grass reads them per plant (vertex), the terrain later (pixel).
        bindings[bindingCount].binding         = gxvk::vk::cover::kMarksBinding;
        bindings[bindingCount].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[bindingCount].descriptorCount = 1;
        bindings[bindingCount].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        // The water's shore field, its cells and its long waves: the swash on the beach (the terrain's pixels).
        for (uint32_t k = 1; k <= 4; ++k)
        {
            bindings[bindingCount + k].binding         = k == 1 ? gxvk::vk::cover::kShoreFieldBinding
                                                       : k == 2 ? gxvk::vk::cover::kShoreCellsBinding
                                                       : k == 3 ? gxvk::vk::cover::kShoreWavesBinding
                                                                : gxvk::vk::cover::kShoreRipplesBinding;
            bindings[bindingCount + k].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[bindingCount + k].descriptorCount = 1;
            bindings[bindingCount + k].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setLayout = {};
        setLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayout.bindingCount = bindingCount + 5;
        setLayout.pBindings    = bindings;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &setLayout, nullptr, &s.setLayout), "vkCreateDescriptorSetLayout")) return false;

        VkDescriptorPoolSize sizes[3] = {};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = kSlots;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = (kMapSamplers + s.textureSlots) * kSlots;
        sizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sizes[2].descriptorCount = 2 * kSlots;
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
                Write(Error, "vulkan: cover: no buffer for the parameters");
                s.failed = true;
                return false;
            }
        }

        // The maps are filtered where the device can filter 32-bit floats (not guaranteed; common).
        VkFormatProperties mapFormat = {};
        if (h->iapi->GetPhysicalDeviceFormatProperties) h->iapi->GetPhysicalDeviceFormatProperties(h->physical, kMapFormat, &mapFormat);
        const bool linearMaps = (mapFormat.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        if (!linearMaps) Write(Warn, "vulkan: cover: the device can't filter RGBA32F; the cover maps are read unfiltered (steps of a cell)");
        s.mapSampler = MakeSampler(false, 1.0f, linearMaps ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        s.texSampler = MakeSampler(true, h->samplerAnisotropy ? (props.limits.maxSamplerAnisotropy < 16.0f ? props.limits.maxSamplerAnisotropy : 16.0f) : 1.0f,
                                   VK_FILTER_LINEAR);
        if (s.failed) return false;

        // 1 x 1 placeholders until the maps come (no snow, all kept, dry, no water) and for missing textures.
        if (!CreateImage(1, kMapFormat, s.nearMap) || !CreateImage(1, kMapFormat, s.farMap) || !CreateImage(1, VK_FORMAT_R8G8B8A8_UNORM, s.white)
            || !CreateImage(1, kMapFormat, s.trailMaps[0]) || !CreateImage(1, kMapFormat, s.trailMaps[1]) || !CreateImage(1, kMapFormat, s.marks)
            || !CreateImage(1, kMapFormat, s.surface, kSurfaceUsage) || !CreateDepth(VkExtent2D{ 1, 1 }))
            return false;

        s.ready = true;
        Write(Info, "vulkan: cover: ready (%u texture slots%s, %s, a %u-byte parameter block)", s.textureSlots, s.bindless ? ", indexed per pixel" : "",
              s.tessellation ? "covers raise the terrain (tessellation)" : "flat covers (no tessellation)", static_cast<unsigned>(sizeof(Params)));
        return true;
    }

    VkDescriptorSet FrameSet(uint32_t slotIndex, uint64_t serial)
    {
        if (!s.ready || slotIndex >= kSlots) return VK_NULL_HANDLE;
        Slot& slot = s.slots[slotIndex];
        if (slot.serial == serial) return slot.set;

        VkCommandBuffer pre = frame::PrePass();
        if (!pre) return VK_NULL_HANDLE;

        static Frame f;   // ~30 KB: off the stack
        f = Frame();
        const bool on = s.provider && s.provider(f);
        f.slotCount = f.slotCount < s.textureSlots ? f.slotCount : s.textureSlots;

        static const float kEmpty[4] = { 0.0f, 1.0f, 0.0f, kNoWater };
        static const float kWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        Clear(pre, s.nearMap, kEmpty);
        Clear(pre, s.farMap, kEmpty);
        Clear(pre, s.white, kWhite);

        bool uploaded = true;
        if (!on || !f.nearCells || f.nearSize == 0 || f.nearSize > kMaxMap)
            s.nearPresent = false;
        else if (!s.nearPresent || f.nearVersion != s.nearVersion || s.nearMap.size != f.nearSize)
        {
            s.nearPresent = Upload(pre, s.nearMap, f.nearCells, f.nearSize);
            s.nearVersion = f.nearVersion;
            uploaded = uploaded && s.nearPresent;
        }
        if (!on || !f.farCells || f.farSize == 0 || f.farSize > kMaxMap)
            s.farPresent = false;
        else if (!s.farPresent || f.farVersion != s.farVersion || s.farMap.size != f.farSize)
        {
            s.farPresent = Upload(pre, s.farMap, f.farCells, f.farSize);
            s.farVersion = f.farVersion;
            uploaded = uploaded && s.farPresent;
        }
        // The trails: what changed since last frame (or all of a new window).
        static const float kUntouched[4] = { 1.0e6f, 0.0f, 0.0f, 0.0f };
        const Frame::TrailLayer* trailLayers[2] = { &f.trailFine, &f.trailCoarse };
        for (int k = 0; k < 2; ++k)
        {
            Clear(pre, s.trailMaps[k], kUntouched);
            if (!on || !f.trails)
                s.trailPresent[k] = false;
            else
                uploaded = UploadTrail(pre, s.trailMaps[k], *trailLayers[k], s.trailPresent[k]) && uploaded;
            Clear(pre, s.trailMaps[k], kUntouched);
        }
        // The ground marks (none: all zero), with the covers off too.
        static const float kNoMarks[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Clear(pre, s.marks, kNoMarks);
        if (!f.marksCells || f.marksSize == 0 || f.marksSize > kMaxMap)
            s.marksPresent = false;
        else if (!s.marksPresent || f.marksVersion != s.marksVersion || s.marks.size != f.marksSize)
        {
            s.marksPresent = Upload(pre, s.marks, f.marksCells, f.marksSize);
            s.marksVersion = f.marksVersion;
            uploaded = uploaded && s.marksPresent;
        }
        Clear(pre, s.marks, kNoMarks);
        Clear(pre, s.nearMap, kEmpty); // remade for a new size and not filled: cleared
        Clear(pre, s.farMap, kEmpty);

        // The surface grid (step 5e): remade at the size asked for (the client told first, its buffers
        // go); what this slot's frame copied the last time round, handed over in place (that frame has
        // completed); the grid cleared once (a tag no cell has); then this frame's copy of what the frames
        // before wrote into the slot's other buffer, for its next turn. The pixels write it in GENERAL.
        const bool surfaceWanted = on && f.surface && f.surfaceSink && f.surfaceSize > 1 && f.surfaceSize <= kMaxSurface;
        if (f.surfaceSink) s.surfaceSink = f.surfaceSink;
        if (surfaceWanted && s.surface.size != f.surfaceSize)
        {
            s.dapi->DeviceWaitIdle(s.device);
            if (s.surfaceSink) s.surfaceSink(nullptr, 0, 0.0f);
            for (Slot& other : s.slots)
            {
                memory::DestroyBuffer(other.surfaceRead[0]);
                memory::DestroyBuffer(other.surfaceRead[1]);
                other.surfacePending = -1;
            }
            DestroyImage(s.surface);
            if (!CreateImage(f.surfaceSize, kMapFormat, s.surface, kSurfaceUsage) && !CreateImage(1, kMapFormat, s.surface, kSurfaceUsage)) return VK_NULL_HANDLE;
        }
        if (slot.surfacePending == 0 || slot.surfacePending == 1)
        {
            memory::Buffer& read = slot.surfaceRead[slot.surfacePending];
            memory::InvalidateHost(read.block);
            if (on && f.surfaceSink) f.surfaceSink(static_cast<const float*>(read.block.mapped), s.surface.size, slot.surfaceCell);
            slot.surfacePending = slot.surfacePending + 2;  // handed over: the next copy takes the other one
        }
        if (s.surface.layout == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            Barrier(pre, s.surface, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue none = {};
            none.float32[0] = -1.0e9f;
            none.float32[1] = -1.0f;   // no cell's tag
            none.float32[2] = -1.0e9f;
            VkImageSubresourceRange range = {};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            s.dapi->CmdClearColorImage(pre, s.surface.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &none, 1, &range);
            Barrier(pre, s.surface, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT);
        }
        else if (surfaceWanted && s.surface.size == f.surfaceSize)
        {
            // The buffer not handed over last (2 + k: k was handed over; -1: none yet).
            const int target = slot.surfacePending >= 2 ? 3 - slot.surfacePending : 0;
            memory::Buffer& into = slot.surfaceRead[target];
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(f.surfaceSize) * f.surfaceSize * 16;
            if (into.handle == VK_NULL_HANDLE
                && (!memory::CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, memory::Where::Readback, into) || !into.block.mapped))
            {
                memory::DestroyBuffer(into);
                Write(Error, "vulkan: cover: no buffer to read the surface grid back into");
                s.failed = true;
                return VK_NULL_HANDLE;
            }
            Barrier(pre, s.surface, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy = {};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent                 = VkExtent3D{ f.surfaceSize, f.surfaceSize, 1 };
            s.dapi->CmdCopyImageToBuffer(pre, s.surface.image, VK_IMAGE_LAYOUT_GENERAL, into.handle, 1, &copy);
            Barrier(pre, s.surface, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT);
            // The copy's write made visible to the host before this slot's fence is waited.
            VkMemoryBarrier toHost = {};
            toHost.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            s.dapi->CmdPipelineBarrier(pre, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
            slot.surfacePending = target;
            slot.surfaceCell = f.surfaceCell;
        }

        // The terrain's depth per pixel (step 5e): at the target's size, cleared to "none" (all ones) for
        // this frame's terrain; the frames before have finished with it (same queue, the barrier waits).
        const VkExtent2D extent = frame::Extent();
        if (extent.width > 0 && extent.height > 0 && (extent.width != s.depthExtent.width || extent.height != s.depthExtent.height))
        {
            s.dapi->DeviceWaitIdle(s.device);
            DestroyDepth();
            if (!CreateDepth(extent) && !CreateDepth(VkExtent2D{ 1, 1 })) return VK_NULL_HANDLE;
        }
        {
            VkImageMemoryBarrier b = {};
            b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask               = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout                   = s.depthLayout;
            b.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
            b.image                       = s.depth;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;
            s.dapi->CmdPipelineBarrier(pre, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkClearColorValue none = {};
            none.uint32[0] = 0xFFFFFFFFu;
            s.dapi->CmdClearColorImage(pre, s.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &none, 1, &b.subresourceRange);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
            s.dapi->CmdPipelineBarrier(pre, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            s.depthLayout = VK_IMAGE_LAYOUT_GENERAL;
        }

        // The parameters, into this slot's buffer (its previous frame has completed).
        Params p = {};
        const uint32_t entries = on && f.entries ? (f.entryCount < kMaxEntries ? f.entryCount : kMaxEntries) : 0;
        p.enable[0]  = on ? 1.0f : 0.0f;
        p.enable[1]  = static_cast<float>(f.debug);
        p.enable[2]  = f.wetDarkening;
        p.enable[3]  = f.brightness;
        p.nearMap[0] = f.nearCell;
        p.nearMap[1] = static_cast<float>(s.nearPresent ? s.nearMap.size : 1);
        p.nearMap[2] = static_cast<float>(f.nearFirstI);
        p.nearMap[3] = static_cast<float>(f.nearFirstJ);
        p.farMap[0]  = f.farCell;
        p.farMap[1]  = static_cast<float>(s.farPresent ? s.farMap.size : 1);
        p.farMap[2]  = static_cast<float>(f.farFirstI);
        p.farMap[3]  = static_cast<float>(f.farFirstJ);
        p.outside[0] = f.outside[0];
        p.outside[1] = f.outside[1];
        p.outside[2] = f.outside[2];
        p.outside[3] = s.nearPresent ? 1.0f : 0.0f;
        p.tuning[0]  = f.thinDepth > 0.001f ? f.thinDepth : 0.001f;
        p.tuning[1]  = 1.0f / (f.textureTile > 0.01f ? f.textureTile : 0.01f);
        p.tuning[2]  = f.depthScale;
        p.tuning[3]  = s.farPresent ? 1.0f : 0.0f;
        p.tuning2[0] = f.breakupScale;
        p.tuning2[1] = f.driftScale;
        p.tuning2[2] = f.fallenSnow ? 1.0f : 0.0f;
        p.tuning2[3] = static_cast<float>(entries);
        p.tess[0]    = f.tessDetail;
        p.tess[1]    = f.tessReach;
        p.tess[2]    = f.tessMost < s.maxTessLevel ? f.tessMost : s.maxTessLevel;
        p.tess[3]    = on && s.tessellation && f.tessDetail > 0.0f ? 1.0f : 0.0f;
        float* trailRows[2] = { p.trailFine, p.trailCoarse };
        for (int k = 0; k < 2; ++k)
        {
            trailRows[k][0] = trailLayers[k]->cell;
            trailRows[k][1] = static_cast<float>(s.trailPresent[k] ? s.trailMaps[k].size : 0);
            trailRows[k][2] = static_cast<float>(trailLayers[k]->firstI);
            trailRows[k][3] = static_cast<float>(trailLayers[k]->firstJ);
        }
        p.trail[0]   = f.trailNow;
        p.trail[1]   = f.trailPersist;
        p.trail[2]   = f.trailSlur;
        p.trail[3]   = on && f.trails && (s.trailPresent[0] || s.trailPresent[1]) ? 1.0f : 0.0f;
        p.trail2[0]  = f.trailRim;
        p.surface[0] = f.surfaceCell;
        p.surface[1] = surfaceWanted && s.surface.size == f.surfaceSize && f.surfaceCell > 0.0f ? static_cast<float>(f.surfaceSize) : 0.0f;
        p.surface[2] = static_cast<float>(f.surfaceFirstI);
        p.surface[3] = static_cast<float>(f.surfaceFirstJ);
        // The water's shore field, for the swash (none: shoreGrid[2] 0, the white image bound instead).
        const gxvk::vk::water::ShoreShared shore = gxvk::vk::water::Shore();
        if (shore.valid)
        {
            std::memcpy(p.shoreGrid, shore.grid, sizeof p.shoreGrid);
            std::memcpy(p.shoreLook, shore.look, sizeof p.shoreLook);
            std::memcpy(p.shoreWind, shore.wind, sizeof p.shoreWind);
            p.shoreLook[3] = shore.swash;
            std::memcpy(p.shoreWave, shore.wave, sizeof p.shoreWave);
            std::memcpy(p.shoreCamera, shore.camera, sizeof p.shoreCamera);
            std::memcpy(p.shoreCaustic, shore.caustic, sizeof p.shoreCaustic);
            std::memcpy(p.shoreSun, shore.sun, sizeof p.shoreSun);
        }
        // The sun's shadow map on ground turned from the sun: the stock darkens it too, so units' shadows
        // show through hills onto their far slopes (Seyri, 2026-11-05). Ours fades the cascades there.
        static SXC::Settings::Bool s_facing("Terrain", "ShadowFacing", true, "Sun shadows: not on ground turned from the sun",
                                            "Fade the sun's shadow map where the ground faces away from the sun, so units' shadows "
                                            "no longer fall through hills onto their far slopes. Off: as the stock client.");
        static SXC::Settings::Float s_facingWidth("Terrain", "ShadowFacingWidth", 0.05f, 0.01f, 0.3f, "Sun shadows: fade width",
                                                  "How gradually the shadow fades as the ground turns from the sun (in N.L: 0.05 is about 3 degrees).");
        p.sunShadow[0] = s_facing.Get() ? 1.0f : 0.0f;
        p.sunShadow[1] = s_facingWidth.Get();
        // Per texture: the slot showing it (a loaded one), and its gloss flag.
        for (uint32_t k = 0; k < kMaxTextures; ++k)
            p.texInfo[k / 4][k % 4] = on && f.textureGloss && f.textureGloss[k] ? 2.0f : 0.0f;
        uint32_t textures = 0;
        for (uint32_t sl = 0; on && sl < f.slotCount; ++sl)
        {
            const uint32_t id = f.slotTexture[sl];
            if (id < 1 || id > kMaxTextures || f.slots[sl].view == VK_NULL_HANDLE) continue;
            p.texInfo[(id - 1) / 4][(id - 1) % 4] += 4.0f * static_cast<float>(sl + 1);
            ++textures;
        }
        if (entries) std::memcpy(p.entries, f.entries, entries * kEntryRows * 16);
        std::memcpy(slot.params.block.mapped, &p, sizeof p);
        memory::FlushHost(slot.params.block);

        // The parameters and the maps every frame; the texture bindings when a view changed (in the
        // bindless array most stay put frame after frame). An empty slot holds the white image.
        VkDescriptorBufferInfo params = { slot.params.handle, 0, sizeof(Params) };
        VkDescriptorImageInfo  maps[4] = {
            { s.mapSampler, s.nearMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { s.mapSampler, s.farMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { s.mapSampler, s.trailMaps[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { s.mapSampler, s.trailMaps[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        };
        const VkDescriptorImageInfo marks = { s.mapSampler, s.marks.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo shoreField = shore.valid ? VkDescriptorImageInfo{ shore.fieldSampler, shore.field, VK_IMAGE_LAYOUT_GENERAL }
                                                             : VkDescriptorImageInfo{ s.mapSampler, s.white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo shoreCells = shore.valid ? VkDescriptorImageInfo{ shore.cellsSampler, shore.cells, VK_IMAGE_LAYOUT_GENERAL }
                                                             : VkDescriptorImageInfo{ s.mapSampler, s.white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo shoreWaves = shore.valid ? VkDescriptorImageInfo{ shore.wavesSampler, shore.waves, VK_IMAGE_LAYOUT_GENERAL }
                                                             : VkDescriptorImageInfo{ s.mapSampler, s.white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo shoreRipples = shore.valid ? VkDescriptorImageInfo{ shore.wavesSampler, shore.ripples, VK_IMAGE_LAYOUT_GENERAL }
                                                               : VkDescriptorImageInfo{ s.mapSampler, s.white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo surface = { VK_NULL_HANDLE, s.surface.view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo depth = { VK_NULL_HANDLE, s.depthView, VK_IMAGE_LAYOUT_GENERAL };
        static VkDescriptorImageInfo texImages[kBindlessSlots];
        bool texturesChanged = !slot.texturesWritten;
        for (uint32_t sl = 0; sl < s.textureSlots; ++sl)
        {
            const bool have = on && sl < f.slotCount && f.slotTexture[sl] && f.slots[sl].view != VK_NULL_HANDLE;
            // Every image a draw binds rests in SHADER_READ_ONLY_OPTIMAL (Texture.cpp).
            const VkImageView view = have ? f.slots[sl].view : s.white.view;
            const uint32_t    id   = have ? f.slots[sl].imageId : 0u;
            texImages[sl] = { s.texSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            texturesChanged = texturesChanged || slot.written[sl] != id;
            slot.written[sl] = id;
        }

        VkWriteDescriptorSet writes[kMaxBindings] = {};
        uint32_t count = 0;
        auto add = [&](uint32_t binding, VkDescriptorType type, uint32_t n) -> VkWriteDescriptorSet& {
            VkWriteDescriptorSet& w = writes[count++];
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = slot.set;
            w.dstBinding      = binding;
            w.descriptorCount = n;
            w.descriptorType  = type;
            return w;
        };
        add(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1).pBufferInfo = &params;
        add(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1).pImageInfo = &maps[0];
        add(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1).pImageInfo = &maps[1];
        add(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1).pImageInfo = &maps[2];
        add(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1).pImageInfo = &maps[3];
        add(kSurfaceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1).pImageInfo = &surface;
        add(kDepthBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1).pImageInfo = &depth;
        add(gxvk::vk::cover::kMarksBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1).pImageInfo = &marks;
        add(gxvk::vk::cover::kShoreFieldBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1).pImageInfo = &shoreField;
        add(gxvk::vk::cover::kShoreCellsBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1).pImageInfo = &shoreCells;
        add(gxvk::vk::cover::kShoreWavesBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1).pImageInfo = &shoreWaves;
        add(gxvk::vk::cover::kShoreRipplesBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1).pImageInfo = &shoreRipples;
        if (texturesChanged)
        {
            if (s.bindless)
                add(kFirstTex, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kBindlessSlots).pImageInfo = texImages;
            else
                for (uint32_t sl = 0; sl < kFallbackSlots; ++sl)
                    add(kFirstTex + sl, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1).pImageInfo = &texImages[sl];
            slot.texturesWritten = true;
        }
        s.dapi->UpdateDescriptorSets(s.device, count, writes, 0, nullptr);
        slot.serial = serial;

        if (!on)
            std::snprintf(s.status, sizeof s.status, "terrain drawn, covers off");
        else
            std::snprintf(s.status, sizeof s.status, "drawn: near map %s, far map %s, %u entries, %u of %u texture slots showing%s%s",
                          s.nearPresent ? "present" : "none", s.farPresent ? "present" : "none", entries, textures, s.textureSlots,
                          s.bindless ? " (indexed per pixel)" : "", uploaded ? "" : " (an upload failed)");
        return slot.set;
    }

    const char* Status() { return s.status; }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        DestroyImage(s.nearMap);
        DestroyImage(s.farMap);
        DestroyImage(s.white);
        DestroyImage(s.trailMaps[0]);
        DestroyImage(s.trailMaps[1]);
        DestroyImage(s.marks);
        DestroyImage(s.surface);
        DestroyDepth();
        if (s.surfaceSink) s.surfaceSink(nullptr, 0, 0.0f);  // its buffers go
        for (Slot& slot : s.slots)
        {
            memory::DestroyBuffer(slot.params);
            memory::DestroyBuffer(slot.surfaceRead[0]);
            memory::DestroyBuffer(slot.surfaceRead[1]);
        }
        if (s.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.mapSampler != VK_NULL_HANDLE) d.DestroySampler(s.device, s.mapSampler, nullptr);
        if (s.texSampler != VK_NULL_HANDLE) d.DestroySampler(s.device, s.texSampler, nullptr);
        if (s.setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        const gxvk::vk::cover::Provider provider = s.provider;
        s = State();
        s.provider = provider;   // the device's, kept for the next bring-up
    }
}
