#pragma once

#include <cstdint>

class C3Vector;
class CDetailDoodadInst;
class CGxTex;

// SXC's instanced grass (port plan step 7). The client places the ground-effect plants of a chunk once
// (CMapChunk::CreateDetailDoodads) and bakes them into chunk-space vertices on the CPU whenever a
// batch's stream buffer lost its contents, which at long grass distances is nearly every frame. Here
// each chunk's plants become instances once (their placement as the stock bake computes it, in world
// space), each plant model's geometry is uploaded once, and every visible chunk's grass of one texture
// is drawn together, instanced (the device's Vulkan/Grass and SxcGrassDraw; the stock DetailDoodad
// vertex modules' instanced variants). The seam is CWorldScene::RenderDetailDoodads; a chunk the
// device can't take this frame (its pool is full) is drawn the stock way.
//
// Density (step 7d; GroundEffectDoodadDensity through the mod's provider, sxc.grass): each plant carries
// how many copies it wants, out to what radius from the player and how far they may stray. The chunks
// near the player draw their runs again once per copy; the vertex shader (Shaders/GrassDensity.glsl)
// turns and moves each copy within the plant's own terrain triangle (its plane gives the height) and
// grows it in with the distance, so nothing is rebuilt as the player moves.
namespace SXC::Grass {
    // One instanced draw: a plant model's indices, its plants' range of the pool; `copy` 0 the plants
    // themselves, 1.. their density copies (step 7d: the vertex shader places copy k of each plant).
    struct Run {
        uint32_t firstIndex;
        uint32_t indexCount;
        int32_t vertexOffset;
        uint32_t firstInstance;
        uint32_t instanceCount;
        uint32_t copy;
        float distance;   // yd from the player to the run's chunk (the mirror takes the near ones: SXC/View)
    };

    // What the device offers (null: no instanced grass on this renderer).
    struct Device {
        // A plant model's geometry (5 floats a vertex: position, texture coordinate), appended once per key.
        bool (*mesh)(uint32_t key, const float* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount, uint32_t& firstIndex,
                     int32_t& vertexOffset, uint32_t& indexCountOut);
        bool (*allocate)(uint32_t plants, uint32_t& first);
        void (*write)(uint32_t first, const void* plants, uint32_t count);
        void (*release)(uint32_t first, uint32_t count);
        // Draws runs with the client's grass state as it stands (the bound DetailDoodad shaders, their
        // constants) and `texture` at stage 0. False: nothing drawn.
        bool (*draw)(CGxTex* texture, const Run* runs, uint32_t count);
        uint32_t (*plantsInUse)();
    };
    void SetDevice(const Device* device);

    // Whether the instanced path draws this frame's grass ([Grass] Instanced, a device, the client's
    // grass shaders on).
    bool Active();

    // The seam in CWorldScene::RenderDetailDoodads, around its walk of the visible chunks (the client's
    // grass state already set): Begin; per chunk Take (true: ours, skip the stock draw); End draws them.
    void Begin();
    bool Take(CDetailDoodadInst* inst);
    void End();

    // A chunk's grass is going away (CDetailDoodadInst::Free): its plants go back to the pool.
    void Forget(CDetailDoodadInst* inst);

    // One line for the panels.
    const char* Status();

    // The wind the mod handed over this frame (sxc.grass set_motion) at the middle of its grid, around the
    // player: x and y (their length the strength, 0..1 and above in storms), gusts 0..1. False when the
    // mod sends none (our water's waves take a breeze then).
    bool WindAtPlayer(float& x, float& y, float& gust);

    // [Grass] Instanced (the client's Info panel switches it).
    bool Enabled();
    void SetEnabled(bool on);

    // [Grass] Placement and PaintedMin (step 7d; sxc.grass set_placement): how CMapChunk::CreateDetailDoodads
    // picks a cell's ground effect (SXC_GRASS_PLACE_*: the file's dominant layer, the paint's, or every
    // layer by its paint above the least share). Stock grass too; a change places every chunk's plants again.
    uint32_t Placement();
    float PaintedMin();

    // [Grass] SteepestGround and SlopeSink (step 7d; sxc.grass set_slopes): the least up part of the ground's
    // normal a plant may stand on (stock 0.4), and how far an upright plant is lowered on a slope (yd; 0
    // unless SlopeSink is set: the drop under its base's downhill edge x the factor).
    float SteepestGround();
    float SlopeSink(uint32_t doodadId, float scale, const C3Vector& normal);
}
