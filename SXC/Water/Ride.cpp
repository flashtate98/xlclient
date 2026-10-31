// Swimmers riding the waves (SXC/Water/Ride.h).
#include "SXC/Water/Ride.h"
#include "SXC/Water/Water.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "SXC/Settings/Settings.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/Object/Types.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/Map.h"

namespace {
    SXC::Settings::Bool& RideOn() {
        static SXC::Settings::Bool s_setting("Liquid", "RideWaves", true, "Swimmers ride the waves",
                                             "Units swimming at the surface are drawn rising, sinking and tilting with our water's waves "
                                             "(only drawn so: their real position stays on the still surface).");
        return s_setting;
    }

    SXC::Settings::Float& Tilt() {
        static SXC::Settings::Float s_setting("Liquid", "RideTilt", 0.6f, 0.0f, 1.0f, "Swimmers tilt with the waves",
                                              "How much of the wave's slope a swimmer leans with (0 upright, 1 lying on the surface).");
        return s_setting;
    }

    SXC::Settings::Float& CameraFollows() {
        static SXC::Settings::Float s_setting("Liquid", "RideCamera", 1.0f, 0.0f, 1.0f, "The camera rides the waves",
                                              "How much of your own rise and fall on the waves the camera follows (0 it stays still).");
        return s_setting;
    }

    constexpr uint32_t kPoints = 64;      // gxvk::vk::water::kQueryPoints
    constexpr float kRange = 150.0f;      // yd from the camera (the surface's movement fades from 150 yd)
    // How deep a swimmer may be (the still surface above its position, yd) and still ride: fully to 3 yd (the
    // position of a surface swimmer lies under the water), not from 6 (diving).
    constexpr float kRideDepth = 3.0f, kDiveDepth = 6.0f;
    constexpr float kSmooth = 0.08f;      // s: how fast a rider follows its answers
    constexpr float kStale = 0.3f;        // s: no answer for this long, it settles back

    double Now() {
        using Clock = std::chrono::steady_clock;
        static const Clock::time_point s_start = Clock::now();
        return std::chrono::duration<double>(Clock::now() - s_start).count();
    }

    // What was asked, by tag, until answered (a few frames on; one per frame slot and some).
    struct Asked {
        uint64_t tag = 0;
        uint32_t count = 0;
        uint64_t guids[kPoints] = {};
        float weights[kPoints] = {};
    };
    Asked s_asked[8];
    uint64_t s_tag = 0;

    // What the Water panel shows.
    struct Status {
        uint32_t found = 0;      // swimmers asked about at the last capture
        uint32_t answered = 0;   // answers in the last batch
        float playerRise = 0.0f, playerWeight = 0.0f, playerDepth = 0.0f;
        bool playerSwimming = false;
    };
    Status s_status;

    struct Rider {
        float rise = 0.0f, slopeX = 0.0f, slopeY = 0.0f;    // the last answer, weighted
        float shownRise = 0.0f, shownX = 0.0f, shownY = 0.0f;   // what's drawn, following it
        double answered = 0.0;
        double drawn = -1.0;
    };
    std::unordered_map<uint64_t, Rider> s_riders;

    void Answer(uint64_t tag, const float (*results)[4], uint32_t count) {
        for (const Asked& asked : s_asked) {
            if (asked.tag != tag || !asked.count) {
                continue;
            }

            const double now = Now();
            s_status.answered = 0;

            for (uint32_t i = 0; i < count && i < asked.count; i++) {
                if (results[i][3] < 0.5f) {
                    continue;
                }

                s_status.answered++;
                Rider& r = s_riders[asked.guids[i]];
                r.rise = results[i][0] * asked.weights[i];
                r.slopeX = results[i][1] * asked.weights[i];
                r.slopeY = results[i][2] * asked.weights[i];
                r.answered = now;
            }

            return;
        }
    }

    struct Swimmer {
        uint64_t guid;
        float x, y, surface, weight, distance;
    };

