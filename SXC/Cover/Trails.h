#pragma once

#include <cstdint>
#include <vector>
#include "Engine/Source/Tempest/C3Vector.h"

class CM2Model;

// SXC's trails in the covers (step 5d): where units' feet press the cover down. Two world-aligned
// toroidal layers around the player (cell (i, j) at [mod(j, size) * size + mod(i, size)], 4 floats):
// a fine one (0.125 yd) that fills back in by the covers' relax time, a coarse one (0.5 yd) that keeps
// trails longer (the look's persist time). Per cell: the ceiling the cover was pressed down to (world z;
// 1e6 untouched) and when, the rim's strength (0..1) and when. The terrain's tessellation reads them
// and lets the ceiling rise back over time (Shaders/TerrainCover.glsl), so nothing here relaxes cells.
//
// The feet and body parts are the units' bodies (SXC/Bones/Bodies.h: feet from their footstep events, body
// parts from attachment points), followed every frame; a footstep then only tells when the foot is down.
// Where the foot carves: sxc_cover.h's SXC_COVER_CARVE_*.
namespace SXC::Cover::Trails {
    constexpr float kUntouched = 1.0e6f;

    struct Rect {
        int32_t x, y, w, h; // texels
    };

    struct Layer {
        uint32_t size = 0;
        float cell = 1.0f;
        int32_t firstI = 0, firstJ = 0;
        std::vector<float> cells;
        std::vector<Rect> dirty; // changed since the device last took them
        bool full = true;        // everything changed (a new window): upload all of it
        uint32_t version = 1;
    };

    struct Settings {
        bool on = false;
        float reach = 45.0f;     // yd: units this close to the player carve
        int carve = 0;           // SXC_COVER_CARVE_* (sxc_cover.h)
        float lift = 0.3f;       // yd: SXC_COVER_CARVE_UP_TO's height
        float deepest = 2.0f;    // yd: the deepest any cover is now; a point higher above its unit isn't written
    };

    // The footstep seam: CGUnit_C::AnimEventCallback, for every event (non-footsteps are ignored).
    void Event(CM2Model* model, uint32_t eventId, const C3Vector& position, uint64_t guid);

    // Once per frame (SXC::Cover::Current): follow the known feet, carve, move the windows with the player.
    void Update(const Settings& settings);

    const Layer& Fine();
    const Layer& Coarse();
    // The device took the changes: forget them.
    void TookChanges();

    // Seconds the trails' clock has run (the shader's "now").
    float Now();

    // One of the player's feet as followed this frame (what a model's footstep events tie it to).
    struct FootView {
        bool known = false;
        uint32_t event = 0;      // the last footstep event's id (four characters, "$FL0")
        uint16_t bone = 0;       // the bone it follows (the foot's joint, found at its first step)
        float sole = 0.0f;       // yd the followed point lies below that joint
        int32_t keyBone = -1;    // that bone's key-bone id (-1 none)
        uint16_t parent = 0xFFFF;
        float forward = 0.0f, left = 0.0f, up = 0.0f; // yd from the unit's position, in its facing
    };

    struct Stats {
        uint32_t feet = 0;       // feet followed now
        uint32_t parts = 0;      // body parts followed now (hands, hips, chest)
        uint32_t stamps = 0;     // carvings last frame
        uint64_t footsteps = 0;  // footstep events seen
        FootView player[2];      // left, right
    };
    Stats GetStats();

    // Forget everything (the world left, trails off).
    void Reset();
}
