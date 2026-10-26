// SXC: volumetric fog. A froxel volume over the world viewport (160 x 90 columns, 64 slices by the
// distance from the camera out to `far`), filled and integrated front to back by one compute shader
// from a fog medium (a near field of light layers and pools around the player, a haze), then laid over
// the scene in one full-screen pass that reads the scene depth: dst x transmittance + scattered light.
//
// Recorded between the engine's draws (SXC::Fog's seam in the world render: after the opaque world and
// the water, before the weather and the see-through models), through frame::BeginEffect(). Not in the
// original; see orchestration docs/research/immersive-azeroth-port-2026-10-19.md, step 4.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::fog
{
    struct Frame
    {
        float      viewProj[16] = {};  ///< view x the native projection (row vectors), relative to `origin`
        float      origin[3]    = {};  ///< the camera's world position
        float      seconds      = 0.0f;
        VkViewport viewport     = {};  ///< the world's, in pixels of the target, with its depth range
        float      colour[3]    = {};  ///< the fog's lit colour

        // The near field: a world-aligned toroidal grid, cell (i, j) (i = floor(x / cellSize)) at texel
        // (i mod size, j mod size), valid for i in [firstI, firstI + size). RGBA16F per texel; null =
        // no field. Uploaded when `fieldVersion` changes.
        uint32_t        fieldSize    = 0;
        float           cellSize     = 2.0f;
        int32_t         firstI       = 0, firstJ = 0;
        const uint16_t* light        = nullptr; ///< density (1/yd), layer base (world z), thickness (yd), patchiness
        const uint16_t* pool         = nullptr; ///< density (1/yd), pool top (world z), soft top (yd), 0
        const uint16_t* tint         = nullptr; ///< the fog's colour per cell (rgb, x tintBrightness), 0; null: colour everywhere
        float           tintBrightness = 1.0f;
        bool            fieldReady   = false;   ///< the pool layer's w: how much the near field shows per cell (else 1)
        uint32_t        fieldVersion = 0;

        // The far map: the same layout, coarse; null = none. Uploaded when `farVersion` changes.
        uint32_t        farSize      = 0;
        float           farCellSize  = 33.333f;
        int32_t         farFirstI    = 0, farFirstJ = 0;
        const uint16_t* farLayer     = nullptr; ///< density (1/yd), floor (world z), thickness (yd), 0
        const uint16_t* farTint      = nullptr; ///< colour (rgb, x tintBrightness), 0; null: colour everywhere
        uint32_t        farVersion   = 0;

        // The wake map around the player; null = none. Uploaded when `wakeVersion` changes.
        uint32_t        wakeSize     = 0;
        float           wakeTexel    = 1.0f;    ///< yd
        float           wakeOrigin[2] = {};     ///< world x, y of texel (0, 0)'s corner
        const uint16_t* wake         = nullptr; ///< push (rim < 0), its top (world z), displacement x, y (yd)
        uint32_t        wakeVersion  = 0;

        float densityScale  = 1.0f;
        float noiseAmount   = 0.6f;
        float noiseTile     = 64.0f;   ///< yd
        float drift[2]      = {};      ///< yd the patches have drifted so far
        float stepLength    = 0.5f;    ///< yd: the slices are integrated in steps this long (at most 32 per slice)
        float nearDistance  = 0.5f;    ///< yd
        float farDistance   = 250.0f;  ///< yd
        float hazeDensity   = 0.0f;    ///< per yd at the haze's base
        float hazeBase      = 0.0f;    ///< world z
        float hazeThickness = 30.0f;   ///< yd: e-folding height above the base
        int   debug         = 0;       ///< 1: magenta
        bool  perPixel      = true;    ///< march each pixel's ray (sharp thin layers); false: the froxel volume
        int   maxSteps      = 160;     ///< per pixel: the most steps a ray takes
        float handover      = 16.0f;   ///< yd: the near field fades into the far map over this band

        // Our lights in the fog (Vulkan/Lights' set, FogLights.comp): a light volume over the viewport.
        float lightStrength = 0.0f;    ///< x their light in the fog; 0 off
        float lightGlow     = 0.3f;    ///< forward scattering (Henyey-Greenstein g, -0.9 .. 0.9): brighter looking toward a lamp
        float lightRange    = 96.0f;   ///< yd: the light volume's reach from the camera
    };

    /// Records the fog over the current target. False when nothing was drawn; Status() says why.
    bool Record(const Frame& frame);

    /// One line: what the fog did last, or why it didn't.
    const char* Status();

    /// Everything, with the device idle (before frame::Destroy()).
    void Shutdown();
}
