// sxc.sky (SXC/Api/sxc_sky.h) and the seams in DayNight that call it (SXC/Sky/Sky.h).

#include "SXC/Sky/Sky.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "SXC/Api/sxc_sky.h"
#include "WoW/Source/Game/GameTime.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"

namespace {
    constexpr float kPi = 3.14159265f;

    bool s_haveProvider;
    sxc_sky_provider s_provider;

    // This frame's answers.
    bool s_timeSet;          // the provider changed the time
    int32_t s_skyMinute;     // its minute of the day
    bool s_hideSunGlare;
    bool s_hideMoonGlare;
    bool s_inBodies;         // draw_body may be called

    // The bodies drawn this frame (SXC::Sky::BodiesSeen: our water reflects them).
    constexpr uint32_t kMaxSeen = 8;
    SXC::Sky::BodySeen s_seen[kMaxSeen];
    uint32_t s_seenCount = 0;

    void Seen(float x, float y, float z, float sizeDegrees, uint32_t argb) {
        const float length = std::sqrt(x * x + y * y + z * z);

        if (s_seenCount >= kMaxSeen || length < 1e-6f || sizeDegrees <= 0.0f) {
            return;
        }

        SXC::Sky::BodySeen& b = s_seen[s_seenCount++];
        b.dir[0] = x / length;
        b.dir[1] = y / length;
        b.dir[2] = z / length;
        b.sizeDegrees = sizeDegrees;
        b.colour[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
        b.colour[1] = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
        b.colour[2] = static_cast<float>(argb & 0xFF) / 255.0f;
    }
    bool s_bodiesReplace;    // pass 0 answered: the stock three are skipped

    // The bodies' textures by path, each in a planet of its own (DNPlanet::Initialize loads it the
    // stock way); a planet also draws the client-like bodies.
    std::map<std::string, DayNight::DNPlanet>& Planets() {
        static std::map<std::string, DayNight::DNPlanet> s_planets;
        return s_planets;
    }

    DayNight::DNPlanet* PlanetFor(const char* texture) {
        auto found = Planets().find(texture);

        if (found == Planets().end()) {
            DayNight::DNPlanet planet = {};
            found = Planets().emplace(texture, planet).first;
            found->second.Initialize(texture);
        }

        return &found->second;
    }

    CImVector Color(uint32_t argb) {
        CImVector c;
        c.b = static_cast<uint8_t>(argb & 0xFF);
        c.g = static_cast<uint8_t>((argb >> 8) & 0xFF);
        c.r = static_cast<uint8_t>((argb >> 16) & 0xFF);
        c.a = static_cast<uint8_t>((argb >> 24) & 0xFF);
        return c;
    }

    uint32_t Argb(const CImVector& c) {
        return static_cast<uint32_t>(c.a) << 24 | static_cast<uint32_t>(c.r) << 16 | static_cast<uint32_t>(c.g) << 8 | c.b;
    }

    C3Vector Unit(const wxl_vec3& v) {
        const float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        return length > 1.0e-6f ? C3Vector(v.x / length, v.y / length, v.z / length) : C3Vector(0.0f, 0.0f, 1.0f);
    }

    // A body facing the view, drawn exactly like the stock ones (DNPlanet::Render: its quad, the
    // horizon clip and fade, alpha blending).
    void DrawLikeStock(const sxc_sky_body& body) {
        DayNight::DNPlanet* planet = PlanetFor(body.texture);
        const C3Vector dir = Unit(body.direction);
        const C3Vector& camera = DayNight::GetInfo()->m_cameraPos;
        planet->m_position = C3Vector(camera.x + dir.x * SXC_SKY_DISTANCE, camera.y + dir.y * SXC_SKY_DISTANCE, camera.z + dir.z * SXC_SKY_DISTANCE);
        planet->m_scale = 2.0f * SXC_SKY_DISTANCE * tanf(body.size_degrees * 0.5f * kPi / 180.0f);
        planet->m_color = Color(body.color);
        planet->Render();
    }

    // A body whose top points to the zenith, turned by its rotation, alpha or additive: the quad built
    // here, faded per corner over +-1 degree around the horizon.
    void DrawOwn(const sxc_sky_body& body) {
        DayNight::DNPlanet* planet = PlanetFor(body.texture);
        CGxTex* gxTex = TextureGetGxTex(planet->m_texture, 0, nullptr);

        if (!gxTex) {
            return;
        }

        const C3Vector L = Unit(body.direction);
        C3Vector right(L.y, -L.x, 0.0f);
        const float flat = sqrtf(right.x * right.x + right.y * right.y);
        right = flat > 1.0e-5f ? C3Vector(right.x / flat, right.y / flat, 0.0f) : C3Vector(0.0f, -1.0f, 0.0f);
        const C3Vector up(right.y * L.z - right.z * L.y, right.z * L.x - right.x * L.z, right.x * L.y - right.y * L.x);
        const float a = body.rotation_degrees * kPi / 180.0f;
        const float ca = cosf(a);
        const float sa = sinf(a);
        // Clockwise: the picture's right swings down.
        const C3Vector r2(ca * right.x - sa * up.x, ca * right.y - sa * up.y, ca * right.z - sa * up.z);
        const C3Vector u2(sa * right.x + ca * up.x, sa * right.y + ca * up.y, sa * right.z + ca * up.z);
        const float half = SXC_SKY_DISTANCE * tanf(body.size_degrees * 0.5f * kPi / 180.0f);

        // Top-left, top-right, bottom-left, bottom-right (a strip), relative to the camera: the sky's
        // view has no translation (DNPlanet::Render does the same with its billboard).
        static const float kCorner[4][2] = { { -1.0f, 1.0f }, { 1.0f, 1.0f }, { -1.0f, -1.0f }, { 1.0f, -1.0f } };
        C3Vector verts[4];
        C2Vector texCoords[4];
        CImVector colors[4];
        const CImVector color = Color(body.color);

        for (int k = 0; k < 4; k++) {
            const float cx = kCorner[k][0] * half;
            const float cy = kCorner[k][1] * half;
            verts[k] = C3Vector(L.x * SXC_SKY_DISTANCE + cx * r2.x + cy * u2.x, L.y * SXC_SKY_DISTANCE + cx * r2.y + cy * u2.y,
                                L.z * SXC_SKY_DISTANCE + cx * r2.z + cy * u2.z);
            texCoords[k] = C2Vector((kCorner[k][0] + 1.0f) * 0.5f, (1.0f - kCorner[k][1]) * 0.5f);
            const float length = sqrtf(verts[k].x * verts[k].x + verts[k].y * verts[k].y + verts[k].z * verts[k].z);
            const float sine = length > 1.0e-6f ? verts[k].z / length : 0.0f;
            float fade = (sine + 0.01745f) / 0.0349f;
            fade = fade < 0.0f ? 0.0f : (fade > 1.0f ? 1.0f : fade);
            colors[k] = color;
            colors[k].a = static_cast<uint8_t>(color.a * fade);
        }

        static const uint16_t kIndices[4] = { 0, 1, 2, 3 };
        g_theGxDevicePtr->RsPush();
        g_theGxDevicePtr->XformPush(GxXform_World);
        g_theGxDevicePtr->m_xforms[GxXform_World].Identity();
        GxRsSet(GxRs_BlendingMode, body.blend == 1 ? GxBlend_Add : GxBlend_Alpha);
        GxUpdateAlphaRef(GxRs_AlphaRef);
        GxRsSet(GxRs_Lighting, 0);
        GxRsSet(GxRs_Fog, 0);
        GxRsSet(GxRs_DepthWrite, 0);
        GxRsSet(GxRs_Culling, 0);
        g_theGxDevicePtr->RsSet(GxRs_Texture0, gxTex);
        GxRsSet(GxRs_ColorOp0, 0);
        GxRsSet(GxRs_AlphaOp0, 0);
        GxPrimVertexPtr(4, verts, sizeof(C3Vector), nullptr, 0, colors, sizeof(CImVector), texCoords, sizeof(C2Vector), nullptr, 0);
        GxPrimIndexPtr(4, kIndices);

        CGxBatch batch;
        batch.m_primType = GxPrim_TriangleStrip;
        batch.m_start = 0;
        batch.m_count = 4;
        batch.m_minIndex = 0;
        batch.m_maxIndex = 3;
        g_theGxDevicePtr->Draw(&batch, 1);

        GxXformPop(GxXform_World);
        g_theGxDevicePtr->RsPop();
    }

    // --- the service ----------------------------------------------------------------------------------

    wxl_result WXL_CALL SetProvider(const sxc_sky_provider* provider) {
        if (provider && provider->struct_size < sizeof(sxc_sky_provider)) {
            return WXL_E_INVALID;
        }

        s_haveProvider = provider != nullptr;
        s_provider = provider ? *provider : sxc_sky_provider{};
        s_timeSet = false;
        s_hideSunGlare = s_hideMoonGlare = false;
        return WXL_OK;
    }

    void WXL_CALL DrawBody(const sxc_sky_body* body) {
        if (!s_inBodies || !body || body->struct_size < sizeof(sxc_sky_body) || !body->texture || !*body->texture || body->size_degrees <= 0.0f) {
            return;
        }

        Seen(body->direction.x, body->direction.y, body->direction.z, body->size_degrees, body->color);

        if (body->zenith_up || body->blend == 1 || body->rotation_degrees != 0.0f) {
            DrawOwn(*body);
        } else {
            DrawLikeStock(*body);
        }
    }

    void WXL_CALL Stock(int32_t role, sxc_sky_stock* out) {
        if (!out || out->struct_size < sizeof(sxc_sky_stock)) {
            return;
        }

        const DayNight::DNPlanet* planets[3] = { &DayNight::g_planets.m_sun, &DayNight::g_planets.m_moon, &DayNight::g_planets.m_moon2 };

        if (role < 0 || role > 2) {
            return;
        }

        const DayNight::DNPlanet& p = *planets[role];
        out->position = wxl_vec3{ p.m_position.x, p.m_position.y, p.m_position.z };
        out->scale = p.m_scale;
        out->base_scale = p.m_baseScale;
        out->color = Argb(p.m_color);
        out->period_days = p.m_period;
    }

    wxl_vec3 WXL_CALL Camera() {
        const C3Vector& camera = DayNight::GetInfo()->m_cameraPos;
        return wxl_vec3{ camera.x, camera.y, camera.z };
    }

    void Glare(const DayNight::DNGlare& glare, sxc_sky_glare& out) {
        out.position = wxl_vec3{ glare.m_position.x, glare.m_position.y, glare.m_position.z };
        out.scale_min = glare.m_scaleMin;
        out.scale_max = glare.m_scaleMax;
        out.hide = WXL_FALSE;
    }

    void Apply(DayNight::DNGlare& glare, const sxc_sky_glare& in) {
        glare.m_position = C3Vector(in.position.x, in.position.y, in.position.z);
        glare.m_scaleMin = in.scale_min;
        glare.m_scaleMax = in.scale_max;
    }

    void Tint(CImVector& color, uint32_t tint) {
        color.r = static_cast<uint8_t>((color.r * ((tint >> 16) & 0xFF) + 127) / 255);
        color.g = static_cast<uint8_t>((color.g * ((tint >> 8) & 0xFF) + 127) / 255);
        color.b = static_cast<uint8_t>((color.b * (tint & 0xFF) + 127) / 255);
    }
}

namespace SXC::Sky {
    const sxc_sky_api* Api() {
        static const sxc_sky_api s_api = { sizeof(sxc_sky_api), SetProvider, DrawBody, Stock, Camera };
        return &s_api;
    }

