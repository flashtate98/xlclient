// The rivers' flow field (SXC/Water/Flow.h).
#include "SXC/Water/Flow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "SXC/Api/sxc_water.h"

namespace {
    using namespace SXC::Water;

    constexpr float kHalfWorld = 32.0f * 533.33333f;
    constexpr int32_t kN = kShoreSize;

    struct Node {
        float x, y, z, width, speed;
        uint32_t kind;
    };

    struct River {
        uint32_t id, first, count, joins, branchesFrom;
    };

    struct State {
        std::vector<River> rivers;
        std::vector<Node> nodes;
        bool dirty = true;
        uint32_t shoreVersion = 0;
        FlowMap map;
        // The sums while drawing: flow x and y, roughness and the weights, then the largest weight.
        std::vector<float> fx, fy, rough, sum, top;
    };
    State s;

    uint16_t Half(float f) {
        uint32_t x;
        std::memcpy(&x, &f, 4);
        const uint32_t sign = (x >> 16) & 0x8000u;
        const int32_t exponent = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
        const uint32_t mantissa = (x & 0x7FFFFFu) + 0x1000u;

        if (exponent <= 0) {
            return static_cast<uint16_t>(sign);
        }

        if (exponent >= 31) {
            return static_cast<uint16_t>(sign | 0x7BFFu);
        }

        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
    }

    float FromHalf(uint16_t h) {
        const uint32_t sign = (h & 0x8000u) << 16;
        const uint32_t exponent = (h >> 10) & 0x1Fu;
        const uint32_t mantissa = h & 0x3FFu;

        if (exponent == 0) {
            return 0.0f;   // the field's subnormals don't matter
        }

        const uint32_t x = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
        float f;
        std::memcpy(&f, &x, 4);
        return f;
    }

