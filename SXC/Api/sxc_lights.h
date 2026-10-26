/*
 * sxc.lights: point lights drawn per pixel by the client's own shaders, from what a mod sets. The client
 * gathers the lights (the models' own point lights: torches, braziers, lanterns; and the lights a mod
 * puts on model files by data), merges close ones, lets them flicker and hands the nearest to the GPU
 * on a world grid around the camera; each pixel adds its grid cell's lights to the stock light.
 *
 * Vulkan only: on another renderer available() answers false. The light editor's queries (models near a
 * point, their attachment points, last frame's lights) answer on any renderer.
 */

#ifndef SXC_API_LIGHTS_H
#define SXC_API_LIGHTS_H

#include "SXC/Api/sxc.h"

#define SXC_LIGHTS_SERVICE         "sxc.lights"
#define SXC_LIGHTS_SERVICE_VERSION 1

#define SXC_LIGHTS_MAX_POOL     512 ///< lights in range at most (the farthest are left out past this)
#define SXC_LIGHTS_MAX_PER_CELL 32  ///< a grid cell's lights at most (the shaders' loop bound)
#define SXC_LIGHTS_MAX_CELLS    64  ///< grid cells per side at most

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sxc_lights_settings
{
    uint32_t struct_size;
    wxl_bool enabled;
    wxl_bool stock_off;       ///< the client's own point lights (3 per terrain chunk) off while ours are on
    int32_t  debug;           ///< 0 off, 1 our lights only (on grey ground), 2 the cell's light count
    float    radius;          ///< yd: a light is exactly 0 here (the stock torches reach ~13 yd to 5%)
    float    brightness;      ///< x the lights' colours
    float    range;           ///< yd from the camera: no lights beyond, fading out over the last third
    float    cell_size;       ///< yd per grid cell (the grid reaches the range, SXC_LIGHTS_MAX_CELLS per side at most)
    int32_t  max_per_cell;    ///< lights a cell holds (the strongest at its middle), 1..SXC_LIGHTS_MAX_PER_CELL
    float    merge_distance;  ///< yd: closer lights merge into one (torch groups)
    float    flicker;         ///< 0..1: how much a flame dips (each light its own phase)
    float    flicker_speed;   ///< x the default pace (~3 dips a second)
    /// Inside and outside apart: a light inside an indoor WMO group lights only indoor surfaces (indoor
    /// WMO groups), one outside only outdoor ones (the terrain, outdoor groups); models take both. Lights
    /// with a shadow map don't need it (their walls, floors and roofs are in the map).
    wxl_bool separate_indoor;
    /// Shadows: the nearest lights that stand still get a cube shadow map of the WMOs and the terrain
    /// around them (walls between rooms, floors, roofs); drawn once and kept until the light moves or its
    /// surroundings change (a WMO finishing loading).
    wxl_bool shadows;
    int32_t  shadow_count;    ///< lights with a shadow map at most (the nearest), up to the device's (stats.shadow_slots)
    int32_t  shadow_updates;  ///< shadow maps drawn per frame at most
    wxl_bool shadow_doodads;  ///< the placed doodads cast too (solid and alpha-keyed batches; blended ones don't)
} sxc_lights_settings;

/// What a light is (a DoodadLightProperties row). Negative values (and flicker_mode -1) take the
/// settings' defaults.
typedef struct sxc_lights_kind
{
    uint32_t id;
    uint32_t type;            ///< SXC_LIGHTS_POINT, SXC_LIGHTS_SPOT
    float    color[3];        ///< linear 0..1, x the intensity
    float    radius;          ///< yd (x the model's scale); <= 0: the settings' radius
    float    falloff;         ///< the window's power: 1 smooth, higher tighter; <= 0: 1
    float    inner_angle;     ///< spot: degrees from the axis at full strength
    float    outer_angle;     ///< spot: degrees from the axis beyond which there is none
    int32_t  flicker_mode;    ///< SXC_LIGHTS_FLICKER_*
    float    flicker_speed;   ///< x the default pace; < 0: 1
    float    flicker_amount;  ///< 0..1 how much it dips; < 0: the settings' flicker
    uint32_t flags;           ///< reserved
} sxc_lights_kind;

