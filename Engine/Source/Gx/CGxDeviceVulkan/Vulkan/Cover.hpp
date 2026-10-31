// SXC: surface covers in our terrain shader (Shaders/Terrain.frag). The terrain pipelines carry a third
// descriptor set: the cover parameters and entries (a uniform buffer), the near and far maps (fallen
// snow, painted snow's kept share, wetness, the water line; RGBA32F, world-aligned and toroidal) and
// the cover textures: one array of 1024 indexed per pixel where the device can ("bindless": Vulkan 1.2,
// shaderSampledImageArrayNonUniformIndexing, the sampler limits), else 16 slots the client gives to the
// textures in view (WXL's way). One set per frame slot, written at the frame's first terrain draw, its
// uploads recorded into the frame's pre-pass.
//
// The data comes from SXC/Cover through the device (the provider); see orchestration docs/research/
// immersive-azeroth-port-2026-10-19.md, step 5b.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::cover
{
    constexpr uint32_t kMaxEntries  = 127;
    constexpr uint32_t kMaxTextures = 1024;   ///< texture IDs 1..
    constexpr uint32_t kBindlessSlots = 1024; ///< slots with per-pixel indexing (one per texture)
    constexpr uint32_t kFallbackSlots = 16;   ///< slots without it (the textures in view)
    constexpr uint32_t kMaxMap      = 256;
    constexpr uint32_t kMaxTrail    = 1024;   ///< trail layers: texels per side at most
    constexpr uint32_t kMaxSurface  = 1024;   ///< the surface grid's cells per side at most (step 5e)
    constexpr uint32_t kMarksBinding = 23;    ///< set 2: the ground marks (after the 16 fallback texture slots, in both modes)
    constexpr uint32_t kShoreFieldBinding = 24;   ///< set 2: the water's shore field (Vulkan/Water.hpp's Shore(); the swash)
    constexpr uint32_t kShoreCellsBinding = 25;   ///< set 2: its cells (the nearest water's surface)

    struct Frame
    {
        bool  on         = false;
        bool  fallenSnow = true;
        int   debug      = 0;
        float brightness = 0.93f, wetDarkening = 0.45f, thinDepth = 0.08f;
        float depthScale = 1.0f, breakupScale = 1.0f, driftScale = 1.0f, textureTile = 4.1666667f;
        float outside[3] = { 0.0f, 1.0f, 0.0f };   ///< beyond the far map: fallen snow, kept share, moisture above rest
        float tessDetail = 0.0f;                   ///< raised chunks (5c): yd per segment per yd of distance; 0 = flat
        float tessReach  = 250.0f;                 ///< yd: subdivided within this
        float tessMost   = 16.0f;                  ///< segments per edge at most

        const void* entries = nullptr;             ///< 64 bytes each (sxc_cover_entry): entry k + 1 at [k]
        uint32_t    entryCount = 0;

        // RGBA32F per cell, cell (i, j) at [mod(j, size) * size + mod(i, size)], valid for i in
        // [firstI, firstI + size); size 0 = none. Uploaded when the version changes.
        uint32_t     nearSize = 0;
        float        nearCell = 2.0f;
        int32_t      nearFirstI = 0, nearFirstJ = 0;
        const float* nearCells = nullptr;
        uint32_t     nearVersion = 0;
        uint32_t     farSize = 0;
        float        farCell = 33.333f;
        int32_t      farFirstI = 0, farFirstJ = 0;
        const float* farCells = nullptr;
        uint32_t     farVersion = 0;

        // SXC: the ground marks (burnt, frozen; SXC_MARK_*), toroidal like the maps; filled with the
        // covers off too (the grass's vertex stage reads them, set 2 binding kMarksBinding).
        uint32_t     marksSize = 0;
        float        marksCell = 1.0f;
        int32_t      marksFirstI = 0, marksFirstJ = 0;
        const float* marksCells = nullptr;
        uint32_t     marksVersion = 0;

        // Trails (step 5d): the fine and the coarse layer (RGBA32F, toroidal like the maps), and the
        // texel rectangles that changed (x, y, w, h each) unless all of it did.
        struct TrailLayer
        {
            uint32_t       size = 0;      ///< 0 = none
            float          cell = 1.0f;
            int32_t        firstI = 0, firstJ = 0;
            const float*   cells = nullptr;
            const int32_t* rects = nullptr;
            uint32_t       rectCount = 0;
            bool           full = true;
        };
        bool       trails = false;
        float      trailNow = 0.0f, trailPersist = 600.0f, trailSlur = 0.6f, trailRim = 1.0f;
        TrailLayer trailFine, trailCoarse;

        // The surface grid (step 5e): the terrain's pixels write the covers' surface there (RGBA32F per
        // cell at [mod(j, size) * size + mod(i, size)]: surface z, the cell's tag (i & 4095) * 4096 +
        // (j & 4095), the bare ground's z, 1 written), only cells in [first, first + size); remade when
        // the size changes. Read back each frame and handed to `surfaceSink` some frames later: the
        // device's own mapped buffer (the texels as they were; a tag that isn't the cell's own is a
        // stale or never-written cell), valid until the next call (cells null: none any more).
        bool        surface = false;
        uint32_t    surfaceSize = 0;
        float       surfaceCell = 0.25f;
        int32_t     surfaceFirstI = 0, surfaceFirstJ = 0;
        void      (*surfaceSink)(const float* cells, uint32_t size, float cell) = nullptr;

        // The texture slots: slot s (1..slotCount) holds texture slotTexture[s - 1] (0 none) as
        // slots[s - 1] (view null: not loaded, drawn plain); textureGloss[id - 1]: its alpha is a gloss mask.
        uint32_t           slotCount = 0;
        pipelines::Texture slots[kBindlessSlots];
        uint16_t           slotTexture[kBindlessSlots] = {};
        const bool*        textureGloss = nullptr;   ///< kMaxTextures, or null
    };

    /// Fills this frame's data; false when the covers are off (the set then says so).
    using Provider = bool (*)(Frame& out);
    void SetProvider(Provider provider);

    /// Makes the layout, the sets and the placeholders. False when the device can't (too few samplers
    /// per stage, a create failed): the terrain then stays on the stock shaders.
    bool Init();
    bool Ready();

    /// Whether the cover textures are one array indexed per pixel (a 1.2 device with non-uniform
    /// indexing and the limits for it), else 16 slots; and how many slots that makes.
    bool Bindless();
    uint32_t TextureSlots();

    /// Whether chunks can be raised onto their covers (step 5c): the device tessellates (and the
    /// modules came up); and the most segments per edge it allows (capped).
    bool Tessellation();
    float MaxTessLevel();

    /// Set 2's layout for the terrain pipelines (null before Init()).
    VkDescriptorSetLayout SetLayout();

    /// This frame's set for slot `slot`: written (and its maps uploaded) at the first call of the frame.
    /// Null when the frame can't be had.
    VkDescriptorSet FrameSet(uint32_t slot, uint64_t serial);

    /// One line: what the covers did last frame.
    const char* Status();

    /// Everything, with the device idle.
    void Shutdown();
}
