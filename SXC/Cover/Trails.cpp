// SXC's trails in the covers (SXC/Cover/Trails.h).

#include "SXC/Cover/Trails.h"

#include <cmath>
#include <initializer_list>
#include <unordered_map>
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "SXC/Api/sxc_cover.h"
#include "SXC/Log.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"

namespace {
    using namespace SXC::Cover::Trails;

    constexpr uint32_t kFineSize = 768;     // x 0.125 yd = 96 yd across
    constexpr float kFineCell = 0.125f;
    constexpr int32_t kFineStep = 64;       // cells the window moves at a time
    constexpr uint32_t kCoarseSize = 512;   // x 0.5 yd = 256 yd across
    constexpr float kCoarseCell = 0.5f;
    constexpr int32_t kCoarseStep = 32;
    constexpr uint32_t kMaxRects = 96;      // more changed rectangles in a frame: upload the whole layer

    // Whether a new carving goes deeper than what a cell holds: its ceiling as it has risen since, at a
    // rate assumed here (the shader's own rate follows the cover's depth and relax time).
    constexpr float kRiseEstimate = 0.02f;  // yd per second
    constexpr float kRimLife = 30.0f;       // s: the same for the rim's strength
    constexpr float kRimWidth = 0.7f;       // the rim reaches this share of the radius beyond the foot
    constexpr float kBowl = 0.35f;          // the print's floor rises toward its edge by this share of the radius
    constexpr float kThroughMost = 1.5f;    // yd above the unit: higher, a foot is in the air (a jump), whatever the mode
    constexpr float kContactBand = 0.1f;    // yd above the foot's height at its last footstep: still in contact
    constexpr uint32_t kMostMisses = 6;     // footsteps with no joint near before a foot follows the event's own point

    Layer s_fine, s_coarse;
    bool s_haveWindow = false;
    uint32_t s_startMs = 0;

    // A point that carves: a foot, or a body part (a hand, a hip, the chest).
    struct Foot {
        bool known = false;
        uint32_t event = 0;
        uint16_t bone = 0;
        // The point followed on the bone, model space: a foot's joint (the bone's pivot), found at the
        // first footstep (else the footstep event's own point); a body part's attachment point.
        C3Vector offset;
        float sole = 0.0f; // yd the carving reaches below the point (a foot: its joint's height above the sole at that step)
        float size = 1.0f; // a body part's radius, x the unit's foot radius (its sole too: the part is a ball)
        float contactLift = 0.0f; // yd a foot's contact point was above the unit at its last footstep
        uint32_t misses = 0;      // footsteps with no joint near (the event's own point is taken after a few)
        int32_t candidate = -1;   // the joint the last step found: a foot is settled when two steps in a row agree
        C3Vector last;
        bool haveLast = false;
    };

    // A walker's points: the feet the footstep events name (left, right), a second pair where the model
    // has four legs (left, right), then its body parts.
    constexpr int kFeet = 4;
    constexpr int kPoints = 12;

    // Body parts that carve (always, wherever they dip into the cover), by the models' attachment points
    // (the same ids on every model); a second id where the first is missing.
    struct BodyPart {
        uint32_t attachment, otherwise;
        float size;
    };
    constexpr uint32_t kNoAttachment = 0xFFFFFFFFu;
    constexpr BodyPart kBodyParts[] = {
        { 1, kNoAttachment, 0.6f },  // right hand
        { 2, kNoAttachment, 0.6f },  // left hand
        { 9, kNoAttachment, 1.2f },  // right hip
        { 10, kNoAttachment, 1.2f }, // left hip
        { 15, 34, 1.5f },            // the chest, front (else its middle)
        { 16, kNoAttachment, 1.5f }, // the chest, back
        { 20, 11, 1.0f },            // the head (else the helm's point)
    };

    struct Walker {
        CM2Model* model = nullptr;
        Foot points[kPoints];
        int count = kFeet;     // points in use (the feet's slots always)
        bool partsFound = false;
        float radius = 0.2f;
    };

    std::unordered_map<uint64_t, Walker> s_walkers;
    Stats s_stats;