    void Time(DayNight::DNInfo* info) {
        s_timeSet = false;

        if (!s_haveProvider || !s_provider.time || !info) {
            return;
        }

        sxc_sky_time time = { sizeof(sxc_sky_time), static_cast<int32_t>(info->m_timeOfDay), info->m_dayProgression, info->m_day };
        time.year = static_cast<int32_t>(g_clientGameTime.m_year) + 2000;
        time.month = static_cast<int32_t>(g_clientGameTime.m_month) + 1;
        time.month_day = static_cast<int32_t>(g_clientGameTime.m_monthDay) + 1;
        time.server_day_progression = info->m_dayProgression;
        time.server_day = info->m_day;
        const sxc_sky_time before = time;
        s_provider.time(s_provider.user, &time);

        if (time.time_of_day == before.time_of_day && time.day_progression == before.day_progression && time.day == before.day) {
            return;
        }

        s_timeSet = true;
        s_skyMinute = ((time.time_of_day % 1440) + 1440) % 1440;
        info->m_timeOfDay = static_cast<uint32_t>(s_skyMinute);
        info->m_dayProgression = time.day_progression;
        info->m_day = time.day;
    }

    int32_t SkyboxMinutes(int32_t clientMinutes) {
        return s_timeSet ? s_skyMinute : clientMinutes;
    }

