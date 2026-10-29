// Our water's look per water (kb: water-shader): the LiquidSurface table (DBFilesClient\LiquidSurface.cdbc).
// A row per LiquidType (0: every water) and place (Scope: empty everywhere, a map's folder name, an ADT's
// "Map_x_y"); the most specific row for a water wins, its type's before the "every water" row. Without a
// row, the defaults below (the first tuning, Seyri 2026-10-28).
#pragma once

#include <cstdint>

namespace SXC::Water {
    // How a row's colour treats the stock one (the light data's shallow and deep water colour, through
    // the day): kept, its brightness kept with our hue, or replaced.
    enum class ColourMode : int32_t { Stock = 0, Hue = 1, Replace = 2 };

    struct Look {
        // The water.
        float clarityClear = 0.01f;   // absorption per yd where the stock tint's alpha is 0 ..
        float clarityMurky = 0.12f;   // .. and where it is 1
        float scatter = 0.65f;        // the water's own colour, x the tint
        float refraction = 0.6f;      // how far the waves bend the scene behind
        ColourMode colourMode = ColourMode::Stock;
        float shallow[3] = { 0.3f, 0.7f, 0.7f };   // the colour at the shore (Hue / Replace)
        float deep[3] = { 0.05f, 0.2f, 0.35f };    // and a few yards down
        // The surface.
        float reflectivity = 0.02f;   // Fresnel looking straight down (real water 0.02)
        float reflection = 1.0f;      // x the reflection
        float window = 0.3f;          // Snell's window from below (cos; real water 0.66)
        float glint = 1.5f;           // the sky bodies' highlight
        float glintSharpness = 600.0f;
        // The waves: the sea's spectrum is one (the row of the water the player is in), each water scales it.
        float waveScale = 1.0f;       // x the shared sea, this water (shading and movement)
        float amplitude = 0.35f;      // the sea's height
        float chop = 1.2f;
        float spread = 0.3f;
        float foam = 1.0f;
        float foamThreshold = 0.95f;  // the Jacobian below which crests foam
        float move = 1.0f;            // the surface's displacement (0 flat)
    };

    // Registers the table (SXC::Startup).
    void RegisterSurface();

    // The look of a LiquidType where the player is (cached; follows the table's edits).
    const Look& SurfaceLook(uint32_t liquidType);

    // The LiquidType of a liquid instance's settings (the settings bank is indexed by type); 0 when unknown.
    uint32_t LiquidTypeOf(const void* settings);

    // The LiquidType the player stands or swims in, 0 when none.
    uint32_t LiquidTypeHere();

    // The Water panel's buttons: the row for a LiquidType everywhere (Scope empty), made if missing, set
    // from `look`; false when the table isn't there. Saving is the Data view's.
    bool WriteSurfaceRow(uint32_t liquidType, const Look& look);
}