/// Where a light and / or an emitter go (a DoodadSourceAssignment row): on every model of a file.
typedef struct sxc_lights_assignment
{
    const char* model_path;   ///< the model file (case and / vs \ don't matter); copied
    uint32_t    attach_type;  ///< SXC_LIGHTS_ATTACH_*
    int32_t     attach_index; ///< the attachment point's ID, the bone index or the particle emitter index
    float       offset[3];    ///< yd from the attachment, in the model's space (scaled with it)
    float       direction[3]; ///< spot: where the cone points; an emitter's direction (a draft); in the model's space
    uint32_t    light_id;     ///< a kind's id; 0 = no light (with the flag: only suppresses)
    uint32_t    flags;        ///< SXC_LIGHTS_SUPPRESS_MODEL_LIGHTS
    uint32_t    id;           ///< the row's ID (the log names it)
    uint32_t    emitter_id;   ///< the mod's emitter (heat, moisture, a draft) placed here too; 0 = none. The client only places it (emitters())
} sxc_lights_assignment;

/// An emitter where its row placed it (emitters()): what it does is the mod's (its emitter_id).
typedef struct sxc_lights_emitter
{
    uint32_t    row_id;        ///< its DoodadSourceAssignment row
    uint32_t    emitter_id;
    float       pos[3];        ///< world
    float       direction[3];  ///< world, unit: the row's direction through the model
    float       scale;         ///< the model's scale (x the emitter's radius)
    float       distance;      ///< from the camera
    const char* model_path;    ///< valid until the next frame
} sxc_lights_emitter;

#define SXC_LIGHTS_POINT 0u
#define SXC_LIGHTS_SPOT  1u

#define SXC_LIGHTS_FLICKER_DEFAULT -1 ///< smooth, by the settings' amount
#define SXC_LIGHTS_FLICKER_OFF      0
#define SXC_LIGHTS_FLICKER_SMOOTH   1 ///< three sines
#define SXC_LIGHTS_FLICKER_NOISE    2 ///< random levels ~6 a second, eased
#define SXC_LIGHTS_FLICKER_STEPS    3 ///< the same levels, held

#define SXC_LIGHTS_ATTACH_ORIGIN    0u
#define SXC_LIGHTS_ATTACH_POINT     1u ///< an attachment point by its ID
#define SXC_LIGHTS_ATTACH_BONE      2u ///< a bone by its index
#define SXC_LIGHTS_ATTACH_EMITTER   3u ///< a particle emitter by its index

#define SXC_LIGHTS_SUPPRESS_MODEL_LIGHTS 0x1u ///< the model's own lights are left out

typedef struct sxc_lights_stats
{
    uint32_t struct_size;
    uint32_t models;          ///< models in the scene
    uint32_t model_lights;    ///< point lights on them
    uint32_t placed;          ///< of those, at the position the client set this frame (the model animates)
    uint32_t in_range;        ///< within the range
    uint32_t merged;          ///< merged into another
    uint32_t active;          ///< handed to the GPU
    uint32_t cells;           ///< grid cells per side
    uint32_t busiest_cell;    ///< lights in the busiest cell
    uint32_t full_cells;      ///< cells that had more than they hold
    uint32_t table_lights;    ///< lights from the assignments
    uint32_t suppressed;      ///< models whose own lights an assignment left out
    uint32_t attach_missing;  ///< assignments whose attachment the model doesn't have (placed at its origin)
    uint32_t dropped;         ///< lights past SXC_LIGHTS_MAX_POOL (the farthest)
    uint32_t indoor;          ///< drawn lights inside an indoor WMO group
    uint32_t indoor_tests;    ///< points located this frame (the rest from the cache)
    uint32_t model_tests;     ///< models located this frame (for which lights they take)
    uint32_t shadow_slots;    ///< shadow maps the device holds (0: none)
    uint32_t shadowed;        ///< drawn lights with a shadow map
    uint32_t shadow_draws;    ///< shadow maps drawn this frame
    uint32_t shadow_triangles;///< triangles in them
    uint32_t shadow_waiting;  ///< lights that should have one and wait their turn (or still moving)
    uint32_t shadow_doodads;  ///< doodads in the maps drawn this frame
} sxc_lights_stats;