    void Lighting() {
        s_hideSunGlare = s_hideMoonGlare = false;
        s_seenCount = 0;   // a new frame's bodies follow

        if (!s_haveProvider || !s_provider.lighting) {
            return;
        }

        DayNight::DNInfo& info = *DayNight::GetInfo();
        sxc_sky_lighting io = {};
        io.struct_size = sizeof(io);
        io.light_direction = wxl_vec3{ info.m_light1.m_dir.x, info.m_light1.m_dir.y, info.m_light1.m_dir.z };
        io.diffuse_tint = 0xFFFFFF;
        Glare(DayNight::g_sunGlare, io.sun_glare);
        Glare(DayNight::g_moonGlare, io.moon_glare);

        for (int i = 0; i < 3; i++) {
            sxc_sky_skybox& slot = io.skyboxes[i];
            slot.used = info.m_blendSkybox[i] != nullptr;
            slot.weight = info.m_blendSkyboxWeight[i];
            slot.combine = info.m_blendSkyboxFlags[i] != 0;
            slot.time_of_day = slot.used && (info.m_blendSkybox[i]->m_flags & 1);
        }

        io.current_skybox = info.m_skybox != nullptr;
        s_provider.lighting(s_provider.user, &io);

        // SetColors copies light1's direction into light0 before SetDirection: write both.
        const C3Vector dir(io.light_direction.x, io.light_direction.y, io.light_direction.z);
        info.m_light1.m_dir = dir;
        info.m_light0.m_dir = dir;

        if ((io.diffuse_tint & 0xFFFFFF) != 0xFFFFFF) {
            Tint(info.m_light1.m_diffuse, io.diffuse_tint);
            Tint(info.m_light0.m_diffuse, io.diffuse_tint);
        }

        Apply(DayNight::g_sunGlare, io.sun_glare);
        Apply(DayNight::g_moonGlare, io.moon_glare);
        s_hideSunGlare = io.sun_glare.hide != 0;
        s_hideMoonGlare = io.moon_glare.hide != 0;

        for (int i = 0; i < 3; i++) {
            if (!io.skyboxes[i].hide || !info.m_blendSkybox[i]) {
                continue;
            }

            if (info.m_skybox == info.m_blendSkybox[i]) {
                info.m_skybox = nullptr;
                info.m_skyboxWeight = 0.0f;
            }

            info.m_blendSkybox[i] = nullptr;
            info.m_blendSkyboxWeight[i] = 0.0f;
            info.m_blendSkyboxFlags[i] = 0;
        }

        if (io.hide_current) {
            info.m_skybox = nullptr;
            info.m_skyboxWeight = 0.0f;
        }
    }

