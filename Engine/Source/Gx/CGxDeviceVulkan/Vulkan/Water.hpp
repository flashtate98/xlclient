// SXC: our water's device side (SXC/Water; Shaders/Water.*): the scene behind the water. At the frame's
// first water draw the open pass ends (frame::BeginEffect), Shaders/WaterCapture.comp copies the target's
// colour and depth (as the view's w) into two images, and the pass resumes; the water draws read them
// through set 2 (with the viewport, to find their pixel in them): refraction, absorption by the water the
// view crosses, screen-space reflections.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::water
{
    /// The waves (step 3: Shaders/WaterWaves.glsl): an FFT sea in three cascades, recorded into the
    /// frame's pre-pass with the copy. The spectrum is made again when the wind changes.
    struct WaveInput
    {
        bool  on        = false;
        float time      = 0.0f;   ///< seconds, wrapping every `period`
        float period    = 600.0f;
        float windX     = 1.0f;   ///< the wind's direction (unit)
        float windY     = 0.0f;
        float speed     = 6.0f;   ///< yd / s
        float spread    = 0.3f;   ///< 0 all with the wind .. 1 every way
        float amplitude = 0.35f;  ///< x the spectrum's heights
        float chop      = 1.2f;   ///< horizontal displacement (sharper crests)
        float foam      = 1.0f;   ///< the foam's strength where crests fold
        bool  rebuild   = false;  ///< make the spectrum again even if the wind hasn't changed
        float move      = 1.0f;   ///< x the surface's displacement near the camera (0 flat)
        uint32_t bodies = 0;      ///< the sky's bodies reflected: direction (unit), angular radius (rad), colour
        float body[4][7] = {};
    };

    /// The look grid (SXC/Water/Surface.h): per terrain chunk around the player an index into a palette of
    /// looks, which Water.vert blends between chunk centres.
    constexpr uint32_t kLookGrid    = 64;   ///< chunks per side
    constexpr uint32_t kLookPalette = 64;   ///< looks at most
    constexpr uint32_t kLookFloats  = 20;   ///< per look (five vec4s)

    struct LookInput
    {
        const uint8_t* index   = nullptr;   ///< kLookGrid x kLookGrid, row-major (chunk y, then x); null: none
        const float*   palette = nullptr;   ///< count x kLookFloats
        uint32_t       count   = 0;
        int32_t        originX = 0;         ///< the chunk at index 0 (the client's counting)
        int32_t        originY = 0;
    };

    /// The shore field (SXC/Water/Shore.h): per cell (a sixteenth of a chunk) the signed shore distance,
    /// the depth and the direction to the shore, as half floats, kept toroidally around the player.
    constexpr uint32_t kShoreSize = 256;

    struct ShoreInput
    {
        const uint16_t* texels  = nullptr;   ///< kShoreSize x kShoreSize x 4; null: none
        const float*    cells   = nullptr;   ///< kShoreSize x kShoreSize x 4 floats: surface, ground, type, covered
        uint32_t        version = 0;         ///< uploaded again when it changes
        int32_t         originX = 0;         ///< the window's first cell (global)
        int32_t         originY = 0;
        float           foam       = 1.0f;    ///< the shore's foam strength
        float           waveHeight = 0.12f;   ///< yd
        float           wavelength = 7.0f;    ///< yd
        float           reach      = 28.0f;   ///< yd: how far out the shore waves begin
    };

    struct CaptureInput
    {
        VkViewport viewport = {};      ///< the draws' viewport on the target (pixels, depth range)
        float      proj[16] = {};      ///< the native projection, row-major (row vectors)
        WaveInput  waves;
        LookInput  looks;
        ShoreInput shore;
    };

    /// The copy, once per frame (later calls the same frame answer as the first). False: nothing to copy
    /// over yet, or the device can't; the water draws the stock way this frame.
    bool Capture(const CaptureInput& in, bool* first = nullptr);

    /// The cascades' world sizes (yd), for the draws.
    constexpr float kWaveSizes[3] = { 256.0f, 48.0f, 9.0f };

    /// Set 2 of the water pipelines (made on first need; null when it can't be).
    VkDescriptorSetLayout SetLayout();

    /// This frame's set 2, or null when the scene wasn't copied this frame.
    VkDescriptorSet FrameSet(uint32_t slot, uint64_t serial);

    const char* Status();

    /// After the device is idle.
    void Shutdown();
}