    int32_t Slot(int32_t v, uint32_t size) {
        const int32_t m = v % static_cast<int32_t>(size);
        return m < 0 ? m + static_cast<int32_t>(size) : m;
    }

    float* Cell(Layer& L, int32_t i, int32_t j) {
        return &L.cells[(static_cast<size_t>(Slot(j, L.size)) * L.size + Slot(i, L.size)) * 4];
    }

    void Clear(float* c) {
        c[0] = kUntouched;
        c[1] = 0.0f;
        c[2] = 0.0f;
        c[3] = 0.0f;
    }

    // World cells [i0, i1] x [j0, j1] (inside the window) changed: their texels, split where they wrap.
    void Mark(Layer& L, int32_t i0, int32_t i1, int32_t j0, int32_t j1) {
        if (L.full) {
            return;
        }

        const int32_t n = static_cast<int32_t>(L.size);
        int32_t xs[2][2], ys[2][2];
        int xc = 0, yc = 0;
        const int32_t sx = Slot(i0, L.size), sy = Slot(j0, L.size), w = i1 - i0 + 1, h = j1 - j0 + 1;

        if (sx + w <= n) {
            xs[xc][0] = sx, xs[xc][1] = w, xc++;
        } else {
            xs[xc][0] = sx, xs[xc][1] = n - sx, xc++;
            xs[xc][0] = 0, xs[xc][1] = w - (n - sx), xc++;
        }

        if (sy + h <= n) {
            ys[yc][0] = sy, ys[yc][1] = h, yc++;
        } else {
            ys[yc][0] = sy, ys[yc][1] = n - sy, yc++;
            ys[yc][0] = 0, ys[yc][1] = h - (n - sy), yc++;
        }

        for (int a = 0; a < xc; a++) {
            for (int b = 0; b < yc; b++) {
                if (L.dirty.size() >= kMaxRects) {
                    L.full = true;
                    L.dirty.clear();
                    return;
                }

                L.dirty.push_back(Rect{ xs[a][0], ys[b][0], xs[a][1], ys[b][1] });
            }
        }
    }

    // The window around the player, moved in steps; the cells that come in are cleared.
    void Follow(Layer& L, uint32_t size, float cell, int32_t step, float px, float py) {
        if (L.size != size) {
            L.size = size;
            L.cell = cell;
            L.cells.assign(static_cast<size_t>(size) * size * 4, 0.0f);
            s_haveWindow = false;
        }

        const int32_t half = static_cast<int32_t>(size) / 2;
        const int32_t pi = static_cast<int32_t>(std::floor(px / cell)), pj = static_cast<int32_t>(std::floor(py / cell));
        auto snap = [step](int32_t v) { return static_cast<int32_t>(std::floor(static_cast<float>(v) / step)) * step; };
        const int32_t ni = snap(pi - half), nj = snap(pj - half);
        const int32_t n = static_cast<int32_t>(size);

        if (!s_haveWindow || std::abs(ni - L.firstI) >= n || std::abs(nj - L.firstJ) >= n) {
            for (size_t k = 0; k < L.cells.size(); k += 4) {
                Clear(&L.cells[k]);
            }

            L.firstI = ni;
            L.firstJ = nj;
            L.full = true;
            L.dirty.clear();
            L.version++;
            return;
        }

        if (std::abs(pi - half - L.firstI) < step && std::abs(pj - half - L.firstJ) < step) {
            return;
        }

        // Columns coming in (world i), over the new rows; then rows coming in.
        const int32_t ci0 = ni > L.firstI ? L.firstI + n : ni, ci1 = ni > L.firstI ? ni + n - 1 : L.firstI - 1;

        if (ni != L.firstI) {
            for (int32_t i = ci0; i <= ci1; i++) {
                for (int32_t j = nj; j < nj + n; j++) {
                    Clear(Cell(L, i, j));
                }
            }

            Mark(L, ci0, ci1, nj, nj + n - 1);
        }

        const int32_t cj0 = nj > L.firstJ ? L.firstJ + n : nj, cj1 = nj > L.firstJ ? nj + n - 1 : L.firstJ - 1;

        if (nj != L.firstJ) {
            for (int32_t j = cj0; j <= cj1; j++) {
                for (int32_t i = ni; i < ni + n; i++) {
                    Clear(Cell(L, i, j));
                }
            }

            Mark(L, ni, ni + n - 1, cj0, cj1);
        }

        L.firstI = ni;
        L.firstJ = nj;
        L.version++;
    }

