/*
 * sxc.sky: one provider (a mod) takes part in the client's sky (DayNight): the time the sky shows, the
 * world light, the glares, the M2 skyboxes, and the bodies drawn in the sun's and moons' place.
 *
 * Everything draws through the engine's Gx layer, so it works on every renderer. Directions are in
 * the world's frame (+X north, +Y west, +Z up). The sky puts its bodies SXC_SKY_DISTANCE from the
 * camera; a body's apparent size is its angular diameter.
 */

#ifndef SXC_API_SKY_H
#define SXC_API_SKY_H

#include "SXC/Api/sxc.h"

#define SXC_SKY_SERVICE         "sxc.sky"
#define SXC_SKY_SERVICE_VERSION 1

#define SXC_SKY_DISTANCE 12.0f

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sxc_sky_role
{
    SXC_SKY_SUN        = 0,
    SXC_SKY_WHITE_LADY = 1,
    SXC_SKY_BLUE_CHILD = 2
} sxc_sky_role;

/// The time the sky shows. In: the client's (the server's game time, or Map.dbc's override comes
/// after this); out: the provider's. The game clock itself is never changed.
typedef struct sxc_sky_time
{
    uint32_t struct_size;
    int32_t  time_of_day;     ///< minute of the day, 0..1439
    float    day_progression; ///< 0..1, what the light curves read
    float    day;             ///< the client's day counter (days since 1970, local midnight)
    // In only: the realm's date as the server sent it (the calendar's), and the client's own values
    // before anything changed them (server time, or Map.dbc's override that comes after).
    int32_t  year;            ///< e.g. 2026
    int32_t  month;           ///< 1..12
    int32_t  month_day;       ///< 1..31
    float    server_day_progression;
    float    server_day;
} sxc_sky_time;

typedef struct sxc_sky_glare
{
    wxl_vec3 position;  ///< world position (camera + direction x SXC_SKY_DISTANCE)
    float    scale_min; ///< the glare's size range (the moon glare follows the White Lady's size)
    float    scale_max;
    wxl_bool hide;      ///< not drawn this frame
} sxc_sky_glare;

typedef struct sxc_sky_skybox
{
    wxl_bool used;        ///< in: a skybox in this slot
    float    weight;      ///< in: its light zone's blend weight
    wxl_bool combine;     ///< in: LightSkybox 0x2, the procedural sky draws behind it
    wxl_bool time_of_day; ///< in: LightSkybox 0x1, its animation follows the time of day
    wxl_bool hide;        ///< out: not drawn (the procedural sky shows instead)
} sxc_sky_skybox;

/// After the client resolved the light colours, the light direction and the bodies' places.
typedef struct sxc_sky_lighting
{
    uint32_t       struct_size;
    wxl_vec3       light_direction;   ///< in / out: the world light, from the sky toward the ground
    uint32_t       diffuse_tint;      ///< out: 0xRRGGBB multiplied into the light's colour; 0xFFFFFF = none
    sxc_sky_glare  sun_glare;         ///< in / out
    sxc_sky_glare  moon_glare;        ///< in / out
    sxc_sky_skybox skyboxes[3];       ///< in / out
    wxl_bool       current_skybox;    ///< in: an override skybox (LightParams) is set
    wxl_bool       hide_current;      ///< out
} sxc_sky_lighting;

/// A stock body as the client placed it this frame.
typedef struct sxc_sky_stock
{
    uint32_t struct_size;
    wxl_vec3 position;    ///< world position
    float    scale;       ///< its quad's side (the size curve included), at SXC_SKY_DISTANCE
    float    base_scale;  ///< without the size curve
    uint32_t color;       ///< ARGB: the zone's light band tint and the storm fade (the Blue Child's is black: never set)
    float    period_days; ///< the Blue Child's cycle (1.7)
} sxc_sky_stock;

typedef struct sxc_sky_body
{
    uint32_t    struct_size;
    const char* texture;          ///< BLP path; loaded once and kept
    wxl_vec3    direction;        ///< toward the body
    float       size_degrees;     ///< angular diameter
    uint32_t    color;            ///< ARGB tint
    int32_t     blend;            ///< 0 alpha, 1 additive
    float       rotation_degrees; ///< the picture turned clockwise as seen (with zenith_up)
    wxl_bool    zenith_up;        ///< its top toward the zenith; otherwise facing the view like the client's
} sxc_sky_body;

typedef struct sxc_sky_provider
{
    uint32_t struct_size;
    void*    user;

    /// Every frame before the sky reads its time. May be NULL.
    void(WXL_CALL* time)(void* user, sxc_sky_time* io);

    /// Every frame after the lighting resolve. May be NULL.
    void(WXL_CALL* lighting)(void* user, sxc_sky_lighting* io);

    /**
     * @brief Draws bodies with draw_body. May be NULL.
     * @param pass  0: in the stock bodies' place (behind the dome); answer WXL_TRUE to skip the stock
     *              three. 1: after the dome (not washed out by it), before the clouds.
     */
    wxl_bool(WXL_CALL* bodies)(void* user, int32_t pass);
} sxc_sky_provider;

typedef struct sxc_sky_api
{
    uint32_t struct_size;

    /// The provider (copied); NULL removes it. One at a time: a second one replaces the first.
    wxl_result(WXL_CALL* set_provider)(const sxc_sky_provider* provider);

    /// Inside bodies() only.
    void(WXL_CALL* draw_body)(const sxc_sky_body* body);

    /// A stock body this frame.
    void(WXL_CALL* stock)(int32_t role, sxc_sky_stock* out);

    /// The camera position the sky is placed around this frame.
    wxl_vec3(WXL_CALL* camera)(void);
} sxc_sky_api;

#ifdef __cplusplus
}
#endif

#endif
