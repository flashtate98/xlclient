// The set-0 descriptor side: one descriptor pool per frame slot with a per-frame cache of the sets
// written from it, and the 1x1 white images a slot without a texture binds.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines/Internal.hpp"

#include "SXC/Settings/Settings.h"  // SXC: [Vulkan] KeepDescriptorSets

#include <cstring>
#include <new>

namespace
{
    using namespace gxvk::vk::pipelines::detail;

    /// The slot of a (mask, images, samplers) tuple.
    uint32_t HashOf(uint32_t mask, const Texture* stages)
    {
        uint32_t h = mask * 0x9E3779B1u;
        for (uint32_t slot = 0; slot < kTextureStages; ++slot)
        {
            if (!(mask & (1u << slot))) continue;
            h ^= (stages[slot].imageId + slot) * 0x85EBCA77u;
            h  = (h << 7) | (h >> 25);
            h ^= stages[slot].samplerKey * 0xC2B2AE3Du;
        }
        h ^= h >> 15;
        return h & (kCacheSlots - 1);
    }

    bool Same(const CacheEntry& e, uint32_t mask, const Texture* stages)
    {
        if (e.mask != mask) return false;
        for (uint32_t slot = 0; slot < kTextureStages; ++slot)
        {
            if (!(mask & (1u << slot))) continue;
            if (e.imageId[slot] != stages[slot].imageId || e.samplerKey[slot] != stages[slot].samplerKey) return false;
        }
        return true;
    }
}

namespace gxvk::vk::pipelines::detail
{
    bool CreateDescriptorObjects()
    {
        const loader::Device& d = *s.dapi;
        // One pool per frame slot, reset when its frame begins: the frame two behind, whose sets
        // these were, has completed by then (the frame waited its slot's fence).
        for (SlotSets& slot : s.slots)
        {
            VkDescriptorPoolSize size = {};
            size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            size.descriptorCount = kDescriptorsPerSlot;
            VkDescriptorPoolCreateInfo pool = {};
            pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.maxSets       = kSetsPerSlot;
            pool.poolSizeCount = 1;
            pool.pPoolSizes    = &size;
            const VkResult r = d.CreateDescriptorPool(s.device, &pool, nullptr, &slot.pool);
            if (r != VK_SUCCESS)
            {
                slot.pool = VK_NULL_HANDLE;
                Write(Error, "vulkan: pipelines: vkCreateDescriptorPool failed: %s", ResultName(r));
                return false;
            }
            slot.cache = new (std::nothrow) CacheEntry[kCacheSlots];
            if (!slot.cache)
            {
                Write(Error, "vulkan: pipelines: cannot allocate the descriptor cache");
                return false;
            }
            slot.serial    = 0;
            slot.allocated = 0;
        }
        return true;
    }

    void DestroyDescriptorObjects()
    {
        const loader::Device& d = *s.dapi;
        for (SlotSets& slot : s.slots)
        {
            DestroyRegs(slot);
            // vkDestroyDescriptorPool frees every set allocated from it.
            if (slot.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, slot.pool, nullptr);
            delete[] slot.cache;
            slot = SlotSets();
        }
        // The device is idle at Shutdown (the frame was released first), so the white images are
        // destroyed outright.
        if (s.white.handle != VK_NULL_HANDLE) texture::DestroyIdle(s.white);
        if (s.whiteCube.handle != VK_NULL_HANDLE) texture::DestroyIdle(s.whiteCube);
        s.whiteFailed     = false;
        s.whiteCubeFailed = false;
    }

