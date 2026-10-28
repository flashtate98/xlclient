#pragma once

#include <cstdint>

namespace DayNight {
    struct DNGlare;
    struct DNPlanet;
    struct DNInfo;
}

// The sky's seams (sxc.sky, SXC/Api/sxc_sky.h): each called from one marked line in the client's
// DayNight code. Without a provider each answers "nothing to do".
namespace SXC::Sky {
    // CGWorldFrame::UpdateDayNight and the time packet's UpdateTime, after the game clock was copied
    // into the day-night block (before Map.dbc's time override).
    void Time(DayNight::DNInfo* info);

    // RenderSky: the minute the M2 skyboxes animate on (the sky's, not the server's).
    int32_t SkyboxMinutes(int32_t clientMinutes);

    // The end of UpdateLighting.
    void Lighting();

    // The top of DNPlanet::Render: true = skip this (stock) body; ours draw on the last of the three.
    bool PlanetRender(DayNight::DNPlanet* planet);

    // The end of DNSky::Render: bodies drawn after the dome.
    void AfterDome();

    // The top of DNGlare::Render: true = hidden this frame.
    bool GlareHidden(const DayNight::DNGlare* glare);

    // The top of CGameTime::IsDaytime for the game clock: false = the client's own answer.
    bool IsDaytime(bool& daytime);

    // A body the sky drew this frame (the provider's, or the stock sun and moons), for reflections: the
    // direction toward it (unit), its angular diameter (degrees), its colour (0..1).
    struct BodySeen {
        float dir[3];
        float sizeDegrees;
        float colour[3];
    };

    // This frame's bodies (since the last lighting update), up to `max`; returns how many.
    uint32_t BodiesSeen(BodySeen* out, uint32_t max);
}
