// SXC's bone roles (SXC/Bones/Bodies.h).

#include "SXC/Bones/Bodies.h"

#include <cmath>
#include <initializer_list>
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"

namespace {
    using namespace SXC::Bones;

    constexpr uint32_t kMostMisses = 6;     // footsteps with no joint near before a foot follows the event's own point

    // Body parts by the models' attachment points (the same ids on every model); a second id where the
    // first is missing. In the roles' order from kHandRight.
    struct BodyPart {
        uint32_t attachment, otherwise;
        float size;
    };
    constexpr uint32_t kNoAttachment = 0xFFFFFFFFu;
    constexpr BodyPart kBodyParts[kRoles - kFeet] = {
        { 1, kNoAttachment, 0.6f },  // right hand
        { 2, kNoAttachment, 0.6f },  // left hand
        { 9, kNoAttachment, 1.2f },  // right hip
        { 10, kNoAttachment, 1.2f }, // left hip
        { 15, 34, 1.5f },            // the chest, front (else its middle)
        { 16, kNoAttachment, 1.5f }, // the chest, back
        { 20, 11, 1.0f },            // the head (else the helm's point)
    };

    std::unordered_map<uint64_t, Body> s_bodies;

    float FootRadius(const CGUnit_C* unit) {
        int32_t texture = 0;
        float size[2] = {};
        unit->GetFootprintInfo(&texture, size);
        const float longer = size[0] > size[1] ? size[0] : size[1];
        const float r = longer > 0.0f ? longer * 0.6f : 0.2f;
        return r < 0.1f ? 0.1f : (r > 2.0f ? 2.0f : r);
    }

    // A footstep event: "$" + B/F/R/S/W + L/R + 0..3 (CGUnit_C::HandleAnimEvent's footstep cases).
    int FootOf(uint32_t eventId) {
        const uint32_t c0 = eventId & 0xFF, c1 = (eventId >> 8) & 0xFF, c2 = (eventId >> 16) & 0xFF, c3 = (eventId >> 24) & 0xFF;

        if (c0 != '$' || (c1 != 'B' && c1 != 'F' && c1 != 'R' && c1 != 'S' && c1 != 'W') || c3 < '0' || c3 > '3') {
            return -1;
        }

        return c2 == 'L' ? 0 : (c2 == 'R' ? 1 : -1);
    }

    // The foot that landed at `contact` (a footstep event's point): the models' footstep events sit on
    // bones of their own (player models: one per animation, under the root, ahead of or behind the unit;
    // they mark where a step lands and don't move with the leg), so the foot is the skeleton's joint nearest
    // to that point, close to the ground, on its own side of the skeleton (a model faces +x, its left is +y:
    // the markers of both feet lie near the middle, nearer the other foot at times) and not the other
    // foot's. False: none near (the event's own point is followed then).
    bool IsMarker(const M2Data* d, uint32_t bone) {
        for (uint32_t e = 0; e < d->m_events.m_count; e++) {
            if (d->m_events[e].m_bone == bone) {
                return true;
            }
        }

        return false;
    }

    bool OnSide(const M2Data* d, uint32_t bone, int side) {
        const float rest = d->m_bones[bone].m_pivot.y;
        return side == 0 ? rest > 0.01f : rest < -0.01f;
    }

    // Whether `ancestor` is `bone` or one of its parents.
    bool Under(const M2Data* d, uint32_t bone, uint32_t ancestor) {
        for (int n = 0; bone < d->m_bones.m_count && n < 256; n++, bone = d->m_bones[bone].m_parentIndex) {
            if (bone == ancestor) {
                return true;
            }
        }

        return false;
    }

    //
    // Only the rest pose's lowest joints on that side can be a foot (within 0.25 of the lowest): a hand,
    // a robe's or cloak's bones, or a leg still in the riding pose just after a dismount may come nearer
    // the marker for a step.
    bool FindFoot(const CM2Model* model, const C3Vector& contact, float radius, int side, const Point& other, Point& f) {
        const M2Data* d = model->m_shared->m_data;
        const float most = 0.6f + radius;
        float best = most * most;
        float lowest = 1.0e6f;
        bool found = false;

        for (uint32_t i = 0; i < d->m_bones.m_count; i++) {
            if (!IsMarker(d, i) && OnSide(d, i, side) && d->m_bones[i].m_pivot.z < lowest) {
                lowest = d->m_bones[i].m_pivot.z;
            }
        }

        for (uint32_t i = 0; i < d->m_bones.m_count; i++) {
            if (d->m_bones[i].m_pivot.z > lowest + 0.25f || IsMarker(d, i) || !OnSide(d, i, side) || (other.known && other.bone == i)) {
                continue;
            }

            const C3Vector p = d->m_bones[i].m_pivot * model->m_boneMatrices[i] * model->m_scene->m_viewInv;
            const float dx = p.x - contact.x, dy = p.y - contact.y, dz = p.z - contact.z;

            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || dz < -0.15f || dz > 0.4f || dx * dx + dy * dy >= best) {
                continue;
            }

            best = dx * dx + dy * dy;
            f.bone = static_cast<uint16_t>(i);
            f.offset = d->m_bones[i].m_pivot;
            f.sole = dz > 0.0f ? dz : 0.0f;
            found = true;
        }

