// SXC's trails in the covers (SXC/Cover/Trails.h).

#include "SXC/Cover/Trails.h"

#include <cmath>
#include <initializer_list>
#include <iterator>
#include <unordered_map>
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "SXC/Api/sxc_cover.h"
#include "SXC/Bones/Bodies.h"
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

    Layer s_fine, s_coarse;
    bool s_haveWindow = false;
    uint32_t s_startMs = 0;

    // What a body's point (SXC/Bones: its feet, then its body parts) carved last: a foot's contact point's
    // height above the unit at its last footstep, where the point was last frame.
    struct Carve {
        float contactLift = 0.0f;
        C3Vector last;
        bool haveLast = false;
    };

    struct Walker {
        const CM2Model* model = nullptr;
        Carve points[SXC::Bones::kRoles];
    };

    std::unordered_map<uint64_t, Walker> s_walkers;

    Walker& WalkerOf(uint64_t guid, const CM2Model* model) {
        Walker& w = s_walkers[guid];

        if (w.model != model) {
            w = Walker{};
            w.model = model;
        }

        return w;
    }

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
        // The body learns its feet whether or not the trails are on (SXC/Bones).
        const int foot = SXC::Bones::Footstep(model, eventId, position, guid);

        if (foot < 0 || !s_settings.on || !s_haveWindow) {
            return;
        }

        s_stats.footsteps++;
        CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(WOWGUID{ guid }, TYPE_UNIT, __FILE__, __LINE__));
        const SXC::Bones::Body* body = unit ? SXC::Bones::Of(unit, guid) : nullptr;

        if (!body) {
            return;
        }

        const C3Vector at = unit->GetPosition();
        const SXC::Bones::Point& f = body->points[foot];
        const C3Vector down = f.known && SXC::Bones::Posed(model) && f.bone < model->m_shared->m_data->m_bones.m_count ? SXC::Bones::Where(model, f) : position;
        WalkerOf(guid, model).points[foot].contactLift = down.z - at.z;

        if (NearUnit("footstep", guid, model, at, down, body->radius)) {
            StampBoth(down.x, down.y, down.z, body->radius, Clock(), true);
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

        // The known feet: carve along where they went since last frame, while they're on the ground. Only
        // bodies that have stepped (a swimmer's body is known for the water's ripples).
        const float now = Clock();
        const float reach2 = settings.reach * settings.reach;
        uint32_t feet = 0, parts = 0;
        SXC::Bones::Prune();

        for (auto it = s_walkers.begin(); it != s_walkers.end();) {
            auto body = SXC::Bones::All().find(it->first);
            it = body == SXC::Bones::All().end() || body->second.model != it->second.model ? s_walkers.erase(it) : std::next(it);
        }

        for (auto& [guid, body] : SXC::Bones::All()) {
            CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(WOWGUID{ guid }, TYPE_UNIT, __FILE__, __LINE__));

            if (!unit || !body.stepped) {
                continue;
            }

            Walker& w = WalkerOf(guid, body.model);
            const C3Vector at = unit->GetPosition();
            const float dx = at.x - p.x, dy = at.y - p.y;
            const CM2Model* model = body.model;
            const bool usable = dx * dx + dy * dy <= reach2 && SXC::Bones::Grounded(unit) && SXC::Bones::Posed(model);

            // Bones are view space as of the model's last animation; a model not animated this scene frame
            // (out of view) still holds an older view's, which the current m_viewInv puts somewhere else in
            // the world: no carving from it (the last point is kept, the furrow resumes when it's animated).
            if (usable && !SXC::Bones::Current(model)) {
                continue;
            }

            for (int k = 0; k < SXC::Bones::kRoles; k++) {
                const SXC::Bones::Point& f = body.points[k];
                Carve& c = w.points[k];

                if (!f.known || !usable || f.bone >= model->m_shared->m_data->m_bones.m_count) {
                    c.haveLast = false;
                    continue;
                }

                const bool isFoot = k < SXC::Bones::kFeet;
                const float radius = isFoot ? body.radius : body.radius * f.size;
                (isFoot ? feet : parts)++;
                // A body part is a ball: it carves down to its bottom.
                C3Vector world = SXC::Bones::Where(model, f);
                world.z -= isFoot ? 0.0f : radius;

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
                    !NearUnit(isFoot ? "followed" : "body part", guid, model, at, world, radius)) {
                    c.haveLast = false;
                    continue;
                }

                const float mx = world.x - c.last.x, my = world.y - c.last.y;

                if (c.haveLast && mx * mx + my * my < 9.0f) {
                    StampPath(c.last, world, radius, now, rim);
                } else {
                    StampBoth(world.x, world.y, world.z, radius, now, rim);
                }

                c.last = world;
                c.haveLast = true;
            }
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
