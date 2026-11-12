// SXC: our point lights (port plan step 6; SXC/Lights). The pipelines that light per pixel carry a
// descriptor set of two storage buffers (Shaders/Lights.glsl): the grid and the lights, and per grid
// cell the indices of the lights that reach it; then the surface class, and the lights' cube shadow maps
// (Vulkan/Shadows, drawn at the same first use). One pair per frame slot, written at the frame's first
// use from what the provider hands over. Set 3 of our terrain pipelines (after the covers' set 2) and
// of the translated pixel modules flagged `lights` (models, WMOs; step 6b).
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Shadows.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::lights
{
    constexpr uint32_t kMaxLights  = 512;
    constexpr uint32_t kMaxPerCell = 32;
    constexpr uint32_t kMaxCells   = 64;

    struct Frame
    {
        bool        on = false;
        int         debug = 0;
        float       originX = 0.0f, originY = 0.0f, cellSize = 16.0f;
        uint32_t    cells = 0, perCell = 0, lightCount = 0;
        const void* lights = nullptr;       ///< 64 bytes each (Shaders/Lights.glsl's Light)
        const uint32_t* cellLights = nullptr; ///< cells x cells x perCell light indices, 0xFFFFFFFF ends a list
        float       viewInv[12] = {};       ///< the world scene's view back to the world, three rows (x, y, z; w the translation)
        const shadows::Job* shadowJobs = nullptr; ///< shadow maps to draw this frame (Vulkan/Shadows.hpp)
        uint32_t    shadowJobCount = 0;
    };

    /// Fills this frame's lights; false when they're off.
    using Provider = bool (*)(Frame& out);
    void SetProvider(Provider provider);

    /// Told how many of the frame's shadow jobs were drawn (the first ones), right after.
    using ShadowReport = void (*)(uint32_t drawn);
    void SetShadowReport(ShadowReport report);

    bool Init();
    bool Ready();

    /// The set's layout (null before Init()).
    VkDescriptorSetLayout SetLayout();

    /// Which lights a draw's surfaces take (binding 2's class; SXC::Lights::Surface).
    enum Surface : uint32_t
    {
        kSurfaceAny = 0,      ///< every light (models)
        kSurfaceOutdoor = 1,  ///< the lights outside (the terrain, outdoor WMO groups)
        kSurfaceIndoor = 2,   ///< the lights inside (indoor WMO groups)
        kSurfaceOff = 3,      ///< none: draws outside the world scene (portraits: another view)
        kSurfaceCount = 4,
    };

    /// This frame's set for slot `slot` and a surface class: the lights filled at the first call of
    /// the frame. Null when not ready.
    VkDescriptorSet FrameSet(uint32_t slot, uint64_t serial, uint32_t surface = kSurfaceAny);

    const char* Status();

    void Shutdown();
}