    float Smooth(float e0, float e1, float x) {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    int32_t Stored(int32_t g) {
        return g & (kN - 1);
    }

    // The direction a river runs at its node `i` (of `count` from `first`): along the stretches either
    // side, so a bend turns over both.
    void Tangent(const River& r, uint32_t i, float& tx, float& ty) {
        const Node& a = s.nodes[r.first + (i > 0 ? i - 1 : 0)];
        const Node& b = s.nodes[r.first + (i + 1 < r.count ? i + 1 : i)];
        tx = b.x - a.x;
        ty = b.y - a.y;
        const float length = std::sqrt(tx * tx + ty * ty);

        if (length > 1e-4f) {
            tx /= length;
            ty /= length;
        } else {
            tx = 1.0f;
            ty = 0.0f;
        }
    }

    bool Rough(uint32_t kind) {
        return kind == SXC_WATER_RIVER_RAPIDS || kind == SXC_WATER_RIVER_FALLS;
    }

    const River* RiverById(uint32_t id) {
        for (const River& r : s.rivers) {
            if (r.id == id) {
                return &r;
            }
        }

        return nullptr;
    }

    // The nearest point of a river's line to (x, y): how far, the river's half width and direction there.
    // False for a river not handed over.
    bool NearestOn(const River* r, float x, float y, float& distance, float& half, float& tx, float& ty) {
        if (!r) {
            return false;
        }

        distance = 1e9f;

        for (uint32_t i = 0; i + 1 < r->count; i++) {
            const Node& a = s.nodes[r->first + i];
            const Node& b = s.nodes[r->first + i + 1];
            const float abx = b.x - a.x, aby = b.y - a.y, length2 = abx * abx + aby * aby;
            const float t = length2 > 1e-6f ? std::clamp(((x - a.x) * abx + (y - a.y) * aby) / length2, 0.0f, 1.0f) : 0.0f;
            const float cx = a.x + abx * t - x, cy = a.y + aby * t - y;
            const float d = std::sqrt(cx * cx + cy * cy);

            if (d < distance) {
                const float l = std::sqrt(length2);
                distance = d;
                half = std::max((a.width + (b.width - a.width) * t) * 0.5f, 0.5f);
                tx = l > 1e-3f ? abx / l : 1.0f;
                ty = l > 1e-3f ? aby / l : 0.0f;
            }
        }

        return distance < 1e9f;
    }

    // Where one river's end is attached to another (it joins it, or branches from it), its flow gives way
    // to that river's inside that river's banks: turned along it toward its middle, and weighing less, so
    // the tributary's water merges into the main current instead of shooting across it (and a branch's
    // draws off along the main river). Nothing outside the other river's banks.
    void GiveWay(const River* other, float px, float py, float& w, float& dx, float& dy) {
        float distance, half, tx, ty;

        if (!NearestOn(other, px, py, distance, half, tx, ty) || distance >= half) {
            return;
        }

        const float keep = Smooth(0.0f, half, distance);   // 0 on its middle, 1 at its banks
        dx = tx + (dx - tx) * keep;
        dy = ty + (dy - ty) * keep;
        const float l = std::sqrt(dx * dx + dy * dy);
        dx = l > 1e-4f ? dx / l : tx;
        dy = l > 1e-4f ? dy / l : ty;
        w *= 0.15f + 0.85f * keep;
    }

    // Cells of the window within a world box (cell x runs along world -y, cell y along world -x).
    struct CellBox {
        int32_t gx0, gx1, gy0, gy1;
    };

    CellBox BoxOf(float x0, float y0, float x1, float y1, int32_t originX, int32_t originY) {
        return CellBox{ std::max(static_cast<int32_t>(std::floor((kHalfWorld - y1) / kShoreCell)), originX),
                        std::min(static_cast<int32_t>(std::floor((kHalfWorld - y0) / kShoreCell)), originX + kN - 1),
                        std::max(static_cast<int32_t>(std::floor((kHalfWorld - x1) / kShoreCell)), originY),
                        std::min(static_cast<int32_t>(std::floor((kHalfWorld - x0) / kShoreCell)), originY + kN - 1) };
    }

    void Add(int32_t gx, int32_t gy, float w, float dx, float dy, float speed, float rough) {
        const int32_t k = Stored(gy) * kN + Stored(gx);
        s.fx[k] += w * dx * speed;
        s.fy[k] += w * dy * speed;
        s.rough[k] += w * rough;
        s.sum[k] += w;
        s.top[k] = std::max(s.top[k], w);
    }

    // One stretch (nodes i and i + 1) into the sums, over the window's cells it reaches. Between stretches
    // the ends are round (a bend turns over both); a river's own ends are round only where it is attached
    // to another river: a free end is a mouth or an inlet (DrawEnd).
    void DrawStretch(const River& r, uint32_t i, int32_t originX, int32_t originY) {
        const Node& a = s.nodes[r.first + i];
        const Node& b = s.nodes[r.first + i + 1];
        float ta[2], tb[2];
        Tangent(r, i, ta[0], ta[1]);
        Tangent(r, i + 1, tb[0], tb[1]);

        const float abx = b.x - a.x, aby = b.y - a.y;
        const float length2 = abx * abx + aby * aby, length = std::sqrt(length2);
        const float widest = std::max(a.width, b.width);
        const float reach = widest * 0.5f + std::max(2.0f, widest * 0.25f);
        const CellBox box = BoxOf(std::min(a.x, b.x) - reach, std::min(a.y, b.y) - reach, std::max(a.x, b.x) + reach, std::max(a.y, b.y) + reach, originX, originY);

        const bool firstStretch = i == 0, lastStretch = i + 2 == r.count;
        const River* joins = lastStretch && r.joins ? RiverById(r.joins) : nullptr;
        const River* branchesFrom = firstStretch && r.branchesFrom ? RiverById(r.branchesFrom) : nullptr;
        const bool capStart = !firstStretch || branchesFrom, capEnd = !lastStretch || joins;

        // Rapids ease in and out over a few yards where the stretch before or after is calm, instead of
        // switching at the node.
        const bool rough = Rough(a.kind);
        const bool roughBefore = i > 0 && Rough(s.nodes[r.first + i - 1].kind);
        const bool roughAfter = !lastStretch && Rough(b.kind);
        const float ramp = std::max(std::min(8.0f, length * 0.3f), 0.5f);

        for (int32_t gy = box.gy0; gy <= box.gy1; gy++) {
            const float px = kHalfWorld - (static_cast<float>(gy) + 0.5f) * kShoreCell;

            for (int32_t gx = box.gx0; gx <= box.gx1; gx++) {
                const float py = kHalfWorld - (static_cast<float>(gx) + 0.5f) * kShoreCell;
                const float raw = length2 > 1e-6f ? ((px - a.x) * abx + (py - a.y) * aby) / length2 : 0.0f;

                if ((raw < 0.0f && !capStart) || (raw > 1.0f && !capEnd)) {
                    continue;
                }

                const float t = std::clamp(raw, 0.0f, 1.0f);
                const float cx = a.x + abx * t - px, cy = a.y + aby * t - py;
                const float d = std::sqrt(cx * cx + cy * cy);
                const float half = std::max((a.width + (b.width - a.width) * t) * 0.5f, 0.5f);
                const float fade = std::max(2.0f, half * 0.5f);

                if (d >= half + fade) {
                    continue;
                }

                float w = 1.0f - Smooth(half, half + fade, d);
                float dx = ta[0] + (tb[0] - ta[0]) * t, dy = ta[1] + (tb[1] - ta[1]) * t;
                const float dl = std::sqrt(dx * dx + dy * dy);
                dx = dl > 1e-4f ? dx / dl : abx;
                dy = dl > 1e-4f ? dy / dl : aby;
                // Faster in the middle of the channel than at its sides.
                const float across = std::min(d / half, 1.0f);
                const float speed = (a.speed + (b.speed - a.speed) * t) * (1.0f - 0.35f * across * across);
                const float along = t * length;
                const float roughness = !rough ? 0.0f
                    : std::min(roughBefore ? 1.0f : Smooth(0.0f, ramp, along), roughAfter ? 1.0f : 1.0f - Smooth(length - ramp, length, along));

                if (joins) {
                    GiveWay(joins, px, py, w, dx, dy);
                }

                if (branchesFrom) {
                    GiveWay(branchesFrom, px, py, w, dx, dy);
                }

                Add(gx, gy, w, dx, dy, speed, roughness);
            }
        }
    }

    // A free end of a river: past its last node a mouth, the current spreading out and slowing into the
    // open water it runs into (the same water over a wider front: slower, until it's gone); before its first
    // node an inlet, the water drawn in from the lake it leaves, converging and speeding up toward the node.
    // Over three widths (at least 24 yd); the banks still turn it (Build).
    void DrawEnd(const River& r, bool mouth, int32_t originX, int32_t originY) {
        const uint32_t i = mouth ? r.count - 1 : 0;
        const Node& e = s.nodes[r.first + i];
        float tx, ty;
        Tangent(r, i, tx, ty);
        const float ox = mouth ? tx : -tx, oy = mouth ? ty : -ty;   // away from the river
        const float half0 = std::max(e.width * 0.5f, 0.5f);
        const float reachOut = std::max(3.0f * e.width, 24.0f);
        constexpr float kSpread = 0.6f;   // the front widens this much per yd out
        const float widest = half0 + reachOut * kSpread;
        const float reach = reachOut + widest + std::max(2.0f, widest * 0.5f);
        const float rough = mouth && Rough(s.nodes[r.first + r.count - 2].kind) ? 1.0f : 0.0f;
        const CellBox box = BoxOf(e.x - reach, e.y - reach, e.x + reach, e.y + reach, originX, originY);

        for (int32_t gy = box.gy0; gy <= box.gy1; gy++) {
            const float px = kHalfWorld - (static_cast<float>(gy) + 0.5f) * kShoreCell;

            for (int32_t gx = box.gx0; gx <= box.gx1; gx++) {
                const float py = kHalfWorld - (static_cast<float>(gx) + 0.5f) * kShoreCell;
                const float rx = px - e.x, ry = py - e.y;
                const float out = rx * ox + ry * oy;   // yd out from the node
                const float side = rx * -oy + ry * ox;  // yd to the side

                if (out <= 0.0f || out >= reachOut) {
                    continue;
                }

                const float half = half0 + out * kSpread;
                const float fade = std::max(2.0f, half * 0.5f);

                if (std::fabs(side) >= half + fade) {
                    continue;
                }

                const float w = (1.0f - Smooth(half, half + fade, std::fabs(side))) * (1.0f - Smooth(0.0f, reachOut, out));
                // Along the river, fanning out from its axis at a mouth, in toward it at an inlet (the side
                // is measured the other way round at an inlet, so one turn does both).
                const float fan = kSpread * std::clamp(side / half, -1.0f, 1.0f);
                float dx = tx - fan * ty;
                float dy = ty + fan * tx;
                const float dl = std::sqrt(dx * dx + dy * dy);
                dx /= dl;
                dy /= dl;
                const float across = std::min(std::fabs(side) / half, 1.0f);
                const float speed = e.speed * (half0 / half) * (1.0f - 0.35f * across * across);
                Add(gx, gy, w, dx, dy, speed, rough * (1.0f - Smooth(0.0f, reachOut * 0.5f, out)));
            }
        }
    }

    void Build(const ShoreMap& shore) {
        const size_t cells = static_cast<size_t>(kN) * kN;
        s.fx.assign(cells, 0.0f);
        s.fy.assign(cells, 0.0f);
        s.rough.assign(cells, 0.0f);
        s.sum.assign(cells, 0.0f);
        s.top.assign(cells, 0.0f);

        for (const River& r : s.rivers) {
            for (uint32_t i = 0; i + 1 < r.count; i++) {
                DrawStretch(r, i, shore.originX, shore.originY);
            }

            // Free ends (not attached to a river handed over): a mouth, an inlet.
            if (!r.joins || !RiverById(r.joins)) {
                DrawEnd(r, true, shore.originX, shore.originY);
            }

            if (!r.branchesFrom || !RiverById(r.branchesFrom)) {
                DrawEnd(r, false, shore.originX, shore.originY);
            }
        }

        for (size_t k = 0; k < cells; k++) {
            uint16_t* out = &s.map.texels[k * 4];

            if (s.sum[k] <= 0.0f) {
                out[0] = out[1] = out[2] = out[3] = 0;
                continue;
            }

            float vx = s.fx[k] / s.sum[k], vy = s.fy[k] / s.sum[k];
            float weight = s.top[k];

            // The banks (the shore field: the distance to the shore, positive over water, and the direction
            // to it): the flow slows toward them and turns along a bank it would run into. Land takes none.
            const float distance = FromHalf(shore.texels[k * 4 + 0]);
            const float toX = FromHalf(shore.texels[k * 4 + 2]), toY = FromHalf(shore.texels[k * 4 + 3]);
            const float speed = std::sqrt(vx * vx + vy * vy);
            const float into = vx * toX + vy * toY;

            if (into > 0.0f) {
                const float turn = 1.0f - Smooth(2.0f, 8.0f, distance);
                vx -= toX * into * turn;
                vy -= toY * into * turn;
                const float left = std::sqrt(vx * vx + vy * vy);

                if (left > 1e-4f) {
                    vx *= speed / left;
                    vy *= speed / left;
                }
            }

            const float bank = Smooth(-0.5f, 3.0f, distance);
            vx *= bank;
            vy *= bank;
            weight *= distance > -2.0f ? 1.0f : 0.0f;

            out[0] = Half(vx);
            out[1] = Half(vy);
            out[2] = Half(s.rough[k] / s.sum[k]);
            out[3] = Half(weight);
        }

        s.map.version++;
    }
}

namespace SXC::Water {
    void SetRivers(const sxc_water_river* rivers, uint32_t riverCount, const sxc_water_river_node* nodes, uint32_t nodeCount) {
        s.rivers.clear();
        s.nodes.clear();

        for (uint32_t n = 0; nodes && n < nodeCount; n++) {
            const sxc_water_river_node& in = nodes[n];
            s.nodes.push_back(Node{ in.x, in.y, in.z, std::max(in.width, 1.0f), std::max(in.speed, 0.0f), in.kind });
        }

        for (uint32_t r = 0; rivers && r < riverCount; r++) {
            const sxc_water_river& in = rivers[r];

            if (in.node_count >= 2 && in.first_node + in.node_count <= nodeCount) {
                s.rivers.push_back(River{ in.id, in.first_node, in.node_count, in.joins, in.branches_from });
            }
        }

        s.dirty = true;
    }

