#pragma once

#include <cstdint>

class CGxTex;
class CTexture;
class CM2Model;
class CM2Scene;

// SXC's point lights (port plan step 6): the sxc.lights service (SXC/Api/sxc_lights.h) holds what a mod
// sets; once per frame the lights are gathered from the M2 scene (the models' own point lights), merged,
// flickered, faded toward the range and listed per cell of a world grid around the camera; the Vulkan
// device pulls the result at its first terrain draw and every pixel adds its cell's lights.
namespace SXC::Lights {
    constexpr uint32_t kMaxPool = 512;
    constexpr uint32_t kMaxPerCell = 32;
    constexpr uint32_t kMaxCells = 64;
    constexpr uint32_t kNoLight = 0xFFFFFFFFu;

    // One light as the shaders read it (64 bytes, std430).
    struct GpuLight {
        float pos[3];        // world
        float invRadius;     // 1 / yd: exactly 0 light at the radius
        float color[3];      // x brightness x fade x flicker
        float falloff;       // the window's power: 1 smooth, higher tighter
        float spotDir[3];    // world, unit: where a spot points
        float cosOuter;      // spot: cos of the outer angle; -2 a point light (always inside)
        float spotScale;     // 1 / (cos inner - cos outer)
        float flags;         // kLightIndoor
        float shadow;        // its shadow map's slot + 1; 0 none
        float pad;
    };
    static_assert(sizeof(GpuLight) == 64, "the shaders read a light as four vec4");
    constexpr float kLightIndoor = 1.0f;   // GpuLight::flags: inside an indoor WMO group

    // Which lights a draw's surfaces take (the device binds a set per class).
    enum Surface : uint32_t {
        kSurfaceAny = 0,      // models: every light
        kSurfaceOutdoor = 1,  // the terrain, outdoor WMO groups: the lights outside
        kSurfaceIndoor = 2,   // indoor WMO groups: the lights inside
        kSurfaceOff = 3,      // another scene than the world's: none
    };

    // A run of a shadow job's vertices: solid (no texture), or alpha-keyed by a model's texture.
    struct ShadowPart {
        uint32_t firstVertex;
        uint32_t vertexCount;
        CGxTex* texture;
    };

    // A light's cube shadow map to draw this frame (the device's Vulkan/Shadows): triangles, three
    // vertices each (xyz relative to the light, then u, v), out to its radius, in parts.
    struct ShadowJob {
        uint32_t slot;
        float radius;
        const float* vertices;
        uint32_t vertexCount;
        const ShadowPart* parts;
        uint32_t partCount;
    };

    // What the device reads per frame (pointers valid until the next Current()).
    struct Frame {
        bool on = false;
        int32_t debug = 0;
        float originX = 0.0f, originY = 0.0f; // the grid's corner (world)
        float cellSize = 16.0f;
        uint32_t cells = 0;                   // per side (0: none)
        uint32_t perCell = 0;
        uint32_t lightCount = 0;
        const GpuLight* lights = nullptr;
        const uint32_t* cellLights = nullptr; // cell (x, y) at [(y * cells + x) * perCell ..]: light indices, kNoLight ends a list
        float viewInv[12] = {};               // the world scene's view back to the world: rows x, y, z (w the translation)
        const ShadowJob* shadowJobs = nullptr;
        uint32_t shadowJobCount = 0;
    };

    // The device's side: whether it draws our lights, its one-line status.
    using StatusFn = const char* (*)();
    void SetDevice(bool available, StatusFn status);

    // The device's shadow maps: how many lights, and how many vertices one frame's maps may hold (0: none).
    void SetShadowDevice(uint32_t slots, uint32_t maxVertices);

    // How many of this frame's shadow jobs the device drew (the first ones); the others' slots are redrawn later.
    void ShadowsDrawn(uint32_t drawn);

    // A slot drawn without some of its alpha-keyed parts (their textures weren't on the GPU yet): looked at
    // again soon, and redrawn.
    void ShadowIncomplete(uint32_t slot);

    // This frame's lights, gathered now. Main thread, once per frame (the device's first terrain draw).
    const Frame& Current();

    // Whether the stock point lights of the terrain chunks are to be left out (ours replace them).
    bool StockTerrainOff();

    // The seam in CM2Scene::Draw: while a scene other than the world's draws (portraits, the character
    // screen: another view), the device draws its models without our lights. Main thread.
    void SceneDrawBegin(const CM2Scene* scene);
    void SceneDrawEnd(const CM2Scene* scene);
    bool WorldDrawing();

    // The seam in CMapObj::RenderGroup: a WMO group's surfaces are indoor (MOGP 0x2000) or outdoor while
    // it draws. Main thread.
    void GroupDrawBegin(bool indoor);
    void GroupDrawEnd();

    // The seam in CM2SceneRender: the model being drawn (null: none). Its side is located like a light's
    // (cached per model, re-located when it moves or every few seconds).
    void SetDrawModel(const CM2Model* model);
    Surface ModelSurface(const CM2Model* model);

    // The seam in CM2SceneRender::SetupTextures: the batch's textures on stages 0 and 1 (null: none); the
    // glow of the model's rows that names one of them, for the next draws. ClearDrawGlow: none.
    void SetDrawGlow(const CM2Model* model, CTexture* const stages[2]);
    void ClearDrawGlow();

    // The device, for a draw: pixel rows 13 and 14 of the translated model shaders (8 floats; row 13's w
    // 0 when the draw doesn't glow). Main thread.
    const float* DrawGlow();
    bool DrawingModel();   // between SetDrawModel(model) and SetDrawModel(nullptr): the M2 scene's draws

    // The class of the draw being made now: a WMO group's side, else the model's being drawn, else any;
    // kSurfaceOff outside the world's scene; with the indoor test off, any.
    Surface DrawSurface();
}
