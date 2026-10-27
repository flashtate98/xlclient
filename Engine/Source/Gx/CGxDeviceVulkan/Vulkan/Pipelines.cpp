// The graphics pipelines a draw binds: the module's life and the record of one draw.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines/Internal.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Cover.hpp"  // SXC:
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Lights.hpp" // SXC:

#include <cstring>
#include <new>

namespace gxvk::vk::pipelines::detail
{
    State s;
}

using namespace gxvk::vk::pipelines::detail;

namespace
{
    /// The set-0 images of a draw for a layout: the draw's texture at each slot of the mask, or the
    /// white image of the slot's kind when the draw has none there. SXC: also when a texture is bound
    /// of the other kind (a 2D image under a cube sampler or the reverse), which the module cannot
    /// sample: D3D9 draws such a draw, and skipping it left whole terrain chunks invisible (a
    /// converted map's reflective layer on a plain texture). @p mismatched counts those slots. False
    /// only when a white image is missing.
    bool StageImages(const Draw& draw, const LayoutEntry& layout, uint32_t cubeMask, Texture* out, uint32_t& mismatched)
    {
        for (uint32_t slot = 0; slot < kTextureStages; ++slot)
        {
            if (!(layout.mask & (1u << slot))) continue;
            const bool wantCube = (cubeMask & (1u << slot)) != 0;
            if ((draw.textureMask & (1u << slot)) && draw.textures[slot].view != VK_NULL_HANDLE)
            {
                if (draw.textures[slot].cube == wantCube)
                {
                    out[slot] = draw.textures[slot];
                    continue;
                }
                ++mismatched;
            }
            const texture::Image* white = White(wantCube);
            if (!white) return false;
            out[slot].view       = white->view;
            out[slot].imageId    = white->id;
            out[slot].samplerKey = kWhiteSamplerKey;
            out[slot].cube       = wantCube;
        }
        return true;
    }
}

namespace gxvk::vk::pipelines
{
    bool Init()
    {
        if (s.ready) return true;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi || !h->iapi)
        {
            Write(Warn, "vulkan: pipelines: Init() without a device; declining");
            return false;
        }
        s.device = h->device;
        s.dapi   = h->dapi;
        const loader::Device& d = *s.dapi;

        VkPhysicalDeviceProperties props = {};
        h->iapi->GetPhysicalDeviceProperties(h->physical, &props);
        s.anisoMax  = h->samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f;
        s.regsAlign = props.limits.minUniformBufferOffsetAlignment ? static_cast<uint32_t>(props.limits.minUniformBufferOffsetAlignment) : 256u;
        s.wireframe = h->fillModeNonSolid;
        if (props.limits.maxUniformBufferRange < kSliceBytesLarge)
        {
            Write(Error, "vulkan: pipelines: maxUniformBufferRange %u cannot hold a %u-byte slice; declining", props.limits.maxUniformBufferRange, kSliceBytesLarge);
            return false;
        }
        // SXC: the push range grew past Vulkan's guaranteed 128 bytes for fixed-function fog.
        if (props.limits.maxPushConstantsSize < kPushBytes)
        {
            Write(Error, "vulkan: pipelines: maxPushConstantsSize %u cannot hold the %u-byte push range; declining", props.limits.maxPushConstantsSize, kPushBytes);
            return false;
        }

        s.entries = new (std::nothrow) Entry[kMaxPipelines];
        if (!s.entries)
        {
            Write(Error, "vulkan: pipelines: cannot allocate the pipeline table");
            return false;
        }

        if (!CreatePrograms() || !CreateRegsLayout() || !CreateDescriptorObjects())
        {
            Shutdown();
            return false;
        }
        gxvk::vk::cover::Init();  // SXC: optional; without it the terrain stays on the stock shaders
        gxvk::vk::lights::Init(); // SXC: our terrain shader needs it too (its set 3)

