/*
 * sxc.cover: surface covers (snow, sand, mud, ash) and wetness drawn by the client's terrain shader from
 * what a mod describes. The client draws; the mod decides what lies where.
 *
 * Painted covers follow the terrain's own layers: the client asks the mod once per terrain chunk which
 * cover entry each of its texture layers carries (the resolver), and the terrain shader shows that
 * entry wherever the layer is painted, from the chunk's alpha map, at any distance. What changes over
 * time comes as coarse maps: fallen snow, painted snow's melt, the ground's wetness and the water line,
 * near the player on a fine grid, per terrain chunk beyond it, one value beyond that. Per spot the
 * covers stack: the painted cover below, fallen snow on top, the ground showing through where they're
 * thin. Where the GPU tessellates, the stack's height raises the terrain itself (the look's tess_*).
 *
 * Vulkan only (our terrain shader, [Terrain] OurShader): on another renderer available() answers false.
 */

#ifndef SXC_API_COVER_H
#define SXC_API_COVER_H

#include "SXC/Api/sxc.h"

#define SXC_COVER_SERVICE         "sxc.cover"
#define SXC_COVER_SERVICE_VERSION 1

#define SXC_COVER_MAX_ENTRIES  127 ///< entries 1..127 (0 = none)
#define SXC_COVER_MAX_TEXTURES 1024 ///< cover textures 1..1024 (0 = none)
#define SXC_COVER_MAX_MAP      256
#define SXC_COVER_MAX_LAYERS   4

/// sxc_cover_entry::flags
#define SXC_COVER_PREVENT_UNDERWATER 0x1u ///< thins out below a water surface (needs the near map's water line)
#define SXC_COVER_PREVENT_ON_LAND    0x2u ///< only below a water surface (river and sea floors)
#define SXC_COVER_USE_VERTEX_COLOR   0x4u ///< tinted by the terrain's vertex colours (else neutral)
#define SXC_COVER_SNOW               0x8u ///< painted snow: thins out by the map's kept share

/// sxc_cover_api::set_textures flags, per texture
#define SXC_COVER_TEXTURE_GLOSS 0x1u ///< its alpha is a gloss mask (as the terrain layers' _s textures)

