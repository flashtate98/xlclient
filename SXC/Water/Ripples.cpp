// The ripple field's stamps (SXC/Water/Ripples.h).
#include "SXC/Water/Ripples.h"
#include "SXC/Water/Water.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include "Engine/Source/Tempest/C3Vector.h"
#include "SXC/Settings/Settings.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "WoW/Source/WorldClient/MapWeather.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    constexpr uint32_t kStamps = 64;      // gxvk::vk::ripples::kStamps
    constexpr float kRainReach = 25.0f;   // yd around the camera the rain stamps
    constexpr float kWeatherKnee = 0.25f; // the engine's dead zone (SXC/World)

    SXC::Settings::Bool& RipplesOn() {
        static SXC::Settings::Bool s_setting("Liquid", "Ripples", true, "Ripples",
                                             "Our water ripples where it is touched (units wading and swimming, rain): a field of small waves "
                                             "around the camera. Off: the stock rings.");
        return s_setting;
    }

    SXC::Settings::Bool& RainOn() {
        static SXC::Settings::Bool s_setting("Liquid", "RainRipples", true, "Rain ripples", "Raindrops ripple our water (by the weather's rain).");
        return s_setting;
    }

    SXC::Settings::Float& Strength() {
        static SXC::Settings::Float s_setting("Liquid", "RippleStrength", 0.5f, 0.0f, 4.0f, "Ripple strength", "x the ripples' slopes on the water.");
        return s_setting;
    }

    SXC::Settings::Float& Speed() {
        static SXC::Settings::Float s_setting("Liquid", "RippleSpeed", 2.0f, 0.3f, 4.0f, "Ripple speed", "yd / s the ripples spread at.");
        return s_setting;
    }

    SXC::Settings::Float& Life() {
        static SXC::Settings::Float s_setting("Liquid", "RippleLife", 15.0f, 0.3f, 15.0f, "Ripple life", "s until a ripple has faded to a tenth.");
        return s_setting;
    }

    SXC::Settings::Float& UnitDepth() {
        static SXC::Settings::Float s_setting("Liquid", "RippleUnitDepth", 0.5f, 0.0f, 0.5f, "Unit ripple depth",
                                              "yd a unit's ripple presses the surface down (x the client's own strength for it).");
        return s_setting;
    }

    SXC::Settings::Float& Standing() {
        static SXC::Settings::Float s_setting("Liquid", "RippleStanding", 0.35f, 0.0f, 1.0f, "Standing ripples",
                                              "x a unit's ripple while it stands or treads water (moving: 1).");
        return s_setting;
    }

    SXC::Settings::Float& RainDepth() {
        static SXC::Settings::Float s_setting("Liquid", "RainRippleDepth", 0.05f, 0.0f, 0.3f, "Rain drop depth",
                                              "yd a raindrop presses the surface down (a few cells wide).");
        return s_setting;
    }

    SXC::Settings::Float& RainDrops() {
        static SXC::Settings::Float s_setting("Liquid", "RainRipplesPerSecond", 600.0f, 0.0f, 3000.0f, "Rain drops / s",
                                              "Drops a second in full rain within 25 yd of the camera.");
        return s_setting;
    }

    float s_stamp[kStamps][4] = {};
    uint32_t s_count = 0;           // this frame's
    uint32_t s_shownUnits = 0, s_shownRain = 0, s_dropped = 0;   // counting this second
    uint32_t s_secondUnits = 0, s_secondRain = 0, s_secondDropped = 0;   // the last second's
    float s_rainCarry = 0.0f;
    uint32_t s_seed = 0x9E3779B9u;

    float Random() {
        s_seed = s_seed * 1664525u + 1013904223u;
        return static_cast<float>(s_seed >> 8) * (1.0f / 16777216.0f);
    }

    // The weather's rain (0..1 after the dead zone), 0 when it isn't raining.
    float RainNow() {
        const Weather* weather = ::World::s_weather;

        if (!weather || static_cast<int32_t>(weather->GetType()) != 1 || weather->m_intensity <= kWeatherKnee) {
            return 0.0f;
        }

        return std::min((weather->m_intensity - kWeatherKnee) / (1.0f - kWeatherKnee), 1.0f);
    }

    bool Push(float x, float y, float radius, float depth) {
        if (s_count >= kStamps) {
            s_dropped++;
            return false;
        }

        s_stamp[s_count][0] = x;
        s_stamp[s_count][1] = y;
        s_stamp[s_count][2] = radius;
        s_stamp[s_count][3] = depth;
        s_count++;
        return true;
    }
}

