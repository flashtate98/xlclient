/*
 * sxc.water: our water (SXC/Water) for mods. What the liquid is at a point (terrain and WMO liquid, which
 * sxc.world's terrain_liquid leaves out), and the ripple field (kb: water-contact): a mod presses
 * ripples into it where something touches the water (an impact emitter: a drip, a waterfall's foot).
 *
 * The ripple field is Vulkan only, and only while its setting is on: ripple() answers false otherwise.
 */

#ifndef SXC_API_WATER_H
#define SXC_API_WATER_H

#include "SXC/Api/sxc.h"

#define SXC_WATER_SERVICE         "sxc.water"
#define SXC_WATER_SERVICE_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

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
} sxc_water_api;

#ifdef __cplusplus
}
#endif

#endif
