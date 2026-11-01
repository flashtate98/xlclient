// The ripple field's stamps (SXC/Water/Ripples.h).
#include "SXC/Water/Ripples.h"
#include "SXC/Water/Water.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "SXC/Bones/Bodies.h"
#include "SXC/Settings/Settings.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/MapWeather.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    constexpr uint32_t kStamps = 256;     // gxvk::vk::ripples::kStamps
    constexpr float kRainReach = 25.0f;   // yd around the camera the rain stamps
    constexpr float kWeatherKnee = 0.25f; // the engine's dead zone (SXC/World)
    constexpr float kBodyReach = 40.0f;   // yd from the camera: units farther ripple nothing (the field shows 30)
    constexpr float kBodyStep = 0.06f;    // yd a body part moves through the surface between its stamps
    constexpr uint32_t kForget = 600;     // frames out of the water before a unit's traces are forgotten

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

    SXC::Settings::Bool& ByBone() {
        static SXC::Settings::Bool s_setting("Liquid", "RipplesByBone", true, "Ripples by body part",
                                             "Units' feet, hands, hips, chest and head ripple the water where they move through the surface "
                                             "(SXC/Bones). Off: one ripple at the unit, when the client makes one.");
        return s_setting;
    }

    SXC::Settings::Float& BoneDepth() {
        static SXC::Settings::Float s_setting("Liquid", "RippleBoneDepth", 0.15f, 0.0f, 1.0f, "Body ripple depth",
                                              "yd a body part presses the surface down per yd it moves through it.");
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
    uint32_t s_shownBody = 0, s_secondBody = 0, s_secondSwimmers = 0, s_swimmersShown = 0;   // swimmers: unit-frames in the water

    // Units in the water this frame (CGUnit_C::CreateRipple): the surface's height at each.
    std::unordered_map<uint64_t, float> s_inWater;

    // Where a unit's body parts were last frame, and how far each has moved through the surface since its
    // last stamp.
    struct Trace {
        const CM2Model* model = nullptr;
        C3Vector last[SXC::Bones::kRoles];
        bool have[SXC::Bones::kRoles] = {};
        float carry[SXC::Bones::kRoles] = {};
        uint32_t away = 0;   // frames since it was in the water
    };
    std::unordered_map<uint64_t, Trace> s_traces;
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

    bool Push(float x, float y, float radius, float depth);

    // The units in the water: each body part near the surface stamps the field as it moves through it, by
    // the way it moved (along and across the surface) and by how near its middle is to the surface.
    void FillBodies(const C3Vector& camera) {
        SXC::Bones::Prune();

        for (auto it = s_traces.begin(); it != s_traces.end();) {
            Trace& t = it->second;

            if (!s_inWater.count(it->first)) {
                std::fill(std::begin(t.have), std::end(t.have), false);

                if (++t.away > kForget) {
                    it = s_traces.erase(it);
                    continue;
                }
            }

            ++it;
        }

        const float depthPerYard = BoneDepth().Get();

        for (const auto& [guid, surface] : s_inWater) {
            CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(WOWGUID{ guid }, TYPE_UNIT, __FILE__, __LINE__));
            const SXC::Bones::Body* body = unit ? SXC::Bones::Of(unit, guid) : nullptr;

            if (!body) {
                continue;
            }

            const C3Vector at = unit->GetPosition();
            const float cx = at.x - camera.x, cy = at.y - camera.y;
            Trace& t = s_traces[guid];
            t.away = 0;

            if (t.model != body->model) {
                t = Trace{};
                t.model = body->model;
            }

            if (cx * cx + cy * cy > kBodyReach * kBodyReach || !SXC::Bones::Current(body->model)) {
                std::fill(std::begin(t.have), std::end(t.have), false);
                continue;
            }

            s_secondSwimmers++;
            const uint32_t bones = body->model->m_shared->m_data->m_bones.m_count;

            for (int k = 0; k < SXC::Bones::kRoles; k++) {
                const SXC::Bones::Point& p = body->points[k];

                if (!p.known || p.bone >= bones) {
                    t.have[k] = false;
                    continue;
                }

                // A ball around the point: a foot's sits on its sole, a body part's around its point.
                const bool isFoot = k < SXC::Bones::kFeet;
                const float radius = std::clamp(isFoot ? body->radius : body->radius * p.size, 0.12f, 1.5f);
                C3Vector c = SXC::Bones::Where(body->model, p);
                c.z += isFoot ? radius : 0.0f;
                const float touch = 1.0f - std::fabs(c.z - surface) / radius;

                if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || touch <= 0.0f) {
                    t.have[k] = false;
                    t.carry[k] = 0.0f;
                    continue;
                }

                if (t.have[k]) {
                    const float dx = c.x - t.last[k].x, dy = c.y - t.last[k].y, dz = c.z - t.last[k].z;
                    const float moved = std::sqrt(dx * dx + dy * dy) + std::fabs(dz);

                    // A jump (a teleport, a model moved): no stroke from it.
                    t.carry[k] += moved < 3.0f ? moved * touch : 0.0f;
                }

                t.last[k] = c;
                t.have[k] = true;

                if (t.carry[k] >= kBodyStep) {
                    if (Push(c.x, c.y, std::clamp(radius * 0.8f, 0.15f, 1.0f), std::min(depthPerYard * t.carry[k], 0.5f))) {
                        s_shownBody++;
                    }

                    t.carry[k] = 0.0f;
                }
            }
        }

        s_inWater.clear();
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

    bool UnitInWater(CGUnit_C* unit, float surface) {
        if (!RipplesOn().Get() || !ByBone().Get() || !Active() || !unit) {
            return false;
        }

        // Its body parts (attachment points) found: every model with a skeleton has some.
        const uint64_t guid = unit->m_obj->m_guid.m_guid;
        const SXC::Bones::Body* body = SXC::Bones::Of(unit, guid);

        if (!body || !body->partsFound) {
            return false;
        }

        bool any = false;

        for (const SXC::Bones::Point& p : body->points) {
            any = any || p.known;
        }

        if (!any) {
            return false;
        }

        s_inWater[guid] = surface;
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
            s_inWater.clear();
            return;
        }

        // The units' bodies first (they stamp often and small), then the rain.
        FillBodies(camera);

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
            s_secondBody = s_shownBody;
            s_shownUnits = s_shownRain = s_dropped = s_shownBody = 0;
            s_swimmersShown = s_secondSwimmers;
            s_secondSwimmers = 0;
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
        bool byBone = ByBone().Get();

        if (ImGui::Checkbox("By body part", &byBone)) {
            ByBone().Set(byBone);
        }

        ImGui::SameLine();
        bool rain = RainOn().Get();

        if (ImGui::Checkbox("Rain ripples", &rain)) {
            RainOn().Set(rain);
        }

        float strength = Strength().Get(), speed = Speed().Get(), life = Life().Get(), depth = UnitDepth().Get(), standing = Standing().Get();
        float rainDepth = RainDepth().Get(), drops = RainDrops().Get(), boneDepth = BoneDepth().Get();

        if (ImGui::SliderFloat("Ripple strength", &strength, 0.0f, 4.0f)) {
            Strength().Set(strength);
        }

        if (ImGui::SliderFloat("Ripple speed (yd/s)", &speed, 0.3f, 4.0f)) {
            Speed().Set(speed);
        }

        if (ImGui::SliderFloat("Ripple life (s)", &life, 0.3f, 15.0f)) {
            Life().Set(life);
        }

        if (ImGui::SliderFloat("Body ripple depth (yd / yd)", &boneDepth, 0.0f, 1.0f, "%.3f")) {
            BoneDepth().Set(boneDepth);
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

        ImGui::TextDisabled("rain now %.2f; stamps / s: body parts %u (unit-frames in the water %u), unit events %u, rain %u, dropped (over %u a frame) %u",
                            RainNow(), s_secondBody, s_swimmersShown, s_secondUnits, s_secondRain, kStamps, s_secondDropped);
    }
}
