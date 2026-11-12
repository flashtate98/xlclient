/*
 * sxc.grass: the ground-effect grass as the client draws it instanced (Vulkan): a mod moves it in the
 * wind and parts it around characters. Each frame the mod hands over the wind around the player (a
 * grid of samples), the characters near it and the motion's settings; the client bends every blade in
 * its vertex shader (Shaders/GrassMotion.glsl). Per plant model the bend runs from its root to its tip
 * in texture V, found from the model's geometry unless a row of the mod's table overrides it.
 *
 * Density: the mod answers, per plant model and place, how many copies of each plant to draw near the
 * player; the client draws them on the GPU (each within its plant's terrain triangle) and fades them out
 * with the distance.
 *
 * On another renderer (or with the instanced grass off) available() answers false and nothing moves.
 */

#ifndef SXC_API_GRASS_H
#define SXC_API_GRASS_H

#include "SXC/Api/sxc.h"

#define SXC_GRASS_SERVICE         "sxc.grass"
#define SXC_GRASS_SERVICE_VERSION 1

#define SXC_GRASS_GRID       8   ///< wind samples per side
#define SXC_GRASS_MAX_ACTORS 16

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sxc_grass_motion
{
    uint32_t struct_size;
    wxl_bool on;
    float    time;            ///< seconds (keep it below an hour: the flutter's phase)
    float    anchor;          ///< 0..0.9: the bottom share of a blade that never moves
    float    amplitude;       ///< yd the tips move at wind strength 1
    float    flutter;         ///< yd of extra per-blade shimmer
    float    flutter_speed;   ///< radians a second
    float    push_strength;   ///< yd the blades lean away from a character
    float    grid_origin[2];  ///< world x, y of cell (0, 0)'s corner
    float    grid_spacing;    ///< yd between samples
    float    wind[SXC_GRASS_GRID * SXC_GRASS_GRID][3]; ///< cell (y * 8 + x): wind x and y (x strength), gust 0..1
    float    player[3];       ///< the actors are gathered around it
    float    actor_range;     ///< yd: grass farther from the player skips the actors
    uint32_t actor_count;
    float    actors[SXC_GRASS_MAX_ACTORS][4]; ///< position, push radius (yd)
} sxc_grass_motion;

/// A GroundEffectDoodadWind row: it replaces the automatic values of one plant model.
typedef struct sxc_grass_doodad
{
    uint32_t id;              ///< GroundEffectDoodad.dbc ID
    uint32_t flags;           ///< SXC_GRASS_NO_WIND, SXC_GRASS_NO_PUSH, SXC_GRASS_FLIP
    float    stiffness;       ///< 0 flexible .. 1 rigid (scales wind and push)
    float    root_v;          ///< the bend's start in texture V; < 0: automatic
    float    tip_v;           ///< its end; < 0: automatic
} sxc_grass_doodad;

#define SXC_GRASS_NO_WIND  0x1u
#define SXC_GRASS_NO_PUSH  0x2u
#define SXC_GRASS_FLIP     0x10u
#define SXC_GRASS_NO_MARKS 0x20u ///< the ground marks leave it alone (stones don't burn or freeze; 0x4, 0x8 are the table's reserved bits)

/// What a ground mark (sxc.cover's set_marks, SXC_MARK_*) does to the plants standing in it, per channel.
/// A channel's value v (0..1; the life channel's magnitude) works in two halves: up to 0.5 the plant takes
/// on the tint, from 0.5 to 1 it shrinks toward `size` (burnt grass blackens, then burns away; growing back
/// in reverse). The wind follows `wind` over the whole range.
typedef struct sxc_grass_mark_look
{
    float tint[3];        ///< rgb 0..1, as the plant's own colour is
    float tint_strength;  ///< 0 none .. 1 the tint at full
    float size;           ///< the plant's size at full (0 gone, 1 as it was)
    float wind;           ///< x the wind at full (0 frozen stiff, 1 as it was)
    float reserved[2];
} sxc_grass_mark_look;

/// The marks' looks; copied (null or on = false: the plants ignore the marks).
typedef struct sxc_grass_marks
{
    uint32_t            struct_size;
    wxl_bool            on;
    sxc_grass_mark_look channel[4];   ///< SXC_MARK_BURN, _FROST, _LIFE (positive: blooming), _SOOT
    sxc_grass_mark_look blight;       ///< the life channel below 0
} sxc_grass_marks;

/// The density of one plant model in one place (step 7d): the provider's answer.
typedef struct sxc_grass_density
{
    float multiplier;         ///< plants drawn per placed plant (1: no copies)
    float radius;             ///< yd around the player; full density within half of it
    float spread;             ///< most yd a copy moves from its plant (it stays on the plant's terrain triangle)
} sxc_grass_density;

/// Asked when a chunk's plants are placed, per plant model and ground effect there; answers are kept
/// until set_density_provider is called again. False: no copies.
typedef wxl_bool(WXL_CALL* sxc_grass_density_fn)(void* user, uint32_t area_id, int32_t map_id, uint32_t doodad_id, uint32_t effect_id,
                                                 sxc_grass_density* out);

/// The player's limits on the density, every frame (changing them rebuilds nothing).
typedef struct sxc_grass_density_limits
{
    uint32_t struct_size;
    wxl_bool on;
    float    max_multiplier;  ///< 1..16
    float    max_radius;      ///< yd
    float    max_spread;      ///< yd
} sxc_grass_density_limits;