        // The set-1 sets, one per uniform chunk, for the session.
        VkDescriptorPoolSize size = {};
        size.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        size.descriptorCount = frame::kFramesInFlight * kMaxRegsChunks;
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = frame::kFramesInFlight * kMaxRegsChunks;
        pool.poolSizeCount = 1;
        pool.pPoolSizes    = &size;
        VkResult r = d.CreateDescriptorPool(s.device, &pool, nullptr, &s.regsPool);
        if (r != VK_SUCCESS)
        {
            s.regsPool = VK_NULL_HANDLE;
            Write(Error, "vulkan: pipelines: vkCreateDescriptorPool (set 1) failed: %s", ResultName(r));
            Shutdown();
            return false;
        }

        VkPipelineCacheCreateInfo cache = {};
        cache.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        if (d.CreatePipelineCache(s.device, &cache, nullptr, &s.cache) != VK_SUCCESS) s.cache = VK_NULL_HANDLE;

        if (!memory::CreateBuffer(kConstantBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory::Where::Stream, s.constant) || !s.constant.block.mapped)
        {
            Write(Error, "vulkan: pipelines: no buffer for the constant stream");
            Shutdown();
            return false;
        }
        std::memset(s.constant.block.mapped, 0, kConstantBytes);
        std::memset(static_cast<uint8_t*>(s.constant.block.mapped) + kConstantColourOffset, 0xFF, 16);
        memory::FlushHost(s.constant.block);