/// A model in the world scene (the light editor's list).
typedef struct sxc_lights_model
{
    const char* path;          ///< the model file as the client names it; valid until the next call
    const char* parent_path;   ///< an attached model (an item in a hand): the model it hangs on; else null
    float       pos[3];        ///< its origin (world)
    float       distance;      ///< from the point asked about
    uint32_t    bones, attachments, emitters, own_lights;
    wxl_bool    animated;      ///< posed this frame (else an attached model's points are unknown)
} sxc_lights_model;

#define SXC_LIGHTS_POINT_OWN_LIGHT 4u ///< sxc_lights_point.kind: one of the model's own lights (index into them)

/// A place on a model a light can hang on, where it is this frame.
typedef struct sxc_lights_point
{
    uint32_t kind;             ///< SXC_LIGHTS_ATTACH_POINT (index: the attachment ID), _BONE, _EMITTER, SXC_LIGHTS_POINT_OWN_LIGHT
    int32_t  index;
    uint32_t bone;
    float    pos[3];           ///< world
} sxc_lights_point;

/// A light as it was drawn last frame (after merging).
typedef struct sxc_lights_active
{
    float       pos[3];
    float       radius;
    float       color[3];      ///< x brightness, fade and flicker
    float       cos_outer;     ///< spot: cos of the outer angle; -2 a point light
    float       spot_dir[3];
    wxl_bool    indoor;
    wxl_bool    shadowed;      ///< has a shadow map
    uint32_t    row_id;        ///< its DoodadSourceAssignment row; 0: a model's own light
    const char* model_path;    ///< the model it is on; valid until the next frame
} sxc_lights_active;

typedef struct sxc_lights_api
{
    uint32_t struct_size;

    /// Whether the client draws our lights (Vulkan with our terrain shader).
    wxl_bool(WXL_CALL* available)(void);

    /// The settings; copied.
    void(WXL_CALL* set_settings)(const sxc_lights_settings* settings);

    /// Last frame's numbers (out->struct_size set by the caller).
    void(WXL_CALL* stats)(sxc_lights_stats* out);

    /// One line: what the lights did last frame, or why not. Valid until the next call.
    const char*(WXL_CALL* status)(void);

    /// The lights by data (DoodadLightProperties, DoodadLightAssignment); copied. Call again when they
    /// change. Null / 0: none.
    void(WXL_CALL* set_table)(const sxc_lights_kind* kinds, uint32_t kind_count, const sxc_lights_assignment* rows, uint32_t row_count);

    // --- appended 2026-10-23: the light editor (check struct_size) ----------------------------------

    /// The models of the world scene (and the models attached to them) within `range` yd of `pos`,
    /// nearest first, at most `max`: how many were written.
    uint32_t(WXL_CALL* models_near)(const float pos[3], float range, sxc_lights_model* out, uint32_t max);

    /// The placed model of file `path` nearest `pos`: its attachment points, particle emitters and own
    /// lights (and its bones' pivots with `bones`), where they are this frame; how many were written.
    uint32_t(WXL_CALL* model_points)(const char* path, const float pos[3], wxl_bool bones, sxc_lights_point* out, uint32_t max);

    /// Last frame's drawn lights; how many were written.
    uint32_t(WXL_CALL* active_lights)(sxc_lights_active* out, uint32_t max);

    /// The assignment rows (ids) whose attachment a model didn't have (placed at its origin) since the
    /// table was last set; how many were written.
    uint32_t(WXL_CALL* missing_rows)(uint32_t* out, uint32_t max);

    // --- appended 2026-10-26: emitters (check struct_size) -----------------------------------------

    /// The emitters the rows placed on the scene's models within the range (the settings'; nearest first
    /// not guaranteed): from the lights' last gathering, or a scan of their own when the lights didn't
    /// gather (off, another renderer). How many were written.
    uint32_t(WXL_CALL* emitters)(sxc_lights_emitter* out, uint32_t max);

    /// The placed model of file `path` nearest `pos`: its own x, y and z axes in the world (unit, 3 x 3,
    /// a row per axis), to turn a world direction into the model's space. False: none placed, or it hangs
    /// on another model.
    wxl_bool(WXL_CALL* model_axes)(const char* path, const float pos[3], float axes[9]);
} sxc_lights_api;

#ifdef __cplusplus
}
#endif

#endif
