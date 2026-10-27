#pragma once

#include <cstdint>
#include <vector>
#include "Engine/Source/Tempest/C3Vector.h"

class CAaBox;
class C44Matrix;
class CGxTex;
class CMapRenderChunk;

// SXC's surface covers: the sxc.cover service (SXC/Api/sxc_cover.h) holds what a mod sets (the entries,
// the cover textures, the maps, the look, the resolver); a seam in the terrain chunk draw hands each
// chunk's layer entries to the terrain shader; the Vulkan device pulls the rest once per frame.
namespace SXC::Cover {
    constexpr uint32_t kMaxEntries = 127;
    constexpr uint32_t kMaxTextures = 1024;
    constexpr uint32_t kMaxSlots = 1024;     // texture slots the shader can sample at once (the device says how many it has)

    // What the device reads per frame (pointers valid until the next service call; the frame reads
    // them right away).
    struct Frame {
        bool on = false;                  // a mod turned it on
        bool fallenSnow = true;
        int32_t debug = 0;
        float brightness = 0.93f, wetDarkening = 0.45f, thinDepth = 0.08f;
        float depthScale = 1.0f, breakupScale = 1.0f, driftScale = 1.0f, textureTile = 33.3333333f / 8.0f;
        float outside[3] = { 0.0f, 1.0f, 0.0f };
        float tessDetail = 0.0f, tessReach = 250.0f, tessMost = 16.0f; // depth (sxc_cover_look::tess_*); detail 0 = flat

        const void* entries = nullptr;    // sxc_cover_entry[entryCount]: entry k + 1 at [k]
        uint32_t entryCount = 0, entryVersion = 0;

        // The maps (sxc_cover_map's layout, RGBA32F per cell); size 0 = none.
        uint32_t nearSize = 0;
        float nearCell = 2.0f;
        int32_t nearFirstI = 0, nearFirstJ = 0;
        const float* nearCells = nullptr;
        uint32_t nearVersion = 0;
        uint32_t farSize = 0;
        float farCell = 33.333f;
        int32_t farFirstI = 0, farFirstJ = 0;
        const float* farCells = nullptr;
        uint32_t farVersion = 0;

        // Trails (SXC/Cover/Trails.h): the fine and the coarse layer, with what changed since last frame
        // (texel rectangles; or all of it).
        struct TrailLayer {
            uint32_t size = 0;
            float cell = 1.0f;
            int32_t firstI = 0, firstJ = 0;
            const float* cells = nullptr;
            const int32_t* rects = nullptr; // x, y, w, h per rectangle
            uint32_t rectCount = 0;
            bool full = true;
        };
        bool trails = false;
        float trailNow = 0.0f, trailPersist = 600.0f, trailSlur = 0.6f, trailRim = 1.0f;
        TrailLayer trailFine, trailCoarse;

        // The surface grid (step 5e): the window around the player the terrain writes the covers'
        // surface into (surfaceSize cells of surfaceCell yd: 96 yd across, [Terrain] CoverSurfaceFine);
        // read back through TakeSurface().
        bool surface = false;
        uint32_t surfaceSize = 0;
        float surfaceCell = 0.25f;
        int32_t surfaceFirstI = 0, surfaceFirstJ = 0;

        // The textures with a slot this frame: slot s (1..slotCount) shows texture slotTexture[s - 1]
        // (an ID, 1..) through slotGx[s - 1] (loaded); textureGloss[id - 1]: its alpha is a gloss mask.
        uint32_t slotCount = 0;
        uint16_t slotTexture[kMaxSlots] = {};
        CGxTex* slotGx[kMaxSlots] = {};
        bool textureGloss[kMaxTextures] = {};
    };

    // The device's side: whether it can draw covers, how many texture slots its shader has, its one-line
    // status.
    using StatusFn = const char* (*)();
    void SetDevice(bool available, uint32_t textureSlots, StatusFn status);

    // This frame's data (the texture slots assigned now). Main thread.
    const Frame& Current();

    // The device's read-back of the surface grid (some frames old): per cell the surface's z, the cell's
    // tag, the bare ground's z, 1 written (gxvk::vk::cover::Frame). Read in place: the device's buffer,
    // valid until the next call (null: none any more). Main thread.
    void TakeSurface(const float* cells, uint32_t size, float cell);

    // The covers' surface at a world point as the terrain last drew it (a few frames old), and the bare
    // ground's height there. False where the terrain wasn't drawn near the player lately (not in view, no
    // covers): the stock height then.
    bool SurfaceAt(float x, float y, float& surfaceZ, float* groundZ = nullptr);

    // Effects hanging at a unit's feet (attachment 19, Base: paladin auras, spells' effects there) onto
    // the covers (step 5e, WXL's rule): `pose` (an attached model's context, view space: the parent's bone
    // matrix at the attachment) lifted by as far as its origin lies below the covers' top around it (the
    // ground under it plus the thickest cover within 1.5 yd: the unit's own footprint doesn't pull it in,
    // a slope's uphill ground doesn't push it up); one above it (jumping, flying) stays. `view` / `viewInv`
    // the scene's. Any thread (it reads the frame's grid).
    void LiftAtFeet(C44Matrix& pose, const C44Matrix& view, const C44Matrix& viewInv);

    // The camera kept above the covers' surface (step 5e): a position inside the cover layer (below its
    // surface, not below the bare ground: a cave under a snowy hill stays) is raised to 0.3 yd above it.
    void KeepCameraAbove(C3Vector& position);

    // The client's projected textures (selection and spell target circles, blob shadows, models'
    // projected batches like Consecration) on the raised covers (step 5e): the stock draw lays them on
    // the terrain's flat triangles, under the cover. Between DecalBegin and DecalEnd the client draws
    // the decal's box with the decal's own state, and the device shades it onto the terrain it drew this
    // frame where the cover is raised (Shaders/Decal.frag). `kind`: the decal's coordinates come from
    // the fixed-function texgen and texture matrices (0), or from a models' projected batch's vertex
    // constants c6..c9 (1); `colour` its colour (rgba 0..1).
    struct Decal {
        int kind = 0;
        float colour[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        bool debug = false;   // the covers' debug view 7: the box painted by what the decal finds (Shaders/Decal.frag)
    };
    bool DecalsOnCover();   // the covers are drawn on a device that can
    void DecalBegin(int kind, float r, float g, float b, float a);
    void DecalEnd();
    const Decal* CurrentDecal();   // the device's question at a draw: null outside DecalBegin / DecalEnd

    // The seam: CMapRenderChunk::RenderSinglePassVertexPixelShader, before the draw. Sets pixel
    // constants c11 (each layer's painted entry), c12 (each layer's fallen-snow look), c13.x (1: the
    // chunk can carry depth, so the device raises it) and c13.y (its liquids' lowest top: the water line
    // beyond the near map), asking the mod's resolver the first time a chunk is met, and counts the
    // textures the chunk shows.
    void BeforeChunkDraw(CMapRenderChunk* chunk);

    // The steady ground wind the mod sent with the look (0 calm .. 1 strong; 0 without covers or from
    // an older mod): covers level sooner in it (the terrain shader's pattern fade).
    float SteadyWind();
}