/// Soft edges (step 7d, a test): where ground-effect cells border cells without plants, the plants near
/// the border shrink, and some spill a little way onto the bare ground, smaller still. Changing them
/// places every chunk anew.
typedef struct sxc_grass_edges
{
    uint32_t struct_size;
    wxl_bool on;
    float    band;            ///< yd inside the border over which plants shrink
    float    shrink;          ///< size at the border (0..1; 1: none)
    float    spill;           ///< yd a spilled plant may go onto the bare ground (0: none)
    float    share;           ///< spilled plants per plant near the border (0..4)
    wxl_bool same_texture;    ///< spill only onto bare ground of the same terrain texture
} sxc_grass_edges;

/// What the client found for a plant model (the grass panel).
typedef struct sxc_grass_doodad_info
{
    uint32_t    struct_size;
    const char* path;         ///< its model file; valid until the next call
    wxl_bool    analyzed;     ///< its model has been read
    float       height;       ///< yd
    float       root_v, tip_v; ///< automatic
    wxl_bool    flat;         ///< too low to sway (pebbles, shells)
    float       entry[4];     ///< what its plants carry: root V, 1 / span, wind scale, push scale
} sxc_grass_doodad_info;

struct sxc_grass_inspect;   // below

typedef struct sxc_grass_api
{
    uint32_t struct_size;

    /// Whether the client draws the grass instanced now (the motion only moves that grass).
    wxl_bool(WXL_CALL* available)(void);

    /// This frame's motion; copied. Without a call (or with on = false) the grass stands still.
    void(WXL_CALL* set_motion)(const sxc_grass_motion* motion);

    /// The table of overrides; copied. Call again when it changes (the plants are placed anew).
    void(WXL_CALL* set_doodads)(const sxc_grass_doodad* rows, uint32_t count);

    /// One plant model's values (false: the client doesn't know that ID).
    wxl_bool(WXL_CALL* doodad_info)(uint32_t id, sxc_grass_doodad_info* out);

    /// One line: what the grass did last frame.
    const char*(WXL_CALL* status)(void);

    /// Step 7d (check struct_size). Who answers the density (null: none); call again when its answers
    /// change (the plants are placed anew).
    void(WXL_CALL* set_density_provider)(sxc_grass_density_fn fn, void* user);

    /// This frame's limits; copied. Without a call (or with on = false) no copies are drawn.
    void(WXL_CALL* set_density_limits)(const sxc_grass_density_limits* limits);

    /// Soft edges; copied (null or on = false: none). Only a change places the chunks anew.
    void(WXL_CALL* set_edges)(const sxc_grass_edges* edges);

    /// How ground effects choose their layer (SXC_GRASS_PLACE_*; a client setting, kept; stock grass too):
    /// the cell's dominant layer as the map file has it, the dominant layer worked out from the paint, or
    /// every layer's plants by how much of it is painted where they fall (none under `min_share`). A change
    /// places all the grass again.
    void(WXL_CALL* set_placement)(uint32_t mode, float min_share);
    uint32_t(WXL_CALL* placement)(float* min_share);

    /// Plants on slopes (client settings, kept; stock grass too): the least up part of the ground's normal a
    /// plant may stand on (stock 0.4), and how far upright plants sink by the drop under their downhill
    /// edge (factor, stock 0). A change places all the grass again.
    void(WXL_CALL* set_slopes)(float min_normal_z, float sink);
    void(WXL_CALL* slopes)(float* min_normal_z, float* sink);

    /// Checks the placed plants (stock placement, as the client made them) within `radius` yd of the
    /// player against the terrain. False: not in the world.
    wxl_bool(WXL_CALL* inspect)(float radius, struct sxc_grass_inspect* out);

    // --- appended 2026-10-27 (check struct_size before using) -------------------------------------

    /// What the ground marks (sxc.cover's set_marks) do to plants; copied, call again when it changes.
    void(WXL_CALL* set_marks)(const sxc_grass_marks* marks);
} sxc_grass_api;

/// What inspect found.
typedef struct sxc_grass_inspect
{
    uint32_t    struct_size;
    uint32_t    plants;        ///< checked
    uint32_t    off_ground;    ///< whose origin isn't on the terrain (more than 0.05 yd off)
    float       off_max;       ///< yd, signed (> 0: above)
    uint32_t    hanging;       ///< upright ones whose base hangs more than 0.25 yd over the ground somewhere
    float       hang_max;      ///< yd
    float       hang_slope;    ///< their mean slope (degrees)
    float       slope;         ///< the mean slope of all checked (degrees)
    uint32_t    aligned;       ///< terrain-aligned ones (they lie on the slope; not counted as hanging)
    float       worst[3];      ///< the worst-hanging plant's position
    float       worst_slope;   ///< degrees
    float       worst_scale;
    uint32_t    worst_doodad;
    const char* worst_path;    ///< its model; valid until the next call
} sxc_grass_inspect;

#define SXC_GRASS_PLACE_STOCK    0u
#define SXC_GRASS_PLACE_DOMINANT 1u
#define SXC_GRASS_PLACE_PAINTED  2u

#ifdef __cplusplus
}
#endif

#endif