    // A foot's print: pressed down to `bottom` (its floor rising toward the edge), a rim beside it (unless
    // `rim` is false: a foot dragged through the cover between steps).
    void Stamp(Layer& L, float x, float y, float bottom, float radius, float now, bool rim) {
        if (L.size == 0) {
            return;
        }

        const float r = radius > L.cell * 0.75f ? radius : L.cell * 0.75f;
        const float reach = r * (1.0f + kRimWidth);
        int32_t i0 = static_cast<int32_t>(std::floor((x - reach) / L.cell)), i1 = static_cast<int32_t>(std::floor((x + reach) / L.cell));
        int32_t j0 = static_cast<int32_t>(std::floor((y - reach) / L.cell)), j1 = static_cast<int32_t>(std::floor((y + reach) / L.cell));
        const int32_t n = static_cast<int32_t>(L.size);
        i0 = i0 < L.firstI ? L.firstI : i0;
        j0 = j0 < L.firstJ ? L.firstJ : j0;
        i1 = i1 > L.firstI + n - 1 ? L.firstI + n - 1 : i1;
        j1 = j1 > L.firstJ + n - 1 ? L.firstJ + n - 1 : j1;

        if (i0 > i1 || j0 > j1) {
            return;
        }

        bool changed = false;

        for (int32_t j = j0; j <= j1; j++) {
            for (int32_t i = i0; i <= i1; i++) {
                const float dx = (i + 0.5f) * L.cell - x, dy = (j + 0.5f) * L.cell - y;
                const float d = std::sqrt(dx * dx + dy * dy);

                if (d >= reach) {
                    continue;
                }

                float* c = Cell(L, i, j);
                const float age = now - c[1] > 0.0f ? now - c[1] : 0.0f;
                const float ceiling = c[0] < kUntouched ? c[0] + age * kRiseEstimate : kUntouched;

                if (d <= r) {
                    const float t = d / r;
                    const float z = bottom + t * t * r * kBowl;

                    if (z < ceiling) {
                        c[0] = z;
                        c[1] = now;
                        c[2] = 0.0f; // no rim inside a print
                        changed = true;
                    }

                    continue;
                }

                // The rim: not inside a trail that's deeper than this print (walking leaves no rims in
                // its own track).
                if (!rim || ceiling < bottom + r) {
                    continue;
                }

                const float strength = 1.0f - (d - r) / (reach - r);
                const float rimAge = now - c[3] > 0.0f ? now - c[3] : 0.0f;
                const float current = c[2] * (rimAge < kRimLife ? 1.0f - rimAge / kRimLife : 0.0f);

                if (strength > current + 0.02f) {
                    c[2] = strength;
                    c[3] = now;
                    changed = true;
                }
            }
        }

        if (changed) {
            Mark(L, i0, i1, j0, j1);
            s_stats.stamps++;
        }
    }

    void StampBoth(float x, float y, float bottom, float radius, float now, bool rim) {
        Stamp(s_fine, x, y, bottom, radius, now, rim);
        Stamp(s_coarse, x, y, bottom, radius, now, rim);
    }

    // A foot moved from a to b: prints along the way, close enough to make a furrow.
    void StampPath(const C3Vector& a, const C3Vector& b, float radius, float now, bool rim) {
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        const float length = std::sqrt(dx * dx + dy * dy);
        const float spacing = radius * 0.5f > kFineCell ? radius * 0.5f : kFineCell;
        const int steps = length > spacing ? static_cast<int>(length / spacing) : 0;

        for (int k = 1; k <= steps; k++) {
            const float t = static_cast<float>(k) / (steps + 1);
            StampBoth(a.x + dx * t, a.y + dy * t, a.z + dz * t, radius, now, rim);
        }

        StampBoth(b.x, b.y, b.z, radius, now, rim);
    }