        s.ready = true;
        Write(Debug, "vulkan: pipelines: ready (%u own vertex and %u own fragment programs, %u-byte push constants, %u set-0 sets per frame, uniform slices of %u / %u bytes at alignment %u in %u KiB chunks)",
              kVertexPrograms - 1, kFragmentPrograms - 1, kPushBytes, kSetsPerSlot, kSliceBytesSmall, kSliceBytesLarge, s.regsAlign, kRegsChunkBytes / 1024);
        return true;
    }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        DropPipelines();
        DestroySamplers();
        DestroyDescriptorObjects();
        if (s.regsPool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.regsPool, nullptr);
        DestroyLayouts();
        gxvk::vk::cover::Shutdown();  // SXC: after the pipeline layouts over its set layout
        gxvk::vk::lights::Shutdown();
        memory::DestroyBuffer(s.constant);
        if (s.cache != VK_NULL_HANDLE) d.DestroyPipelineCache(s.device, s.cache, nullptr);
        DestroyPrograms();
        delete[] s.entries;
        s = State();
    }

    void ForgetBound() { s.bound = Bound(); }  // SXC:

    bool Record(VkCommandBuffer cmd, VkRenderPass pass, uint32_t passGeneration, uint64_t serial, uint32_t slot, const Draw& draw)
    {
        if (!s.ready || !cmd || pass == VK_NULL_HANDLE)
        {
            ++s.stats.skipped;
            return false;
        }
        if (draw.streamCount == 0 || draw.streamCount > kMaxStreams || !(draw.attribMask & (1u << kAttribPosition)) || (draw.count == 0 && draw.runCount == 0)
            || (draw.runs && !draw.indexed))
        {
            ++s.stats.skipped;
            return false;
        }

        // A new render pass generation: the pipelines were built against a pass that no longer
        // exists. The frame made the device idle before it dropped that pass.
        if (passGeneration != s.passGen)
        {
            DropPipelines();
            s.passGen = passGeneration;
        }

        // The sampler-slot set: the pixel record's for a translated module, slots 0 and 1 for the
        // fixed-function stages, slot 0 alone otherwise.
        const bool translated = TranslatedFamily(draw.vertex) || TranslatedFamily(draw.fragment);
        uint32_t samplerMask = 1u;
        uint32_t cubeMask    = 0;
        if (draw.fragment == FragmentProgram::Translated && draw.fragmentRecord)
        {
            samplerMask = draw.fragmentRecord->samplerMask;
            cubeMask    = draw.fragmentRecord->cubeMask;
        }
        else if (draw.fragment == FragmentProgram::Terrain)
        {
            samplerMask = terrain::SamplerMask(draw.terrainFragment);  // SXC: what the module declares
            cubeMask    = (draw.terrainFragment & terrain::kFragmentCube) ? 1u : 0u;
        }
        else if (draw.fragment == FragmentProgram::FixedFunction || draw.fragment == FragmentProgram::FixedFunctionTranslated
                 || draw.fragment == FragmentProgram::Decal)
            samplerMask = 3u;
        uint32_t layoutIndex = 0;
        // SXC: our terrain shader and the decals on the covers read the covers' set 2 (and the lights'
        // set 3); the translated pixel modules flagged `lights` (models, WMOs) the lights' set 3.
        const bool cover = draw.fragment == FragmentProgram::Terrain || draw.fragment == FragmentProgram::Decal;
        const bool lit = draw.fragment == FragmentProgram::Translated && draw.fragmentRecord && draw.fragmentRecord->lights;
        if (!LayoutFor(samplerMask, cover ? kExtraCover : (lit ? kExtraLights : kExtraNone), layoutIndex))
        {
            ++s.stats.skipped;
            return false;
        }

        Key k;
        std::memset(&k, 0, sizeof k);
        k.pass        = pass;
        k.passGen     = passGeneration;
        k.streamCount = draw.streamCount;
        for (uint32_t i = 0; i < draw.streamCount; ++i) k.stride[i] = draw.streams[i].stride;
        k.attribMask = draw.attribMask;
        for (uint32_t a = 0; a < kAttribCount; ++a)
            if (draw.attribMask & (1u << a)) k.attribs[a] = draw.attribs[a];
        k.topology       = draw.topology;
        k.blend          = draw.blend;
        k.depthTest      = draw.depthTest ? 1u : 0u;
        k.depthWrite     = draw.depthWrite ? 1u : 0u;
        k.depthFunc      = draw.depthFunc;
        k.cull           = draw.cull;
        k.colourMask     = draw.colourMask & 0xFu;
        k.wireframe      = (draw.wireframe && s.wireframe) ? 1u : 0u;
        k.vertex         = static_cast<uint32_t>(draw.vertex);
        k.fragment       = static_cast<uint32_t>(draw.fragment);
        k.vertexRecord   = draw.vertex == VertexProgram::Translated ? manifest::IndexOf(draw.vertexRecord) : manifest::kNoRecord;
        k.fragmentRecord = draw.fragment == FragmentProgram::Translated ? manifest::IndexOf(draw.fragmentRecord) : manifest::kNoRecord;
        k.layoutIndex    = layoutIndex;
        k.terrainVertex   = draw.vertex == VertexProgram::Terrain ? draw.terrainVertex : 0u;      // SXC:
        k.terrainFragment = draw.fragment == FragmentProgram::Terrain ? draw.terrainFragment : 0u;
        k.instanceStreams = draw.instanceStreams;   // SXC: the instanced grass
        k.plantStream     = draw.plantStream >= 0 ? static_cast<uint32_t>(draw.plantStream) + 1u : 0u;
        k.copyStream      = draw.copyStream >= 0 ? static_cast<uint32_t>(draw.copyStream) + 1u : 0u;

        const Entry* entry = Lookup(k);
        if (!entry || entry->pipeline == VK_NULL_HANDLE)
        {
            ++s.stats.skipped;
            return false;
        }
        const LayoutEntry& layout = s.layouts[layoutIndex];

        // Set 0: the images of the mask's slots (the fragment programs sample what they declare
        // unconditionally, so every slot binds a valid image of its kind).
        Texture stages[kTextureStages];
        uint32_t mismatched = 0;
        if (!StageImages(draw, layout, cubeMask, stages, mismatched))
        {
            ++s.stats.skipped;
            return false;
        }
        if (mismatched && s.stats.cubeMismatch++ == 0)
        {
            Write(Warn, "vulkan: pipelines: a draw binds a texture of the other kind than its sampler (2D / cube); "
                        "drawn with white there instead (counted in the cube mismatches)");
        }
        const VkDescriptorSet set0 = DescriptorFor(slot, serial, layout, stages);
        if (set0 == VK_NULL_HANDLE)
        {
            ++s.stats.skipped;
            return false;
        }

        // Set 1: the constant slice, for the translated family.
        VkDescriptorSet set1    = VK_NULL_HANDLE;
        uint32_t        offset1 = 0;
        if (translated && !RegsSlice(slot, serial, draw, set1, offset1))
        {
            ++s.stats.skipped;
            return false;
        }

        // SXC: set 2, the covers' (written and uploaded at the frame's first terrain draw).
        const VkDescriptorSet set2 = cover ? gxvk::vk::cover::FrameSet(slot, serial) : VK_NULL_HANDLE;
        // SXC: set 3, the lights' (filled at the frame's first use), for the draw's surface class: the
        // terrain outdoor; a lit module as the device found it (a WMO group indoor or outdoor, models
        // any, another scene than the world's none).
        const VkDescriptorSet set3 = cover ? gxvk::vk::lights::FrameSet(slot, serial, gxvk::vk::lights::kSurfaceOutdoor)
                                     : lit ? gxvk::vk::lights::FrameSet(slot, serial, draw.lightsSurface)
                                           : VK_NULL_HANDLE;
        if ((cover && set2 == VK_NULL_HANDLE) || ((cover || lit) && set3 == VK_NULL_HANDLE))
        {
            ++s.stats.skipped;
            return false;
        }

        const loader::Device& d = *s.dapi;
        Bound& b = s.bound;
        if (b.cmd != cmd || b.serial != serial)
        {
            b        = Bound();
            b.cmd    = cmd;
            b.serial = serial;
        }

        if (b.pipeline != entry->pipeline)
        {
            d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry->pipeline);
            b.pipeline = entry->pipeline;
        }

        // A layout change makes every bound set and the push range stale.
        if (b.layout != layout.layout)
        {
            b.layout = layout.layout;
            b.set0   = VK_NULL_HANDLE;
            b.set1   = VK_NULL_HANDLE;
            b.set2   = VK_NULL_HANDLE;  // SXC:
            b.set3   = VK_NULL_HANDLE;
            b.have1  = false;
            b.pushed = false;
        }
        if (b.set0 != set0)
        {
            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, 0, 1, &set0, 0, nullptr);
            b.set0 = set0;
        }
        if (translated && (!b.have1 || b.set1 != set1 || b.offset1 != offset1))
        {
            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, 1, 1, &set1, 1, &offset1);
            b.set1    = set1;
            b.offset1 = offset1;
            b.have1   = true;
        }
        if (cover && b.set2 != set2)  // SXC:
        {
            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, 2, 1, &set2, 0, nullptr);
            b.set2 = set2;
        }
        if ((cover || lit) && b.set3 != set3)  // SXC:
        {
            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, 3, 1, &set3, 0, nullptr);
            b.set3 = set3;
        }

        VkBuffer     vbs[kMaxStreams + 1];
        VkDeviceSize offsets[kMaxStreams + 1];
        uint32_t     vbCount = draw.streamCount;
        for (uint32_t i = 0; i < draw.streamCount; ++i)
        {
            vbs[i]     = draw.streams[i].buffer;
            offsets[i] = draw.streams[i].offset;
        }
        // The constant stream rides along at binding streamCount whenever a pipeline of this layout
        // may read it; a pipeline that declares no such binding ignores the extra buffer.
        vbs[vbCount]     = s.constant.handle;
        offsets[vbCount] = 0;
        ++vbCount;
        bool same = b.vbCount == vbCount;
        for (uint32_t i = 0; same && i < vbCount; ++i) same = b.vbs[i] == vbs[i] && b.offsets[i] == offsets[i];
        if (!same)
        {
            d.CmdBindVertexBuffers(cmd, 0, vbCount, vbs, offsets);
            b.vbCount = vbCount;
            for (uint32_t i = 0; i < vbCount; ++i)
            {
                b.vbs[i]     = vbs[i];
                b.offsets[i] = offsets[i];
            }
        }

        if (!b.placed || std::memcmp(&b.viewport, &draw.viewport, sizeof draw.viewport) != 0)
        {
            d.CmdSetViewport(cmd, 0, 1, &draw.viewport);
            b.viewport = draw.viewport;
        }
        if (!b.placed || std::memcmp(&b.scissor, &draw.scissor, sizeof draw.scissor) != 0)
        {
            d.CmdSetScissor(cmd, 0, 1, &draw.scissor);
            b.scissor = draw.scissor;
        }
        b.placed = true;

        if (!b.biasSet || b.depthBias != draw.depthBias)
        {
            d.CmdSetDepthBias(cmd, draw.depthBias, 0.0f, 0.0f);
            b.depthBias = draw.depthBias;
            b.biasSet   = true;
        }
        if (!b.blendSet || std::memcmp(b.blendConstant, draw.blendConstant, sizeof draw.blendConstant) != 0)
        {
            d.CmdSetBlendConstants(cmd, draw.blendConstant);
            std::memcpy(b.blendConstant, draw.blendConstant, sizeof b.blendConstant);
            b.blendSet = true;
        }

        float push[kPushBytes / sizeof(float)];
        std::memcpy(push, draw.xform, sizeof draw.xform);
        std::memcpy(push + 16, draw.params, sizeof draw.params);
        std::memcpy(push + 20, draw.tex, sizeof draw.tex);
        std::memcpy(push + 36, draw.fog, sizeof draw.fog);  // SXC: fixed-function fog
        static_assert(sizeof push == (36 + 4) * sizeof(float), "Shaders/Push.glsl's layout");
        if (!b.pushed || std::memcmp(b.push, push, sizeof push) != 0)
        {
            d.CmdPushConstants(cmd, layout.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes, push);
            std::memcpy(b.push, push, sizeof push);
            b.pushed = true;
        }

        if (draw.indexed)
        {
            if (draw.indexBuffer == VK_NULL_HANDLE)
            {
                ++s.stats.skipped;
                return false;
            }
            if (b.ib != draw.indexBuffer)
            {
                d.CmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT16);
                b.ib = draw.indexBuffer;
            }
            if (draw.runs)
            {
                // SXC: the instanced grass's runs, one bind for all of them; step 7d: a run of copies
                // moves the copy stream to its entry.
                const bool copies = draw.copyStream >= 0 && static_cast<uint32_t>(draw.copyStream) < draw.streamCount;
                for (uint32_t r = 0; r < draw.runCount; ++r)
                {
                    const Run& run = draw.runs[r];
                    if (!run.indexCount || !run.instanceCount) continue;
                    if (copies)
                    {
                        const uint32_t      slot   = static_cast<uint32_t>(draw.copyStream);
                        const VkDeviceSize  offset = draw.streams[slot].offset + static_cast<VkDeviceSize>(run.copy) * 16u;
                        if (b.offsets[slot] != offset)
                        {
                            d.CmdBindVertexBuffers(cmd, slot, 1, &draw.streams[slot].buffer, &offset);
                            b.offsets[slot] = offset;
                        }
                    }
                    d.CmdDrawIndexed(cmd, run.indexCount, run.instanceCount, run.firstIndex, run.vertexOffset, run.firstInstance);
                }
            }
            else
                d.CmdDrawIndexed(cmd, draw.count, 1, draw.firstIndex, 0, 0);
        }
        else
        {
            d.CmdDraw(cmd, draw.count, 1, 0, 0);
        }
        ++s.stats.drawn;
        return true;
    }

    void GetStats(Stats& out) { out = s.stats; }
}