namespace SXC::Water {
    bool RippleFromUnit(const C3Vector& position, float size, float strength, bool moving) {
        if (!RipplesOn().Get() || !Active()) {
            return false;
        }

        // The client's sizes run 0.33 .. 1.67 (a third of the unit's scale): the dip a bit narrower.
        const float radius = 0.15f + std::clamp(size, 0.1f, 2.0f) * 0.35f;
        Push(position.x, position.y, radius, UnitDepth().Get() * std::clamp(strength, 0.0f, 1.0f) * (moving ? 1.0f : Standing().Get()));
        s_shownUnits++;
        return true;
    }

    void FillRipples(Waves& waves, const C3Vector& camera, bool frame) {
        waves.ripplesOn = RipplesOn().Get();
        waves.rippleSpeed = Speed().Get();
        waves.rippleLife = Life().Get();
        waves.rippleStrength = Strength().Get();

        if (!frame) {
            return;
        }

        using Clock = std::chrono::steady_clock;
        static Clock::time_point s_last = Clock::now();
        const Clock::time_point now = Clock::now();
        const float dt = std::chrono::duration<float>(now - s_last).count();
        s_last = now;

        if (!waves.ripplesOn) {
            s_count = 0;
            return;
        }

        // The rain's drops, after the units' (they come first): small, but two cells or more across, or the
        // field can't hold them (a one-cell dip is smoothed away by the step and the slope's sampling).
        const float rain = RainOn().Get() ? RainNow() : 0.0f;
        s_rainCarry += rain * RainDrops().Get() * (dt > 0.0f && dt < 0.5f ? dt : 0.0f);

        while (s_rainCarry >= 1.0f) {
            s_rainCarry -= 1.0f;
            const float angle = Random() * 6.2831853f, reach = std::sqrt(Random()) * kRainReach;

            if (!Push(camera.x + std::cos(angle) * reach, camera.y + std::sin(angle) * reach, 0.2f + 0.1f * Random(), RainDepth().Get() * (0.6f + 0.8f * Random()))) {
                s_rainCarry = 0.0f;
                break;
            }

            s_shownRain++;
        }

        waves.rippleElapsed = dt;
        waves.rippleStamps = s_count;
        waves.rippleStamp = s_stamp;

        // What the panel shows: the last second's counts.
        static Clock::time_point s_shownAt = now;

        if (now - s_shownAt > std::chrono::seconds(1)) {
            s_shownAt = now;
            s_secondUnits = s_shownUnits;
            s_secondRain = s_shownRain;
            s_secondDropped = s_dropped;
            s_shownUnits = s_shownRain = s_dropped = 0;
        }

        // The stamps are copied by the device at this frame's capture; the next frame starts empty.
        s_count = 0;
    }

    void DrawRipplePanel() {
        bool on = RipplesOn().Get();

        if (ImGui::Checkbox("Ripples", &on)) {
            RipplesOn().Set(on);
        }

        ImGui::SameLine();
        bool rain = RainOn().Get();

        if (ImGui::Checkbox("Rain ripples", &rain)) {
            RainOn().Set(rain);
        }

        float strength = Strength().Get(), speed = Speed().Get(), life = Life().Get(), depth = UnitDepth().Get(), standing = Standing().Get();
        float rainDepth = RainDepth().Get(), drops = RainDrops().Get();

        if (ImGui::SliderFloat("Ripple strength", &strength, 0.0f, 4.0f)) {
            Strength().Set(strength);
        }

        if (ImGui::SliderFloat("Ripple speed (yd/s)", &speed, 0.3f, 4.0f)) {
            Speed().Set(speed);
        }

        if (ImGui::SliderFloat("Ripple life (s)", &life, 0.3f, 15.0f)) {
            Life().Set(life);
        }

        if (ImGui::SliderFloat("Unit ripple depth (yd)", &depth, 0.0f, 0.5f, "%.3f")) {
            UnitDepth().Set(depth);
        }

        if (ImGui::SliderFloat("Standing ripples (x)", &standing, 0.0f, 1.0f)) {
            Standing().Set(standing);
        }

        if (ImGui::SliderFloat("Rain drop depth (yd)", &rainDepth, 0.0f, 0.3f, "%.3f")) {
            RainDepth().Set(rainDepth);
        }

        if (ImGui::SliderFloat("Rain drops / s", &drops, 0.0f, 3000.0f, "%.0f")) {
            RainDrops().Set(drops);
        }

        ImGui::TextDisabled("rain now %.2f; stamps / s: units %u, rain %u, dropped (over 64 a frame) %u", RainNow(), s_secondUnits, s_secondRain, s_secondDropped);
    }
}