#ifdef __cplusplus
extern "C" {
#endif

/// One cover as the terrain shader reads it (80 bytes).
typedef struct sxc_cover_entry
{
    float    depth;         ///< yd; 0 = no painted cover (the entry may still be a fallen-snow look)
    float    max_slope;     ///< degrees: steeper ground holds none...
    float    slope_fade;    ///< ...thinning out over this many degrees below it
    float    drift;         ///< depth variation, share of the depth (0..1)
    float    tint[3];       ///< rgb 0..1
    float    tint_strength; ///< 0 the cover's own colour .. 1 the tint
    float    breakup;       ///< 0 smooth .. 1 ragged, patchy edges
    float    wetness;       ///< its own wet look, 0..1
    float    moisture_min;  ///< the ground moisture it needs to show (mud); above 1 = always
    float    rest_moisture; ///< its ground's rest moisture (the maps carry the moisture above rest)
    int32_t  texture;       ///< 0 = plain (the look's brightness), 1..SXC_COVER_MAX_TEXTURES
    uint32_t flags;         ///< SXC_COVER_*
    float    z_offset;      ///< yd the cover's base sits above (+) or below (-) the terrain: below, only what rises above it shows
    float    reserved;
    // Trails (step 5d):
    float    stiffness;     ///< 0 feet press it to the ground .. 1 rigid (no trail)
    float    rim;           ///< the rim pushed up beside a trail, share of the cover's depth
    float    relax_seconds; ///< a trail fills back in over this long
    float    reserved2;
} sxc_cover_entry;

typedef struct sxc_cover_layer
{
    const char* texture;       ///< the layer's texture path (valid during the call)
    uint32_t    ground_effect; ///< GroundEffectTexture ID, 0 none
    int32_t     terrain_type;  ///< that effect's TerrainType ID, -1 none
} sxc_cover_layer;

/// What the resolver is asked about one terrain chunk, and what it answers.
typedef struct sxc_cover_chunk
{
    uint32_t        struct_size;
    int32_t         map_id;
    uint32_t        area;        ///< the chunk's AreaTable ID
    int32_t         layer_count; ///< 1..SXC_COVER_MAX_LAYERS, in the terrain shader's layer order
    sxc_cover_layer layers[SXC_COVER_MAX_LAYERS];
    /// Answers, per layer (entry indices, 0 = none): the painted cover it carries, and the look fallen
    /// snow takes on it.
    uint16_t        painted[SXC_COVER_MAX_LAYERS];
    uint16_t        snow_look[SXC_COVER_MAX_LAYERS];
} sxc_cover_chunk;

/// Called on the main thread while the terrain draws, once per chunk until invalidate(). Fills
/// io->painted and io->snow_look (zeroed before the call).
typedef void(WXL_CALL* sxc_cover_resolve_fn)(void* user, sxc_cover_chunk* io);

/// A world-aligned toroidal grid: cell (i, j) (i = floor(x / cell_size), j = floor(y / cell_size)) at
/// [mod(j, size) * size + mod(i, size)], 4 floats each, valid for i in [first_i, first_i + size) (same
/// for j). Every cell holds a value (fill cells the simulation doesn't know with the coarser values);
/// the shader fades into the next coarser map over the last 2 cells.
typedef struct sxc_cover_map
{
    uint32_t     struct_size;
    uint32_t     size;      ///< cells per side, 1 .. SXC_COVER_MAX_MAP
    float        cell_size; ///< yd
    int32_t      first_i, first_j;
    /// Per cell: fallen snow (yd), painted snow's kept share (0..1), the ground's moisture above its
    /// rest (0..1), the water surface (world z; below -10000: no water near). The far map ignores the
    /// water surface.
    const float* cells;
} sxc_cover_map;

typedef struct sxc_cover_look
{
    uint32_t struct_size;
    wxl_bool enabled;
    wxl_bool fallen_snow;     ///< fallen snow drawn on top
    int32_t  debug;           ///< 0 off, 1 what lies where, 2 wetness, 3 entries, 4 the maps' reach, 5 the height, 6 trails
    float    brightness;      ///< a plain cover's colour (no texture), 0..1
    float    wet_darkening;   ///< how much darker soaked ground gets (0..1)
    float    thin_depth;      ///< yd: fallen snow hides the ground below from this depth
    float    depth_scale;     ///< x every depth (testing; 1 = as the entries say)
    float    breakup_scale;   ///< x every breakup
    float    drift_scale;     ///< x every drift
    float    texture_tile;    ///< yd per repeat of a cover texture; 0 = 4.1667 (the terrain layers')
    float    outside[3];      ///< beyond the far map: fallen snow, kept share, moisture above rest
    /// Depth (the terrain raised onto its covers by tessellation, where the GPU has it): a segment of the
    /// subdivided terrain is about this many yd per yd of distance from the camera (smaller: finer, more
    /// GPU); 0 = flat covers (the look only).
    float    tess_detail;
    float    tess_reach;      ///< yd: the terrain is subdivided within this distance (beyond, raised at its own vertices)
    float    tess_most;       ///< segments per triangle edge at most (the GPU's limit caps it)
    /// Trails (step 5d; with depth): units' feet press the cover down where they step and drag (found
    /// from their models' footstep events, followed by their bones), a rim beside. A fine layer near the
    /// player fills back in by the covers' relax_seconds; a coarse one keeps the trail for trail_persist.
    wxl_bool trails;
    float    trail_persist;   ///< s: the coarse layer keeps a trail this long
    float    trail_slur;      ///< how much deeper cover softens a trail's edges (radius per yd of depth)
    float    trail_rim;       ///< x every rim
    float    trail_reach;     ///< yd: units within this of the player leave trails
    /// Where a followed foot carves (the cover is only ever pressed down to the foot's own height, so a
    /// foot above the surface leaves nothing): SXC_COVER_CARVE_*.
    int32_t  trail_carve;
    float    trail_lift;      ///< yd above the unit: SXC_COVER_CARVE_UP_TO's height

    // --- appended 2026-10-27: read when struct_size reaches them, else 0 ----------------------------
    /// The steady wind at ground level around the player (0 calm .. 1 strong): covers lying in it are
    /// levelled sooner, so a terrain texture's pattern (parallax, bumps) fades under thinner snow.
    float    steady_wind;
} sxc_cover_look;

/// Wherever the foot is (up to 1.5 yd above the unit: not while jumping): its swing through deep cover
/// leaves a drag line between the prints; rims only near contact.
#define SXC_COVER_CARVE_THROUGH 0
/// Only near contact (within 0.1 yd of the height it had at its last footstep, relative to the unit):
/// clean prints.
#define SXC_COVER_CARVE_CONTACT 1
/// Up to trail_lift above the unit, rims everywhere (the first version's rule, 0.3 yd).
#define SXC_COVER_CARVE_UP_TO   2

typedef struct sxc_cover_api
{
    uint32_t struct_size;

    /// Whether the client draws covers (our terrain shader on Vulkan).
    wxl_bool(WXL_CALL* available)(void);

    /// The resolver (null: none; every chunk has no cover). Setting it invalidates every chunk.
    void(WXL_CALL* set_resolver)(sxc_cover_resolve_fn resolve, void* user);

    /// The resolver's answers changed (tables edited): every chunk asks again.
    void(WXL_CALL* invalidate)(void);

    /// The entries, copied: entries[k] is entry k + 1. count <= SXC_COVER_MAX_ENTRIES.
    void(WXL_CALL* set_entries)(const sxc_cover_entry* entries, uint32_t count);

    /// The cover textures (archive paths): paths[k] is texture k + 1, flags[k] its SXC_COVER_TEXTURE_*
    /// (flags may be null). count <= SXC_COVER_MAX_TEXTURES. The client loads one when a drawn chunk
    /// first shows an entry naming it, and keeps it. How many show at once depends on the GPU: all of
    /// them where it indexes textures per pixel ("bindless"), else the 16 the chunks in view use most
    /// (the others draw plain until a slot frees up); status() says which.
    void(WXL_CALL* set_textures)(const char* const* paths, const uint32_t* flags, uint32_t count);

    /// The near map and the far map; copied (call again when they change). Null clears one.
    void(WXL_CALL* set_map)(const sxc_cover_map* near_map);
    void(WXL_CALL* set_far)(const sxc_cover_map* far_map);

    /// The look; copied.
    void(WXL_CALL* set_look)(const sxc_cover_look* look);

    /// One line: what the cover did last frame, or why not. Valid until the next call.
    const char*(WXL_CALL* status)(void);

    /// A cover texture's load state: 0 not loaded (not used yet, or no such texture), 1 loading, 2 loaded
    /// (with its gloss variant "_s" when 3); + 4 while it has a slot (it shows).
    int32_t(WXL_CALL* texture_state)(uint32_t texture);
} sxc_cover_api;

#ifdef __cplusplus
}
#endif

#endif
