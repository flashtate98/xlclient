/*
 * sxc.world: where the player is, from the engine. One snapshot per call (cheap; a mod takes one per
 * frame) plus the area queries the climate and the place rules need, and point queries on the loaded
 * world: the terrain's height and painted layers, terrain liquid, a line trace.
 *
 * The point queries read what's loaded around the camera (a few hundred yards); elsewhere they
 * answer false. Main thread only.
 */

#ifndef SXC_API_WORLD_H
#define SXC_API_WORLD_H

#include "SXC/Api/sxc.h"

#define SXC_WORLD_SERVICE         "sxc.world"
#define SXC_WORLD_SERVICE_VERSION 1

#define SXC_WORLD_MAX_AREA_CHAIN 8

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sxc_weather_type
{
    SXC_WEATHER_FINE = 0,
    SXC_WEATHER_RAIN = 1,
    SXC_WEATHER_SNOW = 2,
    SXC_WEATHER_SAND = 3
} sxc_weather_type;

typedef struct sxc_world_snapshot
{
    uint32_t    struct_size;
    wxl_bool    in_world;          ///< a player object exists
    wxl_vec3    player_position;
    int32_t     map_id;            ///< -1 outside the world
    /// The player's area and its parents, most specific first (sub-area, zone, ...); WMO areas count.
    uint32_t    area_chain[SXC_WORLD_MAX_AREA_CHAIN];
    int32_t     area_count;
    wxl_bool    outdoors;          ///< the client's verdict (gates sky, fog, outdoor light)
    int32_t     terrain_type;      ///< TerrainType ID under the player, -1 none
    float       weather_raw;       ///< storm intensity as the weather keeps it
    float       weather_intensity; ///< 0..1 after the engine's dead zone (below 0.25 nothing reacts)
    int32_t     weather_type;      ///< sxc_weather_type
    wxl_vec3    camera_position;
    const char* zone_text;         ///< for display; valid until the zone changes
    const char* subzone_text;
} sxc_world_snapshot;

#define SXC_TERRAIN_MAX_LAYERS 4

typedef struct sxc_terrain_layer
{
    const char* texture;       ///< the layer's texture path; valid until the next sxc.world call
    uint32_t    ground_effect; ///< GroundEffectTexture ID, 0 none
    int32_t     terrain_type;  ///< that effect's TerrainType ID, -1 none
    float       weight;        ///< painted strength here, 0..1 (layer 0 gets what the others leave)
} sxc_terrain_layer;

/// What the terrain is made of at a point: the chunk's area, its layers and how strongly each is
/// painted there (bilinear over the chunk's alpha maps, ~0.5 yd texels).
typedef struct sxc_terrain_sample
{
    uint32_t          struct_size;
    uint32_t          area;           ///< the terrain chunk's AreaTable ID
    int32_t           layer_count;    ///< 0..SXC_TERRAIN_MAX_LAYERS
    int32_t           dominant_layer; ///< the client's own dominant layer for the cell (footsteps, grass); -1 none
    uint32_t          chunk_serial;   ///< changes whenever the chunk under the point is loaded anew
    sxc_terrain_layer layers[SXC_TERRAIN_MAX_LAYERS];

    // --- appended 2026-10-27: filled when struct_size reaches them ----------------------------
    uint32_t layer_flags[SXC_TERRAIN_MAX_LAYERS];   ///< each layer's MCLY flags (0x400 reflective, 0x100 alpha map...)
    uint32_t texture_flags[SXC_TERRAIN_MAX_LAYERS];     ///< each layer's texture's MTXF flags as applied (TerrainTextureParams over
                                                        ///< the ADT's); bits 4-7: retail's texture scale (1 << those bits)
    uint32_t adt_texture_flags[SXC_TERRAIN_MAX_LAYERS]; ///< the same as the ADT has them, 0 where the tile has no MTXF
} sxc_terrain_sample;

/// LiquidType.dbc's Type column.
typedef enum sxc_liquid_category
{
    SXC_LIQUID_WATER = 0,
    SXC_LIQUID_OCEAN = 1,
    SXC_LIQUID_MAGMA = 2,
    SXC_LIQUID_SLIME = 3
} sxc_liquid_category;