    // A footstep event: "$" + B/F/R/S/W + L/R + 0..3 (CGUnit_C::HandleAnimEvent's footstep cases).
    int FootOf(uint32_t eventId) {
        const uint32_t c0 = eventId & 0xFF, c1 = (eventId >> 8) & 0xFF, c2 = (eventId >> 16) & 0xFF, c3 = (eventId >> 24) & 0xFF;

        if (c0 != '$' || (c1 != 'B' && c1 != 'F' && c1 != 'R' && c1 != 'S' && c1 != 'W') || c3 < '0' || c3 > '3') {
            return -1;
        }

        return c2 == 'L' ? 0 : (c2 == 'R' ? 1 : -1);
    }

    // The model a unit stands on: its mount while mounted, else its own. Only its footsteps count (a mount
    // fading out after a dismount still sends its own).
    const CM2Model* Carrier(const CGUnit_C* unit) {
        return unit->IsMounted() && unit->m_mountModel ? unit->m_mountModel : unit->m_worldModel;
    }

    // Units that leave no trails: flying, on a transport (their positions are the transport's).
    bool Grounded(const CGUnit_C* unit) {
        return !(unit->m_movement->m_flags & 0x40000000) && !unit->GetTransportGUID();
    }

    float FootRadius(const CGUnit_C* unit) {
        int32_t texture = 0;
        float size[2] = {};
        unit->GetFootprintInfo(&texture, size);
        const float longer = size[0] > size[1] ? size[0] : size[1];
        const float r = longer > 0.0f ? longer * 0.6f : 0.2f;
        return r < 0.1f ? 0.1f : (r > 2.0f ? 2.0f : r);
    }

    // A followed foot's point in the world now.
    C3Vector FootPoint(const CM2Model* model, const Foot& f) {
        C3Vector p = f.offset * model->m_boneMatrices[f.bone] * model->m_scene->m_viewInv;
        p.z -= f.sole;
        return p;
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
    bool FindFoot(const CM2Model* model, const C3Vector& contact, float radius, int side, const Foot& other, Foot& f) {
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
    // joint. A biped has none (nothing else on a side is that low). Carves with the found foot's sole and
    // contact height.
    bool FindSecondFoot(const CM2Model* model, int side, const Foot& first, Foot& out) {
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
            out = Foot{};
            out.known = true;
            out.event = first.event;
            out.bone = static_cast<uint16_t>(i);
            out.offset = r;
            out.sole = first.sole;
            found = true;
        }

        return found;
    }

    // The body parts a model has (kBodyParts), after the feet's slots.
    void FindParts(const CM2Model* model, Walker& w) {
        const M2Data* d = model->m_shared->m_data;
        w.count = kFeet;

        for (const BodyPart& b : kBodyParts) {
            for (uint32_t id : { b.attachment, b.otherwise }) {
                const uint16_t index = id < d->m_attachmentIndicesById.m_count ? d->m_attachmentIndicesById[id] : 0xFFFF;

                if (index >= d->m_attachments.m_count || d->m_attachments[index].m_bone >= d->m_bones.m_count || w.count >= kPoints) {
                    continue;
                }

                Foot& f = w.points[w.count++];
                f = Foot{};
                f.known = true;
                f.bone = d->m_attachments[index].m_bone;
                f.offset = d->m_attachments[index].m_position;
                f.size = b.size;
                break;
            }
        }

        w.partsFound = true;
    }

    float Clock() {
        return static_cast<float>(OsGetAsyncTimeMs() - s_startMs) * 0.001f;
    }

    // A foot far from its unit is a wrong position (ghost trails seen 2026-10-21, ~30 yd from the
    // player): refused, and the first few logged with where they came from.
    uint32_t s_strays = 0;

    bool NearUnit(const char* source, uint64_t guid, const CM2Model* model, const C3Vector& unitAt, const C3Vector& foot, float radius) {
        const float dx = foot.x - unitAt.x, dy = foot.y - unitAt.y;
        const float limit = 6.0f + 4.0f * radius;

        if (dx * dx + dy * dy <= limit * limit) {
            return true;
        }

        if (++s_strays <= 20) {
            SXC::Log("trails: stray foot (%s) unit %016llx at %.1f %.1f %.1f, foot at %.1f %.1f %.1f (%.1f yd off); model stamp %u, scene stamp %u",
                     source, static_cast<unsigned long long>(guid), unitAt.x, unitAt.y, unitAt.z, foot.x, foot.y, foot.z, std::sqrt(dx * dx + dy * dy),
                     model ? model->m_frameStamp : 0u, model && model->m_scene ? model->m_scene->m_frameStamp : 0u);
        }

        return false;
    }
}

namespace SXC::Cover::Trails {
    static Settings s_settings;