    /**
     * The set-0 descriptor set of one (layout, images, samplers) tuple for the frame `serial` on
     * slot `slot`: the set written earlier this frame, or a fresh one from the slot's pool, written
     * now. The pool is reset at the first set of a new frame. Every slot of the layout's mask must
     * have a view in `stages`.
     *
     * @return VkDescriptorSet set : VK_NULL_HANDLE when the pool is exhausted or a sampler could
     *         not be had (logged once)
     */
    VkDescriptorSet DescriptorFor(uint32_t slot, uint64_t serial, const LayoutEntry& layout, const Texture* stages)
    {
        if (slot >= frame::kFramesInFlight) return VK_NULL_HANDLE;
        SlotSets& ss = s.slots[slot];
        if (ss.pool == VK_NULL_HANDLE || !ss.cache) return VK_NULL_HANDLE;
        const loader::Device& d = *s.dapi;

        if (ss.serial != serial)
        {
            // A new frame on this slot: the slot's previous frame has completed (it waited its fence).
            // SXC (FlashWOW): its sets are kept: a set is written once and never changed, an image id is
            // never given to another image, so a set made in an earlier frame still binds what its key
            // says (one whose image is gone just never matches again). The pool is reset (every set free,
            // the cache emptied by the generation) once fewer than a frame's old ceiling is left; with the
            // setting off, at every frame as before. Writing the sets again was 1.7% of the client's CPU
            // (VS, 24 players in Goldshire).
            static SXC::Settings::Bool s_keep("Vulkan", "KeepDescriptorSets", true, "Vulkan: keep texture sets across frames",
                                              "Keep the draws' texture descriptor sets from frame to frame instead of writing them again each frame.");
            ss.serial = serial;

            if (!s_keep.Get() || ss.allocated + kSetsPerFrame > kSetsPerSlot)
            {
                d.ResetDescriptorPool(s.device, ss.pool, 0);
                ss.allocated = 0;
                ++ss.generation;
            }
        }

        const uint64_t generation = ss.generation + 1;   // never 0: a fresh entry's serial reads as empty
        uint32_t p = HashOf(layout.mask, stages);
        for (;; p = (p + 1) & (kCacheSlots - 1))
        {
            CacheEntry& e = ss.cache[p];
            if (e.serial != generation) break;   // empty in this generation of the pool
            if (Same(e, layout.mask, stages))
            {
                ++s.stats.descriptorHits;
                return e.set;
            }
        }

        if (ss.allocated >= kSetsPerSlot)
        {
            if (!s.warnedSets)
            {
                s.warnedSets = true;
                Write(Warn, "vulkan: pipelines: a frame drew more than %u distinct texture/sampler tuples; further ones are skipped (logged once)", kSetsPerFrame);
            }
            ++s.stats.descriptorFull;
            return VK_NULL_HANDLE;
        }

        VkDescriptorImageInfo images[kTextureStages] = {};
        VkWriteDescriptorSet  writes[kTextureStages] = {};
        uint32_t              count = 0;
        for (uint32_t i = 0; i < kTextureStages; ++i)
        {
            if (!(layout.mask & (1u << i))) continue;
            VkSampler sampler = Sampler(stages[i].samplerKey);
            if (sampler == VK_NULL_HANDLE || stages[i].view == VK_NULL_HANDLE) return VK_NULL_HANDLE;
            // Every image a draw binds is left in SHADER_READ_ONLY_OPTIMAL by its fills (Texture.cpp).
            images[count].sampler     = sampler;
            images[count].imageView   = stages[i].view;
            images[count].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[count].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[count].dstBinding      = i;
            writes[count].descriptorCount = 1;
            writes[count].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[count].pImageInfo      = &images[count];
            ++count;
        }

        VkDescriptorSetAllocateInfo alloc = {};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = ss.pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &layout.set0;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = d.AllocateDescriptorSets(s.device, &alloc, &set);
        if (r != VK_SUCCESS)
        {
            if (!s.warnedSets)
            {
                s.warnedSets = true;
                Write(Error, "vulkan: pipelines: vkAllocateDescriptorSets failed: %s (logged once)", ResultName(r));
            }
            ++s.stats.descriptorFull;
            return VK_NULL_HANDLE;
        }
        ++ss.allocated;
        ++s.stats.descriptorSets;
        for (uint32_t i = 0; i < count; ++i) writes[i].dstSet = set;
        if (count) d.UpdateDescriptorSets(s.device, count, writes, 0, nullptr);

        CacheEntry& e = ss.cache[p];
        e.serial = generation;
        e.mask   = layout.mask;
        for (uint32_t i = 0; i < kTextureStages; ++i)
        {
            e.imageId[i]    = (layout.mask & (1u << i)) ? stages[i].imageId : 0;
            e.samplerKey[i] = (layout.mask & (1u << i)) ? stages[i].samplerKey : 0;
        }
        e.set = set;
        return set;
    }

    /// The 1x1 opaque white image (2D, or a cube map), made on first need and cleared through the
    /// frame's pre-pass, so a sampler slot without a texture has a valid image of its kind to bind.
    const texture::Image* White(bool cube)
    {
        texture::Image& img    = cube ? s.whiteCube : s.white;
        bool&           failed = cube ? s.whiteCubeFailed : s.whiteFailed;
        if (img.handle != VK_NULL_HANDLE) return &img;
        if (failed) return nullptr;
        VkCommandBuffer pre = frame::PrePass();
        if (!pre) return nullptr;
        const bool made = cube ? texture::CreateCube(1, 1, VK_FORMAT_R8G8B8A8_UNORM, img)
                               : texture::CreateMipped(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, img);
        if (!made)
        {
            failed = true;
            Write(Error, "vulkan: pipelines: the 1x1 white %s could not be made; draws needing it are skipped", cube ? "cube map" : "image");
            return nullptr;
        }
        static const float kWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        texture::RecordClear(pre, img, kWhite);
        return &img;
    }
}
