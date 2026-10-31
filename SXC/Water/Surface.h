// Our water's look per water (kb: water-shader): the LiquidSurface table (DBFilesClient\LiquidSurface.cdbc).
// A row per LiquidType (0: every water) and place (Scope: empty everywhere, a map's folder name, an area's
// name or "area:<ID>", an ADT's "Map_x_y"); the most specific row for a water wins, its type's before the
// "every water" row: a tile over an area (a sub-area over its zone) over a map over everywhere. Without a
// row, the defaults below (the first tuning, Seyri 2026-10-28).
//
// Terrain water reads its look per position: the look grid (64 x 64 terrain chunks around the player, an
// index per chunk into a short palette of looks), which the water's vertex stage blends between chunk
// centres, so looks fade into each other over a chunk (~33 yd) where areas meet.
#pragma once

#include <cstdint>
#include <string>

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
        // The shore: one for every water at a time, from the row of the water the player is in (like the
        // sea's spectrum; the shore field and its waves are shared).
        float shoreFoam = 1.0f;
        float shoreWaveHeight = 0.12f;   // yd
        float shoreWavelength = 7.0f;    // yd
        float shoreReach = 28.0f;        // yd: how far out the shore waves begin
        float swash = 0.5f;              // yd: how high the swash runs up the beach
        // Caustics on the floor under the water (the terrain draws them), from the row of the water the
        // player is in like the shore's.
        float caustics = 1.0f;           // strength (0 none: murky water)
        float causticScale = 2.5f;       // yd: the pattern's cells
        float causticDepth = 6.0f;       // yd: how deep they reach (they fade over this)
    };

    // A look as the water shaders read it: five vec4s (Water.vert's look, Water.cpp's p9..p13).
    constexpr uint32_t kPackedLook = 20;
    void PackLook(const Look& look, float out[kPackedLook]);

    // The look grid (SurfaceGrid): `index` per chunk (row-major, y then x; chunk x runs along world -y and
    // chunk y along world -x, as the client counts them) into `palette`.
    constexpr int32_t kGridSize = 64;
    constexpr uint32_t kPaletteSize = 64;

    struct LookGrid {
        bool valid = false;
        int32_t originX = 0, originY = 0;   // the chunk at index 0
        uint8_t index[kGridSize * kGridSize] = {};
        uint32_t paletteCount = 0;
        float palette[kPaletteSize][kPackedLook] = {};
    };

    // Registers the table (SXC::Startup).
    void RegisterSurface();

    // The look grid around the player (rebuilt twice a second, and when the table changes).
    const LookGrid& SurfaceGrid();

    // The look of a LiquidType at a world point (its chunk's area and tile, from the grid; the player's
    // place outside it).
    const Look& SurfaceLookAt(uint32_t liquidType, float x, float y);

    // The LiquidType of a liquid instance's settings (the settings bank is indexed by type); 0 when unknown.
    uint32_t LiquidTypeOf(const void* settings);

    // The LiquidType the player stands or swims in, 0 when none.
    uint32_t LiquidTypeHere();

    // The places a row can name where the player is (the Water panel's "Write" choice).
    enum class ScopeKind : int32_t { Everywhere = 0, Map = 1, Zone = 2, SubArea = 3, Tile = 4 };

    // The Scope text for a kind of place here ("" when there is none, e.g. no sub-area), and a label for it.
    bool ScopeHere(ScopeKind kind, std::string& scope, std::string& label);

    // The Water panel's buttons: the row for a LiquidType and Scope, made if missing, set from `look`;
    // false when the table isn't there. Saving is the Data view's.
    bool WriteSurfaceRow(uint32_t liquidType, const std::string& scope, const Look& look);

    // The player's position in the world (false when not in the world).
    bool PlayerPosition(float& x, float& y);
}