        return found;
    }

    // Four legs: a found foot's partner on the same side, from the rest pose: about as low, at least 0.5 yd
    // along the body from it, on another limb (neither above nor below it in the skeleton); the lowest such
    // joint. A biped has none (nothing else on a side is that low). Takes the found foot's sole.
    bool FindSecondFoot(const CM2Model* model, int side, const Point& first, Point& out) {
        const M2Data* d = model->m_shared->m_data;
        const C3Vector& a = d->m_bones[first.bone].m_pivot;
        float lowest = a.z + 0.15f;
        bool found = false;

        for (uint32_t i = 0; i < d->m_bones.m_count; i++) {
            const C3Vector& r = d->m_bones[i].m_pivot;

            if (r.z >= lowest || std::fabs(r.x - a.x) < 0.5f || IsMarker(d, i) || !OnSide(d, i, side) || Under(d, i, first.bone) ||
                Under(d, first.bone, i)) {
                continue;
            }

            lowest = r.z;
            out = Point{};
            out.known = true;
            out.event = first.event;
            out.bone = static_cast<uint16_t>(i);
            out.offset = r;
            out.sole = first.sole;
            found = true;
        }

        return found;
    }

    // The body parts a model has (kBodyParts).
    void FindParts(const CM2Model* model, Body& b) {
        const M2Data* d = model->m_shared->m_data;

        for (int k = 0; k < kRoles - kFeet; k++) {
            const BodyPart& part = kBodyParts[k];
            Point& p = b.points[kFeet + k];
            p = Point{};

            for (uint32_t id : { part.attachment, part.otherwise }) {
                const uint16_t index = id < d->m_attachmentIndicesById.m_count ? d->m_attachmentIndicesById[id] : 0xFFFF;

                if (index >= d->m_attachments.m_count || d->m_attachments[index].m_bone >= d->m_bones.m_count) {
                    continue;
                }

                p.known = true;
                p.bone = d->m_attachments[index].m_bone;
                p.offset = d->m_attachments[index].m_position;
                p.size = part.size;
                break;
            }
        }

        b.partsFound = true;
    }
}

namespace SXC::Bones {
    const CM2Model* Carrier(const CGUnit_C* unit) {
        return unit->IsMounted() && unit->m_mountModel ? unit->m_mountModel : unit->m_worldModel;
    }

    bool Grounded(const CGUnit_C* unit) {
        return !(unit->m_movement->m_flags & 0x40000000) && !unit->GetTransportGUID();
    }

    bool Posed(const CM2Model* model) {
        return model && model->m_boneMatrices && model->m_scene && model->m_shared && model->m_shared->m_data;
    }

    bool Current(const CM2Model* model) {
        return Posed(model) && model->m_frameStamp == model->m_scene->m_frameStamp;
    }

    C3Vector Where(const CM2Model* model, const Point& point) {
        C3Vector p = point.offset * model->m_boneMatrices[point.bone] * model->m_scene->m_viewInv;
        p.z -= point.sole;
        return p;
    }

    Body* Of(CGUnit_C* unit, uint64_t guid) {
        const CM2Model* model = unit ? Carrier(unit) : nullptr;

        if (!model) {
            return nullptr;
        }

        Body& b = s_bodies[guid];

        if (b.model != model) {
            b = Body{};
            b.model = model;
            b.radius = FootRadius(unit);
        }

        if (!b.partsFound && Posed(model)) {
            FindParts(model, b);
        }

        return &b;
    }

    int Footstep(CM2Model* model, uint32_t eventId, const C3Vector& position, uint64_t guid) {
        const int foot = FootOf(eventId);

        if (foot < 0 || !model) {
            return -1;
        }

        CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(WOWGUID{ guid }, TYPE_UNIT, __FILE__, __LINE__));

        // Only the carrier's footsteps count (a mount fading out after a dismount still sends its own).
        if (!unit || !Grounded(unit) || model != Carrier(unit)) {
            return -1;
        }

        Body& b = *Of(unit, guid);
        b.stepped = true;
        b.radius = FootRadius(unit);
        Point& f = b.points[foot];
        C3Vector* offset = nullptr;
        uint16_t bone = 0;

        // The foot is found once per unit and model, when two of its steps in a row find the same joint
        // (a step in a transition, a rider just off the saddle, may find another), and its partner on that
        // side where the model has four legs; every step after only tells when the foot is down. Steps with
        // no joint near try again at the next one; only a model that never has one follows the event's own
        // point.
        if (!f.known && Posed(model) && model->FindEvent(eventId, &offset, &bone) && offset) {
            Point found = f;

            if (FindFoot(model, position, b.radius, foot, b.points[1 - foot], found)) {
                if (found.bone == f.candidate) {
                    f = found;
                    FindSecondFoot(model, foot, f, b.points[foot + 2]);
                    f.known = true;
                } else {
                    f.candidate = found.bone;
                }
            } else if (++f.misses >= kMostMisses) {
                f.bone = bone;
                f.offset = *offset;
                f.sole = 0.0f;
                f.known = true;
            }
        }

        f.event = eventId;
        return foot;
    }

    std::unordered_map<uint64_t, Body>& All() {
        return s_bodies;
    }

    void Prune() {
        for (auto it = s_bodies.begin(); it != s_bodies.end();) {
            const CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(WOWGUID{ it->first }, TYPE_UNIT, __FILE__, __LINE__));

            if (!unit || it->second.model != Carrier(unit)) {
                it = s_bodies.erase(it);
            } else {
                ++it;
            }
        }
    }

    void Reset() {
        s_bodies.clear();
    }
}