    bool PlanetRender(DayNight::DNPlanet* planet) {
        const bool stock = planet == &DayNight::g_planets.m_sun || planet == &DayNight::g_planets.m_moon || planet == &DayNight::g_planets.m_moon2;

        if (!stock || !s_haveProvider || !s_provider.bodies) {
            // A stock body drawn the stock way: seen as it stands (its quad's angular size at its distance).
            if (stock && planet->m_texture) {
                const C3Vector& camera = DayNight::GetInfo()->m_cameraPos;
                const C3Vector d(planet->m_position.x - camera.x, planet->m_position.y - camera.y, planet->m_position.z - camera.z);
                const float distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                const float size = distance > 1e-3f ? 2.0f * std::atan(planet->m_scale * 0.5f / distance) * 57.29578f : 0.0f;
                Seen(d.x, d.y, d.z, size, Argb(planet->m_color));
            }

            return false;
        }

        // The client draws the three in a row: the provider's go where the first one was, and say
        // whether the stock ones are skipped.
        if (planet == &DayNight::g_planets.m_sun) {
            s_inBodies = true;
            s_bodiesReplace = s_provider.bodies(s_provider.user, 0) != 0;
            s_inBodies = false;
        }

        return s_bodiesReplace;
    }

    void AfterDome() {
        if (!s_haveProvider || !s_provider.bodies) {
            return;
        }

        s_inBodies = true;
        s_provider.bodies(s_provider.user, 1);
        s_inBodies = false;
    }

    bool GlareHidden(const DayNight::DNGlare* glare) {
        return (glare == &DayNight::g_sunGlare && s_hideSunGlare) || (glare == &DayNight::g_moonGlare && s_hideMoonGlare);
    }

    uint32_t BodiesSeen(BodySeen* out, uint32_t max) {
        const uint32_t n = s_seenCount < max ? s_seenCount : max;

        for (uint32_t i = 0; i < n; i++) {
            out[i] = s_seen[i];
        }

        return n;
    }

    bool IsDaytime(bool& daytime) {
        if (!s_timeSet) {
            return false;
        }

        // The client's own rule, on a copy of the clock set to the sky's minute.
        CGameTime time;
        time.SetHourAndMinutes(s_skyMinute);
        daytime = time.IsDaytime();
        return true;
    }
}
