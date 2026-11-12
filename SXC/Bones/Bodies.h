#pragma once

#include <cstdint>
#include <unordered_map>
#include "Engine/Source/Tempest/C3Vector.h"

class CM2Model;
class CGUnit_C;

// SXC's bone roles (kb: bone-roles): where a unit's feet, hands, hips, chest and head are, per unit and the
// model it stands on, for everything that touches the world with a body (the covers' trails, the water's
// ripples; later foot IK and the unified skeleton).
//
// Feet are found from the units' own footstep events (the client's footprints use them). An event's own
// bone is a marker (player models: one per animation under the root, where a step lands; it doesn't move
// with the leg), so at a unit's first steps with a foot the skeleton's joint nearest the landing point is
// taken as that foot (once two steps in a row agree) and followed from then on, its height above the sole
// at that step taken off; a four-legged model's partner foot on each side from the rest pose. Body parts
// come from the models' attachment points, the same ids on every model, so a unit that never steps (a
// swimmer) has them too.
namespace SXC::Bones {
    // The feet the footstep events name (left, right), a second pair where the model has four legs (left,
    // right), then the body parts.
    enum Role : int {
        kFootLeft,
        kFootRight,
        kHindLeft,
        kHindRight,
        kHandRight,
        kHandLeft,
        kHipRight,
        kHipLeft,
        kChestFront,
        kChestBack,
        kHead,
        kRoles
    };
    constexpr int kFeet = 4;

    struct Point {
        bool known = false;
        uint32_t event = 0;       // a foot: the last footstep event's id (four characters, "$FL0")
        uint16_t bone = 0;
        // The point followed on the bone, model space: a foot's joint (the bone's pivot), found at its first
        // footsteps (else the footstep event's own point); a body part's attachment point.
        C3Vector offset;
        float sole = 0.0f;        // a foot: yd its sole lies below the point (its joint's height above it at that step)
        float size = 1.0f;        // a body part: its radius, x the body's foot radius
        uint32_t misses = 0;      // footsteps with no joint near (the event's own point is taken after a few)
        int32_t candidate = -1;   // the joint the last step found: a foot is settled when two steps in a row agree
    };

    struct Body {
        const CM2Model* model = nullptr;   // the model the unit stands on (Carrier)
        Point points[kRoles];
        bool partsFound = false;
        bool stepped = false;     // a footstep seen on this model
        float radius = 0.2f;      // yd: the unit's foot (from its footprint)
    };

    // The model a unit stands on: its mount while mounted, else its own.
    const CM2Model* Carrier(const CGUnit_C* unit);

    // Not flying, not on a transport (their positions are the transport's).
    bool Grounded(const CGUnit_C* unit);

    // The unit's body on the model it stands on now: made, or made anew after a morph, mounting or
    // dismounting; its body parts found once the model is posed. Null: no model.
    Body* Of(CGUnit_C* unit, uint64_t guid);

    // The footstep seam (CGUnit_C::AnimEventCallback, every event): a foot of the unit's carrier learnt.
    // The foot's role, -1 when it isn't one (not a footstep, another model's, a flying unit's).
    int Footstep(CM2Model* model, uint32_t eventId, const C3Vector& position, uint64_t guid);

    // The model has bones to read, and they are this scene frame's (it was animated now: one out of view
    // holds an older view's bones, which the current view puts somewhere else in the world).
    bool Posed(const CM2Model* model);
    bool Current(const CM2Model* model);

    // A point in the world now (a foot: its sole, below the followed joint). The model must be posed.
    C3Vector Where(const CM2Model* model, const Point& point);

    // The bodies known now, by unit. Prune() first, once a frame, by whoever reads them.
    std::unordered_map<uint64_t, Body>& All();

    // Bodies whose unit is gone or stands on another model now: forgotten.
    void Prune();

    void Reset();
}
