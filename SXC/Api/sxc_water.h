/*
 * sxc.water: our water (SXC/Water) for mods. What the liquid is at a point (terrain and WMO liquid, which
 * sxc.world's terrain_liquid leaves out), and the ripple field (kb: water-contact): a mod presses
 * ripples into it where something touches the water (an impact emitter: a drip, a waterfall's foot).
 *
 * The ripple field is Vulkan only, and only while its setting is on: ripple() answers false otherwise.
 *
 * Rivers (kb: rivers): a mod hands over the rivers of the map the player is on, each a path of nodes
 * with a width and a speed; the client draws them into a flow field around the player (on the shore
 * field's grid) that our water follows.
 */

#ifndef SXC_API_WATER_H
#define SXC_API_WATER_H

#include "SXC/Api/sxc.h"

#define SXC_WATER_SERVICE         "sxc.water"
#define SXC_WATER_SERVICE_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

/// What a stretch of river does (a node's kind, for the stretch after it).
#define SXC_WATER_RIVER_CALM   0u
#define SXC_WATER_RIVER_RAPIDS 1u
#define SXC_WATER_RIVER_FALLS  2u

/// A river's node: where it is (world yd), how wide the river is there (yd, bank to bank) and how fast it
/// flows (yd / s), and what the stretch after it does.
typedef struct sxc_water_river_node
{
    float    x, y, z;
    float    width;
    float    speed;
    uint32_t kind;
} sxc_water_river_node;

/// A river: its nodes (`node_count` of them from `first_node` in the nodes handed over, in order,
/// upstream first). `joins`: the id of the river its last node flows into (0 none); `branches_from`: the
/// one its first node leaves (0 none).
typedef struct sxc_water_river
{
    uint32_t id;
    uint32_t first_node;
    uint32_t node_count;
    uint32_t joins;
    uint32_t branches_from;
    uint32_t flags;   ///< reserved, 0
} sxc_water_river;

typedef struct sxc_water_api
{
    uint32_t struct_size;

    /// The liquid at a world point: a WMO's (where the point is inside the map object's group), else
    /// the terrain's (rivers, lakes, sea). Its surface height and its LiquidType.dbc ID; false where
    /// there is none.
    wxl_bool(WXL_CALL* liquid_at)(const wxl_vec3* position, float* out_height, uint32_t* out_liquid_type);

    /// One ripple this frame at world (x, y): a dip of `radius` yd pressed `depth` yd down (a raindrop:
    /// radius 0.25, depth 0.05). False when the field is off or this frame's stamps are full; points
    /// outside the field (64 yd around the camera) do nothing.
    wxl_bool(WXL_CALL* ripple)(float x, float y, float radius, float depth);

    /// The rivers of the map the player is on, replacing those handed over before (copied; none: 0, 0).
    /// Hand them over again when the map changes.
    void(WXL_CALL* set_rivers)(const sxc_water_river* rivers, uint32_t river_count, const sxc_water_river_node* nodes, uint32_t node_count);

    /// The rivers' flow at world (x, y) as the water draws it (yd / s, world x and y) and how much river is
    /// there (0..1); false outside the field (about 270 yd around the player) or before it is made.
    wxl_bool(WXL_CALL* flow_at)(float x, float y, float* out_flow_x, float* out_flow_y, float* out_weight);
} sxc_water_api;

#ifdef __cplusplus
}
#endif

#endif
