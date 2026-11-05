// SXC: our water's device side (SXC/Water; Shaders/Water.*): the scene behind the water. At the frame's
// first water draw the open pass ends (frame::BeginEffect), Shaders/WaterCapture.comp copies the target's
// colour and depth (as the view's w) into two images, and the pass resumes; the water draws read them
// through set 2 (with the viewport, to find their pixel in them): refraction, absorption by the water the
// view crosses, screen-space reflections.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Ripples.hpp"

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
        bool  dome = false;       ///< the sky's dome as drawn: each row's elevation (sine), the vertices' colours
        float domeRows[8] = {};
        float domeColours[122][4] = {};
        bool  mirror  = false;    ///< the mirror (Vulkan/View.cpp) is drawn at this plane (world z) this frame
        float mirrorZ = 0.0f;
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
        const uint16_t* flow    = nullptr;   ///< the rivers' flow on the same cells (SXC/Water/Flow.h): kShoreSize x kShoreSize x 4 halves; null: none
        uint32_t        flowVersion = 0;     ///< uploaded again when it changes
        int32_t         originX = 0;         ///< the window's first cell (global)
        int32_t         originY = 0;
        float           foam       = 1.0f;    ///< the shore's foam strength
        float           waveHeight = 0.12f;   ///< yd
        float           wavelength = 7.0f;    ///< yd
        float           reach      = 28.0f;   ///< yd: how far out the shore waves begin
        float           swash      = 0.5f;    ///< yd: how high the swash runs up the beach (Shaders/TerrainShore.glsl)
        float           caustics     = 1.0f;  ///< the caustics on the floor (TerrainShore.glsl): strength,
        float           causticScale = 2.5f;  ///< yd per cell,
        float           causticDepth = 6.0f;  ///< yd they reach
        float           sun[3] = { 0.0f, 0.0f, 1.0f };   ///< toward the sun (world, unit)
        float           clouds = 0.0f;        ///< how cloudy (the storm blend, 0..1): the caustics fade
        float           sunColour[3] = { 1.0f, 1.0f, 1.0f };   ///< the sun's direct light (the models' caustics)
        float           underGlow[4] = {};   ///< the underwater medium's glow (rgb), w 1 while it draws: the deep the surface mirrors from below
        int             causticDebug = 0;     ///< the terrain's caustics debug view (0 off)
    };

    /// The shore field as the terrain reads it (Vulkan/Cover.cpp binds it to set 2: the swash on the
    /// beach): the images, and the values the water draws took at its last capture. `valid` while a
    /// field was captured in the last few frames.
    struct ShoreShared
    {
        bool        valid = false;
        VkImageView field = VK_NULL_HANDLE, cells = VK_NULL_HANDLE;   ///< both in VK_IMAGE_LAYOUT_GENERAL
        VkSampler   fieldSampler = VK_NULL_HANDLE, cellsSampler = VK_NULL_HANDLE;
        float       grid[4] = {};   ///< as WaterScene.shoreGrid
        float       look[4] = {};   ///< as WaterScene.shoreLook
        float       wind[4] = {};   ///< the waves' direction (x, y), the wind speed, the water's clock (s)
        float       swash = 0.0f;
        /// The long waves' displacement (cascade 0 alone, a 2D view; GENERAL), so the terrain's waterline
        /// rises and falls with them: (movement x wave scale of the player's water, 1 / the cascade's size
        /// (1 / yd), 1 when the waves run, 0), and the camera (their movement fades with its distance).
        VkImageView waves = VK_NULL_HANDLE;
        VkSampler   wavesSampler = VK_NULL_HANDLE;
        float       wave[4] = {};
        float       camera[4] = {};
        float       caustic[4] = {};   ///< strength, yd per cell, yd they reach, 0
        float       sun[4] = {};       ///< toward the sun (world, unit), how cloudy (0..1)
        float       sunColour[4] = {}; ///< the sun's direct light (rgb), 0
        /// The short waves' slopes (cascade 2 alone, a 2D view; GENERAL): the caustics follow them.
        VkImageView ripples = VK_NULL_HANDLE;
    };

    ShoreShared Shore();

    /// This frame's set 2 for reading how the surface moves (the waves, the shore field, the parameters:
    /// WaterMove.glsl), once the frame's water work ran, whether or not the scene was copied; null before the
    /// first copy ever. `time` and `look` are WaterMove.glsl's clock and movement row without a look grid.
    VkDescriptorSet MoveSet(uint32_t slot, uint64_t serial, float& time, float look[4]);

    /// The surface under the swimmers (Shaders/WaterQuery.comp; SXC/Water/Ride.cpp): points asked at a
    /// capture are answered when its frame slot comes round again, through `sink` with the same `tag`.
    constexpr uint32_t kQueryPoints = 64;

    struct QueryInput
    {
        /// Called at the frame's first capture only: fills up to `max` points (world x, y, the still surface's
        /// z, unused) and their tag; returns how many.
        uint32_t (*ask)(float (*points)[4], uint32_t max, uint64_t* tag) = nullptr;
        float       camera[4] = {};                       ///< the camera's world position, the water's clock (s)
        float       look[4] = { 0.0f, 1.0f, 0.0f, 1.0f };  ///< the movement row without a look grid
        /// Per point: the surface's rise (yd), its slope (x, y), 1 when answered.
        void (*sink)(uint64_t tag, const float (*results)[4], uint32_t count) = nullptr;
    };

    struct CaptureInput
    {
        VkViewport viewport = {};      ///< the draws' viewport on the target (pixels, depth range)
        float      proj[16] = {};      ///< the native projection, row-major (row vectors)
        WaveInput  waves;
        LookInput  looks;
        ShoreInput shore;
        QueryInput query;
        ripples::Input ripples;            ///< the ripple field's step this frame (Vulkan/Ripples.hpp)
        float      rippleStrength = 1.0f;  ///< x the field's slopes on the water
        float      rippleDistance = 45.0f; ///< yd from the camera the ripples show (fading over the last 30%)
        bool       prepareOnly = false;   ///< the frame's water work without the scene copy (no water drawn yet)
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