    uint32_t RiverCount() {
        return static_cast<uint32_t>(s.rivers.size());
    }

    uint32_t RiverNodeCount() {
        return static_cast<uint32_t>(s.nodes.size());
    }

    const FlowMap& FlowField(const ShoreMap& shore) {
        if (!shore.valid) {
            s.map.valid = false;
            return s.map;
        }

        const bool moved = !s.map.valid || shore.originX != s.map.originX || shore.originY != s.map.originY;

        if (!s.dirty && !moved && shore.version == s.shoreVersion) {
            return s.map;
        }

        // Without rivers, an empty field once.
        if (s.rivers.empty() && !s.dirty && s.map.valid) {
            s.map.originX = shore.originX;
            s.map.originY = shore.originY;
            s.shoreVersion = shore.version;
            return s.map;
        }

        Build(shore);
        s.map.valid = true;
        s.map.originX = shore.originX;
        s.map.originY = shore.originY;
        s.shoreVersion = shore.version;
        s.dirty = false;
        return s.map;
    }

    bool FlowAt(float x, float y, float& flowX, float& flowY, float& weight) {
        if (!s.map.valid) {
            return false;
        }

        const int32_t gx = static_cast<int32_t>(std::floor((kHalfWorld - y) / kShoreCell));
        const int32_t gy = static_cast<int32_t>(std::floor((kHalfWorld - x) / kShoreCell));

        if (gx < s.map.originX || gy < s.map.originY || gx >= s.map.originX + kN || gy >= s.map.originY + kN) {
            return false;
        }

        const uint16_t* t = &s.map.texels[(static_cast<size_t>(Stored(gy)) * kN + Stored(gx)) * 4];
        flowX = FromHalf(t[0]);
        flowY = FromHalf(t[1]);
        weight = FromHalf(t[3]);
        return true;
    }
}
