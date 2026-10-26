/*
 * sxc.fog: volumetric fog drawn by the client from a medium a mod describes. The client renders (a
 * froxel volume over the view, laid over the scene after the opaque world and the water, before the
 * weather and the see-through models); the mod decides where the fog is and how it looks.
 *
 * The medium: a near field around the player (a grid of cells, each with a light layer that thins out
 * upward and a flat-topped pool), a coarse far map beyond it, a haze everywhere; a wake map where
 * units push the fog aside. Vulkan only: on another renderer available() answers false and the
 * client's own fog stays alone.
 */

#ifndef SXC_API_FOG_H
#define SXC_API_FOG_H

#include "SXC/Api/sxc.h"

#define SXC_FOG_SERVICE         "sxc.fog"
#define SXC_FOG_SERVICE_VERSION 1

#define SXC_FOG_MAX_FIELD 512
#define SXC_FOG_MAX_FAR   256
#define SXC_FOG_MAX_WAKE  256

#ifdef __cplusplus
extern "C" {
#endif

/// The near field: a world-aligned toroidal grid. Cell (i, j) of the world (i = floor(x / cell_size),
/// j = floor(y / cell_size)) is stored at [mod(j, size) * size + mod(i, size)], 4 floats each; only
/// cells with i in [first_i, first_i + size) (same for j) are drawn, fading out over the last 8.
typedef struct sxc_fog_field
{
    uint32_t     struct_size;
    uint32_t     size;      ///< cells per side, 1 .. SXC_FOG_MAX_FIELD
    float        cell_size; ///< yards
    int32_t      first_i, first_j;
    /// Per cell: extinction per yard at the layer's floor (0 = no fog), the floor (world z: full below
    /// it), the thickness (yards: above the floor it thins by e over this height; negative: the cell
    /// isn't ready, the far map shows there), patchiness 0..1 (how much the noise breaks it up).
    const float* light;
    /// Per cell: extinction per yard (0 = no pool), the top (world z; flat), the soft band under the
    /// top (yards), unused. May be null.
    const float* pool;
    /// Per cell: the fog's colour there (rgb, may exceed 1), unused. May be null: the look's colour
    /// everywhere. Multiplied by the look's brightness.
    const float* colour;
    // --- appended 2026-10-20 ---
    /// Per cell (one float each): how much the near field shows there, 0..1; the far map shows the
    /// rest. For cells fading in after they started (no pop where near and far differ). May be null: 1.
    const float* ready;
} sxc_fog_field;

/// The far map: the fog beyond the near field, coarse (one cell per terrain chunk is typical). Laid out
/// like the near field (toroidal, cell (i, j) at [mod(j, size) * size + mod(i, size)], valid for i in
/// [first_i, first_i + size), fading out over the last 2 cells). It shows where the near field fades
/// out, and where a near cell isn't ready (a negative thickness in its light layer). Past the look's
/// far_distance the ray takes one long step through it to the terrain drawn there.
typedef struct sxc_fog_far
{
    uint32_t     struct_size;
    uint32_t     size;      ///< cells per side, 1 .. SXC_FOG_MAX_FAR
    float        cell_size; ///< yards
    int32_t      first_i, first_j;
    /// Per cell: extinction per yard at the layer's floor (0 = no fog), the floor (world z), the
    /// thickness (yards: the layer thins by e over it), unused.
    const float* layer;
    /// Per cell: the colour (rgb, may exceed 1), unused. May be null: the look's colour. Multiplied by
    /// the look's brightness.
    const float* colour;
} sxc_fog_far;

/// The wake: units pushing the fog aside, a small map around the player (the near field's cells are
/// too coarse for a person-sized hole). Per texel how much of the fog is pushed out up to which
/// height, and how far the fog's patches are dragged there.
typedef struct sxc_fog_wake
{
    uint32_t     struct_size;
    uint32_t     size;      ///< texels per side, 2 .. SXC_FOG_MAX_WAKE
    float        texel;     ///< yards per texel
    float        origin[2]; ///< world x, y of texel (0, 0)'s corner; texel (x, y) at [y * size + x]
    /// Per texel: push 0..1 (the share of the fog taken out; negative: a little more fog, piled up at a
    /// wake's rim), the push's top (world z: it fades out over half a yard above), the patches'
    /// displacement x, y (yards). Keep the border texels empty (push 0): the map is clamped.
    const float* texels;
} sxc_fog_wake;

typedef struct sxc_fog_look
{
    uint32_t struct_size;
    wxl_bool enabled;
    int32_t  colour_mode;     ///< 0 the place's stock fog colour, 1 `colour`
    float    colour[3];       ///< 0..1
    float    brightness;      ///< x the colour
    float    density_scale;   ///< x every density
    float    noise_amount;    ///< 0..1 x the cells' patchiness
    float    noise_tile;      ///< yards per noise tile (patches ~1/8 of it)
    float    drift[2];        ///< yards per second the patches move (the wind)
    float    far_distance;    ///< yards: the volume's reach (and the sky's fog distance)
    float    haze_density;    ///< extinction per yard at the haze's base; 0 = no haze
    float    haze_base;       ///< world z
    float    haze_thickness;  ///< yards: the haze thins by e over this height
    int32_t  debug;           ///< 1: the fog drawn magenta; 2: the near field green, the far map blue, the haze grey
    float    step_length;     ///< yards: the fog is integrated in steps this long near the camera (smaller: smoother thin layers, more GPU); 0 = 0.5
    int32_t  method;          ///< 0 march every pixel's own ray (sharp thin layers), 1 the froxel volume (cheaper, coarse across the screen)
    int32_t  max_steps;       ///< per pixel: the most steps a ray takes (steps grow with distance); 0 = 160
    // --- appended 2026-10-20 ---
    /// yards: the near field fades into the far map over this band at its edge (wider: a softer seam,
    /// less near detail at its rim); 0 = 16
    float    handover;
    // --- appended 2026-10-26 ---
    /// Our lights (sxc.lights) in the fog: x their light where there is fog to scatter it; 0 off (also in an
    /// older, shorter look).
    float    light_strength;
    /// Forward scattering (-0.9 .. 0.9; 0 the same every way): the fog glows brighter looking toward a lamp.
    float    light_glow;
    /// yards from the camera the lights light the fog out to; 0 = 96
    float    light_range;
} sxc_fog_look;

typedef struct sxc_fog_api
{
    uint32_t struct_size;

    /// Whether the client draws the fog (a renderer that can: Vulkan).
    wxl_bool(WXL_CALL* available)(void);

    /// The near field; copied (call again when it changes). Null clears it.
    void(WXL_CALL* set_field)(const sxc_fog_field* field);

    /// The look; copied.
    void(WXL_CALL* set_look)(const sxc_fog_look* look);

    /// One line: what the renderer did last frame, or why not. Valid until the next call.
    const char*(WXL_CALL* status)(void);

    /// The place's stock fog colour this frame (what the sky's distance fog has; it follows the time
    /// of day), rgb 0..1.
    void(WXL_CALL* stock_colour)(float* out_rgb);

    // --- appended 2026-10-20 (check struct_size before using) -------------------------------------

    /// The far map; copied (call again when it changes). Null clears it.
    void(WXL_CALL* set_far)(const sxc_fog_far* far_map);

    /// The wake map; copied (call each frame it changes). Null clears it.
    void(WXL_CALL* set_wake)(const sxc_fog_wake* wake);
} sxc_fog_api;

#ifdef __cplusplus
}
#endif

#endif