    void Event(CM2Model* model, uint32_t eventId, const C3Vector& position, uint64_t guid) {
        const int foot = FootOf(eventId);

        if (foot < 0 || !s_settings.on || !model || !s_haveWindow) {
            return;
        }

        s_stats.footsteps++;
        CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(WOWGUID{ guid }, TYPE_UNIT, __FILE__, __LINE__));

        if (!unit || !Grounded(unit) || model != Carrier(unit)) {
            return;
        }

        Walker& w = s_walkers[guid];

        if (w.model != model) {
            w = Walker{};
            w.model = model;
        }

        w.radius = FootRadius(unit);
        const C3Vector at = unit->GetPosition();
        C3Vector* offset = nullptr;
        uint16_t bone = 0;
        Foot& f = w.points[foot];
        const bool posed = model->m_boneMatrices && model->m_scene && model->m_shared && model->m_shared->m_data;

        // The foot is found once per unit and model, when two of its steps in a row find the same joint
        // (a step in a transition, a rider just off the saddle, may find another), and its partner on that
        // side where the model has four legs; every step after only tells when the foot is down. Steps with
        // no joint near try again at the next one; only a model that never has one follows the event's own
        // point.
        if (!f.known && posed && model->FindEvent(eventId, &offset, &bone) && offset) {
            Foot found = f;

            if (FindFoot(model, position, w.radius, foot, w.points[1 - foot], found)) {
                if (found.bone == f.candidate) {
                    f = found;
                    FindSecondFoot(model, foot, f, w.points[foot + 2]);
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

            f.haveLast = false;
        }

        f.event = eventId;
        const C3Vector down = f.known && posed && f.bone < model->m_shared->m_data->m_bones.m_count ? FootPoint(model, f) : position;
        f.contactLift = down.z - at.z;

        if (NearUnit("footstep", guid, model, at, down, w.radius)) {
            StampBoth(down.x, down.y, down.z, w.radius, Clock(), true);
        }
    }

    void Update(const Settings& settings) {
        s_stats.stamps = 0;
        s_stats.player[0] = FootView{};
        s_stats.player[1] = FootView{};

        if (!settings.on) {
            if (s_settings.on) {
                Reset();
            }

            s_settings = settings;
            return;
        }

        s_settings = settings;
        CGPlayer_C* player = ClntObjMgrGetActivePlayerObj();

        if (!player) {
            return;
        }

        if (!s_startMs) {
            s_startMs = OsGetAsyncTimeMs();
        }

        const C3Vector p = player->GetPosition();
        Follow(s_fine, kFineSize, kFineCell, kFineStep, p.x, p.y);
        Follow(s_coarse, kCoarseSize, kCoarseCell, kCoarseStep, p.x, p.y);
        s_haveWindow = true;

        // The known feet: carve along where they went since last frame, while they're on the ground.
        const float now = Clock();
        const float reach2 = settings.reach * settings.reach;
        uint32_t feet = 0, parts = 0;

        for (auto it = s_walkers.begin(); it != s_walkers.end();) {
            CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(WOWGUID{ it->first }, TYPE_UNIT, __FILE__, __LINE__));
            Walker& w = it->second;

            // Gone, or standing on another model now (a morph, mounting, dismounting): its feet are learnt
            // again from the new model's footsteps.
            if (!unit || w.model != Carrier(unit)) {
                it = s_walkers.erase(it);
                continue;
            }

            const C3Vector at = unit->GetPosition();
            const float dx = at.x - p.x, dy = at.y - p.y;
            CM2Model* model = w.model;
            const bool usable = dx * dx + dy * dy <= reach2 && Grounded(unit) && model->m_boneMatrices && model->m_scene && model->m_shared && model->m_shared->m_data;

            // Bones are view space as of the model's last animation; a model not animated this scene frame
            // (out of view) still holds an older view's, which the current m_viewInv puts somewhere else in
            // the world: no carving from it (the last point is kept, the furrow resumes when it's animated).
            if (usable && model->m_frameStamp != model->m_scene->m_frameStamp) {
                ++it;
                continue;
            }

            if (usable && !w.partsFound) {
                FindParts(model, w);
            }

            for (int k = 0; k < w.count; k++) {
                Foot& f = w.points[k];

                if (!f.known || !usable || f.bone >= model->m_shared->m_data->m_bones.m_count) {
                    f.haveLast = false;
                    continue;
                }

                const bool isFoot = k < kFeet;
                const float radius = isFoot ? w.radius : w.radius * f.size;
                f.sole = isFoot ? f.sole : radius;
                (isFoot ? feet : parts)++;
                const C3Vector world = FootPoint(model, f);

                // The player's feet, where they are around the player (the Cover panel's status line).
                if (k < 2 && unit == static_cast<CGUnit_C*>(player)) {
                    const M2CompBone& b = model->m_shared->m_data->m_bones[f.bone];
                    const float facing = unit->GetFacing(), fx = world.x - at.x, fy = world.y - at.y;
                    FootView& v = s_stats.player[k];
                    v.known = true;
                    v.event = f.event;
                    v.bone = f.bone;
                    v.sole = f.sole;
                    v.keyBone = static_cast<int32_t>(b.m_boneId);
                    v.parent = b.m_parentIndex;
                    v.forward = fx * std::cos(facing) + fy * std::sin(facing);
                    v.left = -fx * std::sin(facing) + fy * std::cos(facing);
                    v.up = world.z - at.z;
                }

                // Where it carves (the cover is pressed only down to the point, so above the surface it
                // leaves nothing anyway; higher above the unit than the deepest cover there is, it isn't
                // written at all). A foot: through the cover, near contact, or up to a height (the second
                // pair by the first pair's contact height); a body part: wherever it dips in.
                const float lift = world.z - at.z;
                const bool contact = isFoot && lift <= w.points[k & 1].contactLift + kContactBand;
                bool carves = !isFoot || lift <= kThroughMost;
                bool rim = !isFoot || contact;

                if (isFoot && settings.carve == SXC_COVER_CARVE_CONTACT) {
                    carves = contact;
                } else if (isFoot && settings.carve == SXC_COVER_CARVE_UP_TO) {
                    carves = lift <= settings.lift;
                    rim = true;
                }

                if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z) || !carves || lift > settings.deepest ||
                    !NearUnit(isFoot ? "followed" : "body part", it->first, model, at, world, radius)) {
                    f.haveLast = false;
                    continue;
                }

                const float mx = world.x - f.last.x, my = world.y - f.last.y;

                if (f.haveLast && mx * mx + my * my < 9.0f) {
                    StampPath(f.last, world, radius, now, rim);
                } else {
                    StampBoth(world.x, world.y, world.z, radius, now, rim);
                }

                f.last = world;
                f.haveLast = true;
            }

            ++it;
        }

        s_stats.feet = feet;
        s_stats.parts = parts;
    }

    const Layer& Fine() { return s_fine; }
    const Layer& Coarse() { return s_coarse; }

    void TookChanges() {
        for (Layer* L : { &s_fine, &s_coarse }) {
            L->dirty.clear();
            L->full = false;
        }
    }

    float Now() { return s_startMs ? Clock() : 0.0f; }

    Stats GetStats() { return s_stats; }

    void Reset() {
        s_walkers.clear();
        s_fine = Layer{};
        s_coarse = Layer{};
        s_haveWindow = false;
        s_stats = Stats{};
    }
}
