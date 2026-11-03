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

    // One stretch (nodes i and i + 1) into the sums, over the window's cells it reaches.
    void DrawStretch(const River& r, uint32_t i, int32_t originX, int32_t originY) {
        const Node& a = s.nodes[r.first + i];
        const Node& b = s.nodes[r.first + i + 1];
        float ta[2], tb[2];
        Tangent(r, i, ta[0], ta[1]);
        Tangent(r, i + 1, tb[0], tb[1]);

        const float abx = b.x - a.x, aby = b.y - a.y;
        const float length2 = abx * abx + aby * aby;
        const float widest = std::max(a.width, b.width);
        const float reach = widest * 0.5f + std::max(2.0f, widest * 0.25f);

        // The cells of the stretch's box (cell x runs along world -y, cell y along world -x).
        const float x0 = std::min(a.x, b.x) - reach, x1 = std::max(a.x, b.x) + reach;
        const float y0 = std::min(a.y, b.y) - reach, y1 = std::max(a.y, b.y) + reach;
        const int32_t gx0 = std::max(static_cast<int32_t>(std::floor((kHalfWorld - y1) / kShoreCell)), originX);
        const int32_t gx1 = std::min(static_cast<int32_t>(std::floor((kHalfWorld - y0) / kShoreCell)), originX + kN - 1);
        const int32_t gy0 = std::max(static_cast<int32_t>(std::floor((kHalfWorld - x1) / kShoreCell)), originY);
        const int32_t gy1 = std::min(static_cast<int32_t>(std::floor((kHalfWorld - x0) / kShoreCell)), originY + kN - 1);
        const float rough = a.kind == SXC_WATER_RIVER_RAPIDS || a.kind == SXC_WATER_RIVER_FALLS ? 1.0f : 0.0f;

        for (int32_t gy = gy0; gy <= gy1; gy++) {
            const float px = kHalfWorld - (static_cast<float>(gy) + 0.5f) * kShoreCell;

            for (int32_t gx = gx0; gx <= gx1; gx++) {
                const float py = kHalfWorld - (static_cast<float>(gx) + 0.5f) * kShoreCell;
                const float t = length2 > 1e-6f ? std::clamp(((px - a.x) * abx + (py - a.y) * aby) / length2, 0.0f, 1.0f) : 0.0f;
                const float cx = a.x + abx * t - px, cy = a.y + aby * t - py;
                const float d = std::sqrt(cx * cx + cy * cy);
                const float half = std::max((a.width + (b.width - a.width) * t) * 0.5f, 0.5f);
                const float fade = std::max(2.0f, half * 0.5f);

                if (d >= half + fade) {
                    continue;
                }

                const float w = 1.0f - Smooth(half, half + fade, d);
                float dx = ta[0] + (tb[0] - ta[0]) * t, dy = ta[1] + (tb[1] - ta[1]) * t;
                const float dl = std::sqrt(dx * dx + dy * dy);
                dx = dl > 1e-4f ? dx / dl : abx;
                dy = dl > 1e-4f ? dy / dl : aby;
                // Faster in the middle of the channel than at its sides.
                const float across = std::min(d / half, 1.0f);
                const float speed = (a.speed + (b.speed - a.speed) * t) * (1.0f - 0.35f * across * across);

                const int32_t k = Stored(gy) * kN + Stored(gx);
                s.fx[k] += w * dx * speed;
                s.fy[k] += w * dy * speed;
                s.rough[k] += w * rough;
                s.sum[k] += w;
                s.top[k] = std::max(s.top[k], w);
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
