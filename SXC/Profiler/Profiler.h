#pragma once

#include <cstdint>

// SXC's frame profiler (WXL's, ported; backlog "The profiler back"): per category of the frame -- the
// client's passes, our systems, and what mods register through sxc.profiler -- the CPU time, the GPU
// time, the draws and triangles, averaged over about a second, in the Client module's "Profiler" panel.
//
// CPU: scopes in the client's code (no hooks: we have the source), on the main thread. Times are
// exclusive: a nested category's time isn't counted again in the one around it.
//
// GPU: the graphics device's timestamps (Vulkan) each time the innermost category changes, read back
// when the frame slot comes round again (no waiting). The whole frame is one submit, so a GPU time is
// the GPU's work on that category, not a span with the CPU's gaps in it (WXL's D3D9 had those). The
// pre-pass (uploads, the point lights' shadow maps) runs ahead of the frame's draws and is timed apart.
namespace SXC::Profiler {
    // The client's own categories; mods register more (Register). The order is the panel's.
    enum Category : int {
        kOther,         // outside every category: the main loop, everything unlisted
        kGameUpdate,    // the update event: objects, Lua OnUpdate, the mods' updates
        kMods,          // the mods' update (wxl events), inside the game update
        kWorldUpdate,   // CGWorldFrame::OnWorldUpdate (units, missiles, world state)
        kInterface,     // the screen's paint around the world: frames, text, the glue
        kWorldRest,     // the world render, not in a more specific category
        kCulling,       // the world: portals, frustum, sort table, the scene's models sorted
        kAnimation,     // the M2 scene: time advanced, bones animated
        kTerrain,
        kWmo,           // the world map objects and their portals
        kSky,           // the sky dome, horizon, celestial bodies
        kEntities,      // models handed to the scene (CPU only: their draws are kModels)
        kModels,        // the M2 scene's passes (opaque and see-through)
        kGrass,         // detail doodads: the stock bake and our instanced grass
        kLiquids,
        kFog,           // our volumetric fog
        kWeather,
        kTransparent,   // the sorted see-through draw lists, particles, spell visuals
        kUnitShadows,   // the client's own: units drawn into their shadow textures (CShadowQuery)
        kGlow,          // the client's full-screen glow (FFX)
        kSceneObjects,  // the world's Gxu scene objects, drawn after the clear
        kPresent,      // the present pass and the tools overlay (ImGui); on the CPU also the acquire's wait
        kFrameCap,      // the CPU waiting for the frame rate cap (maxfps)
        // Ours, CPU only (their GPU work is in the pass they run in, or in the pre-pass).
        kLightsScan,    // our lights: the scene's models walked, merged, the nearest picked
        kLightsShadows, // our lights: the shadow assignment (the rest of it)
        kShadowChecks,  // our lights: the signature checks (has a light's surroundings changed?)
        kShadowGather,  // our lights: the triangles of the maps drawn this frame gathered
        kLightsGrid,    // our lights: the cells built
        kCoverFrame,    // our covers: the frame's data resolved for the terrain shader
        kGrassPrepare,  // our grass: chunks taken, runs built
        // The pre-pass (GPU only): recorded ahead of the frame's draws.
        kUploads,       // texture and buffer uploads, the descriptor work
        kShadowMaps,    // our point lights' cube shadow maps
        kBuiltinCount
    };

    // Categories past the client's: registered by mods.
    constexpr int kMaxCategories = 96;

    // A registered category's flags.
    enum Flags : uint32_t {
        kCpuOnly = 0x1,  // entered often or doing no GPU work: no timestamps (its GPU time goes to the one around it)
    };

    // A category for a name ("immersive-azeroth: fog field"); the same name returns the same id. -1 when
    // full.
    int Register(const char* name, uint32_t flags = 0);

    // Main thread; nothing while the profiler is off.
    void Enter(int category);
    void Leave(int category);

    struct Scope {
        int category;
        explicit Scope(int c) : category(c) { Enter(category); }
        ~Scope() { Leave(category); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    // A draw recorded for the current category.
    void CountDraw(uint32_t triangles);

    // The frame's end (GxScenePresent): closes the frame, folds it into the second's averages.
    void FrameEnd();

    // Whether this frame is being measured (a cheap check before work that only feeds the profiler).
    bool On();

    // --- the graphics device's side ---------------------------------------------------------------------

    // The device's timestamps: Mark() writes one into the frame's draws, closing the GPU category
    // before it (the device keeps which one that is). Null when the device goes away.
    struct Gpu {
        void (*mark)(int category);   // the GPU category changes to `category`
        void (*enable)(bool on);      // timestamps on or off (off: Mark does nothing)
        const char* (*status)();      // one line: supported, the period, frames read
    };
    void SetGpu(const Gpu* gpu);

    // A finished device frame's GPU times (when its slot comes round again): per category, ms. A read-back
    // splits a game frame into two device frames; both are summed into it.
    void GpuTimes(const double* ms, int count);

    // The panel (the Client module's "Profiler").
    void DrawPanel(void* user);
}