/// A unit near a point (sxc_world_api::units). Grows by appending: the caller passes the size it knows.
typedef struct sxc_world_unit
{
    uint64_t guid;
    wxl_vec3 position;
    float    bounding_radius;  ///< yd, as the server sends it (UNIT_FIELD_BOUNDINGRADIUS; 0.5 when missing)
    /// yd: the bounding radius scaled by how much wider the current model is than the native one
    /// (morphs and shapeshifts change the model, the server keeps the radius)
    float    effective_radius;
    float    distance;         ///< yd from the query's centre
    uint32_t display_id, native_display_id;
    wxl_bool mounted;          ///< UNIT_FIELD_MOUNTDISPLAYID set
    wxl_bool is_player;        ///< the active player
} sxc_world_unit;

/// sxc_world_api::trace: what blocks the line.
#define SXC_TRACE_TERRAIN_AND_WMO 0x01000124u ///< terrain and map objects (the cursor's mask); models never block

typedef struct sxc_world_api
{
    uint32_t struct_size;

    /// Fills `out` (set out->struct_size first).
    void(WXL_CALL* snapshot)(sxc_world_snapshot* out);

    /// An area and its parents, most specific first. Returns how many were written (0 for area 0).
    int32_t(WXL_CALL* area_chain)(uint32_t area, uint32_t* out, int32_t max);

    /// The area of the terrain chunk under a world position (x, y); 0 where no chunk is loaded.
    uint32_t(WXL_CALL* terrain_area_at)(float x, float y);

    // --- appended 2026-10-19 (check struct_size before using) -------------------------------------

    /// The height of the drawn terrain surface at (x, y), the same triangles the client draws. False
    /// where no chunk is loaded or on a terrain hole. WMOs and doodads don't count.
    wxl_bool(WXL_CALL* terrain_height)(float x, float y, float* out_z);

    /// The terrain's layers at (x, y). False where no chunk is loaded or on a hole. Set
    /// out->struct_size first.
    wxl_bool(WXL_CALL* terrain_sample)(float x, float y, sxc_terrain_sample* out);

    /// Terrain liquid (rivers, lakes, sea; not WMO liquid) at (x, y): its surface height and its
    /// LiquidType.dbc ID. False where the terrain cell has none.
    wxl_bool(WXL_CALL* terrain_liquid)(float x, float y, float* out_height, uint32_t* out_liquid_type);

    /// A LiquidType's sxc_liquid_category (-1 for an unknown ID); its name in *out_name if not null
    /// ("" if unknown; valid while the client runs).
    int32_t(WXL_CALL* liquid_category)(uint32_t liquid_type, const char** out_name);

    /// Whether the line from `from` to `to` hits something (flags: SXC_TRACE_*, 0 = terrain and
    /// WMO); *out_fraction (if not null) is how far along the line the hit is, 0..1.
    wxl_bool(WXL_CALL* trace)(const wxl_vec3* from, const wxl_vec3* to, uint32_t flags, float* out_fraction);

    /// An area's name in the client's locale ("" if unknown; valid while the client runs).
    const char*(WXL_CALL* area_name)(uint32_t area);

    // --- appended 2026-10-19 (2) ------------------------------------------------------------------

    /// Where a world point is on the window: x, y from 0 (left / top) to 1 (right / bottom), the
    /// way the client places nameplates. False behind the camera (or before the world is up). Off
    /// screen points answer true with values outside 0..1.
    wxl_bool(WXL_CALL* project)(const wxl_vec3* world, float* out_x, float* out_y);

    /// A test weather the client shows as if the server sent it (sxc_weather_type, intensity 0..1 as
    /// the snapshot's weather_intensity). While it's on, the server's weather packets are noted and
    /// held back; type < 0 ends the test and shows the server's last weather again. Not saved.
    void(WXL_CALL* set_weather)(int32_t type, float intensity);

    /// The test weather's type, or -1 when the server's weather shows.
    int32_t(WXL_CALL* weather_test)(void);

    // --- appended 2026-10-20 ------------------------------------------------------------------------

    /// The units (players, NPCs, creatures) within `range` yd of `centre`, nearest first, written to
    /// `out` (at most `max`, each `unit_size` bytes: pass sizeof(sxc_world_unit); fields past what the
    /// client knows stay zero). Returns how many were written.
    int32_t(WXL_CALL* units)(const wxl_vec3* centre, float range, sxc_world_unit* out, int32_t max, uint32_t unit_size);
} sxc_world_api;

#ifdef __cplusplus
}
#endif

#endif
