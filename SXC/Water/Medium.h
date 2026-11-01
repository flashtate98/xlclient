// The underwater medium (kb: underwater): with the camera in water drawn by our shader, the view loses its
// colour channels with distance (red first), goes grey, and fades into the water's glow; the light from
// above dims and tints with depth. Ours instead of the stock underwater fog (one colour, every channel at
// the same rate) and the stock darkening by LiquidType MaxDarkenDepth.
//
// Per place, depth keyframes in the UnderwaterMedium table (DBFilesClient\UnderwaterMedium.cdbc): rows with
// a LiquidType (0 every water), a Scope (LiquidSurface's rules: empty everywhere, a map, an area, a tile)
// and a Depth (yd below the water's surface). The most specific Scope with rows owns the curve; its rows
// are blended by depth. Without rows, the defaults below (the stock look plus the colour shift).
#pragma once

#include <cstdint>

namespace SXC::Water {
    // What the device draws (Vulkan/Medium.hpp's Frame without the view).
    struct MediumFrame {
        float loss[3] = {};       // per yd: log2 of each channel's loss
        float glow[3] = {};       // what far things fade into
        float desaturate = 0.0f;  // yd until grey; 0 never
        float fade = 0.0f;        // per yd
        int debug = 0;            // 1 the fade, 2 distance bands, 3 the lamps in the water alone
        float origin[3] = {};     // the camera's world position
        float halo = 0.0f;        // our lamps lighting the water: per yd scattered (0 off)
        float haloRange = 96.0f;  // yd: how far out their light in the water is gathered
        float haloGlow = 0.5f;    // forward scattering (Henyey-Greenstein g)
        // The waterline: the moving surface relative to the camera (yd; far above: all under, far below: all
        // dry), its slopes dz/dx, dz/dy, 1 = on; the lens: seconds, how wet (1 just surfaced).
        float line[4] = { 1000.0f, 0.0f, 0.0f, 1.0f };
        float lens[2] = {};
        float still = 0.0f;       // the still surface at the camera (world z)
    };

    using MediumFn = bool (*)(const MediumFrame& frame);
    using MediumStatusFn = const char* (*)();

    // The Vulkan device's pass (null when it goes).
    void SetMediumRenderer(MediumFn render, MediumStatusFn status);

    // Registers the table (SXC::Startup).
    void RegisterMedium();

    // DayNight::SetColors: true while the medium takes the light under water, so the stock darkening by
    // depth is skipped.
    bool MediumTakesLight();

    // World's lighting update, after DayNight::UpdateFog: this frame's medium for the camera; the stock fog
    // pushed out past the medium's visibility (it also sets how far the world draws), the sun and ambient
    // dimmed and tinted by the light from above.
    void MediumLighting();

    // CWorldScene::UpdateViewerLiquid: true while the camera is less than the waves' recent reach (and half a
    // yard) below the still surface (terrain water, the waterline on): the client then treats the camera as
    // above water (its sky, fog, light and render order the air's) and the medium draws the water's part of
    // the view, by the surface as drawn.
    bool WaterlineHoldsAir(float cameraZ);

    // The world render, after the water's surface (under water, and above it while the camera is near the
    // surface or the lens is wet): the medium over the opaque world.
    void MediumComposite();

    // The medium's glow this frame (rgb, w 1), what the deep looks like far off; false (w 0) while it doesn't
    // draw. The water's surface mirrors it from below where its own search finds nothing.
    bool MediumGlow(float out[4]);

    // The Water module's "Underwater" panel.
    void DrawMediumPanel(void* user);
}