    struct Collect {
        C3Vector camera;
        std::vector<Swimmer>* found;
    };

    int32_t CollectSwimmer(WOWGUID guid, void* param) {
        Collect& c = *static_cast<Collect*>(param);
        CGObject_C* object = ClntObjMgrObjectPtr(guid, TYPE_OBJECT, __FILE__, __LINE__);

        if (!object || !(object->m_obj->m_type & TYPE_UNIT)) {
            return 1;
        }

        CGUnit_C* unit = static_cast<CGUnit_C*>(object);

        if (!unit->m_movement || !(unit->m_movement->m_flags & MOVEMENTFLAG_SWIMMING)) {
            return 1;
        }

        C3Vector p = unit->GetPosition();
        const float dx = p.x - c.camera.x, dy = p.y - c.camera.y, dz = p.z - c.camera.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (distance > kRange) {
            return 1;
        }

        // WMO water doesn't move (no shore field, no waves on its mesh): its swimmers stay as they are.
        int32_t type = 0;
        float surface = 0.0f;

        if (CMap::QueryLiquidStatusMapObjsExt(&p, &type, &surface)) {
            return 1;
        }

        if (!CMap::QueryLiquidTerrain(&p, &type, &surface, 0)) {
            return 1;
        }

        const float depth = surface - p.z;

        if (depth < -0.5f || depth > kDiveDepth) {
            return 1;
        }

        const float t = std::clamp((depth - kRideDepth) / (kDiveDepth - kRideDepth), 0.0f, 1.0f);
        c.found->push_back(Swimmer{ guid.m_guid, p.x, p.y, surface, 1.0f - t * t * (3.0f - 2.0f * t), distance });
        return 1;
    }

    // The basis rows of a model's transform turned so its up follows the wave's normal (row vectors: the
    // world point is local x * row a + y * row b + z * row c, so turning each row turns the model about its
    // origin).
    void TiltBasis(C44Matrix& m, float slopeX, float slopeY) {
        float nx = -slopeX, ny = -slopeY, nz = 1.0f;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        nx /= length;
        ny /= length;
        nz /= length;
        // About the axis up x n by the angle between them.
        float kx = -ny, ky = nx;
        const float sine = std::sqrt(kx * kx + ky * ky);

        if (sine < 1e-5f) {
            return;
        }

        kx /= sine;
        ky /= sine;
        const float cosine = nz;
        const auto turn = [&](float& x, float& y, float& z) {
            const float cx = ky * z, cy = -kx * z, cz = kx * y - ky * x;   // k x v (k.z = 0)
            const float kv = kx * x + ky * y;
            const float rx = x * cosine + cx * sine + kx * kv * (1.0f - cosine);
            const float ry = y * cosine + cy * sine + ky * kv * (1.0f - cosine);
            const float rz = z * cosine + cz * sine;
            x = rx;
            y = ry;
            z = rz;
        };
        turn(m.a0, m.a1, m.a2);
        turn(m.b0, m.b1, m.b2);
        turn(m.c0, m.c1, m.c2);
    }

    // The rider's drawn rise and slope brought toward its answer (or back to still when answers stopped).
    Rider* Follow(uint64_t guid) {
        const auto found = s_riders.find(guid);

        if (found == s_riders.end()) {
            return nullptr;
        }

        Rider& r = found->second;
        const double now = Now();

        if (r.drawn == now) {
            return &r;
        }

        const float dt = r.drawn < 0.0 ? 1.0f : static_cast<float>(std::min(now - r.drawn, 0.5));
        r.drawn = now;
        const bool fresh = now - r.answered < kStale;
        const float blend = 1.0f - std::exp(-dt / kSmooth);
        r.shownRise += ((fresh ? r.rise : 0.0f) - r.shownRise) * blend;
        r.shownX += ((fresh ? r.slopeX : 0.0f) - r.shownX) * blend;
        r.shownY += ((fresh ? r.slopeY : 0.0f) - r.shownY) * blend;

        if (!fresh && std::fabs(r.shownRise) < 1e-3f && now - r.answered > 2.0) {
            s_riders.erase(found);
            return nullptr;
        }

        return &r;
    }

    C3Vector s_camera;

    // The device's call at the frame's first capture (once a frame: the water's set-up runs for every water
    // draw, and asking each time overwrote what was asked before the answers came back).
    uint32_t Ask(float (*points)[4], uint32_t max, uint64_t* tag) {
        // The player's own state, for the Water panel.
        s_status.playerSwimming = false;
        s_status.playerDepth = 0.0f;
        s_status.playerWeight = 0.0f;

        if (CGPlayer_C* player = ClntObjMgrGetActivePlayerObj()) {
            s_status.playerSwimming = player->m_movement && (player->m_movement->m_flags & MOVEMENTFLAG_SWIMMING);
            C3Vector p = player->GetPosition();
            int32_t type = 0;
            float surface = 0.0f;

            if (CMap::QueryLiquidTerrain(&p, &type, &surface, 0)) {
                s_status.playerDepth = surface - p.z;
            }
        }

        static std::vector<Swimmer> s_found;
        s_found.clear();
        Collect collect{ s_camera, &s_found };
        ClntObjMgrEnumVisibleObjects(&CollectSwimmer, &collect);
        s_status.found = static_cast<uint32_t>(s_found.size());

        if (s_found.empty()) {
            return 0;
        }

        std::sort(s_found.begin(), s_found.end(), [](const Swimmer& a, const Swimmer& b) { return a.distance < b.distance; });
        Asked& asked = s_asked[++s_tag % (sizeof s_asked / sizeof s_asked[0])];
        asked.tag = s_tag;
        asked.count = static_cast<uint32_t>(std::min<size_t>({ s_found.size(), kPoints, max }));

        for (uint32_t i = 0; i < asked.count; i++) {
            const Swimmer& sw = s_found[i];
            points[i][0] = sw.x;
            points[i][1] = sw.y;
            points[i][2] = sw.surface;
            points[i][3] = 0.0f;
            asked.guids[i] = sw.guid;
            asked.weights[i] = sw.weight;
        }

        *tag = asked.tag;
        return asked.count;
    }
}

namespace SXC::Water {
    void RideAsk(Waves& waves) {
        if (!RideOn().Get()) {
            return;
        }

        s_camera = C3Vector(waves.queryCamera[0], waves.queryCamera[1], waves.queryCamera[2]);
        waves.queryAsk = &Ask;
        waves.querySink = &Answer;
    }

    void RideModel(CGUnit_C* unit, C44Matrix& transform) {
        if (s_riders.empty() || !unit) {
            return;
        }

        const Rider* r = Follow(unit->m_obj->m_guid.m_guid);

        if (!r) {
            return;
        }

        transform.d2 += r->shownRise;
        const float tilt = Tilt().Get();

        if (tilt > 0.0f) {
            TiltBasis(transform, r->shownX * tilt, r->shownY * tilt);
        }
    }

    void RideStatus(char* text, size_t size) {
        float rise = 0.0f;
        bool riding = false;

        if (CGPlayer_C* player = ClntObjMgrGetActivePlayerObj()) {
            const auto found = s_riders.find(player->m_obj->m_guid.m_guid);
            riding = found != s_riders.end();
            rise = riding ? found->second.shownRise : 0.0f;
        }

        std::snprintf(text, size, "%s: %u asked, %u answered. You: %s, %.2f yd under the surface, %s (%+.2f yd).",
                      RideOn().Get() ? "on" : "off", s_status.found, s_status.answered, s_status.playerSwimming ? "swimming" : "not swimming",
                      s_status.playerDepth, riding ? "riding" : "not riding", rise);
    }

    float RideLift(CGObject_C* target) {
        if (s_riders.empty() || !target) {
            return 0.0f;
        }

        const Rider* r = Follow(target->m_obj->m_guid.m_guid);
        return r ? r->shownRise * CameraFollows().Get() : 0.0f;
    }
}
