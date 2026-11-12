// SXC: the grass density (port plan step 7d; WXL's density copies, features/GrassInstanced), in the
// instanced grass variant of the DetailDoodad vertex modules (the translator inlines this file after
// GrassMotion.glsl; Tools/VulkanShaders). The chunks near the player draw their plants again once per copy
// (SXC/Grass); copy k of a plant turns about its own up axis and moves toward a random point of the plant's
// own terrain triangle, at most `spread` yards, so the triangle's plane (the plant's normal) gives its
// height exactly (a triangle is convex: the way there never leaves it). Copies grow in with the distance:
// all of them within half the radius of the player, none at the radius.
//
// The rows the client uploads (SXC/Grass):
//   c148  the player's position (x, y), the most copies, the most radius (the player's limits)
//   c149  the most spread, 0, 0, 0
// Per plant (location 6): copies, radius, spread, 0; (location 7) 2 x its triangle + s, t: its spot in its
// terrain cell (0..1; s along the chunk's -y, t along its -x). Per run (location 8): the copy number.

layout(location = 6) in vec4 sxcPlantDensity;
layout(location = 7) in vec2 sxcPlantCell;
layout(location = 8) in vec4 sxcCopy;

const int kSxcGrassDensity = 148;
const float kSxcCellSize = 4.1666665;

uint SxcHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The copy's model-space position (turned) and its shift in world space; returns its size (0: not
// drawn, 1: full). The plant itself (copy 0) stays as it is.
float SxcGrassCopy(inout vec3 m, vec3 normal, out vec3 shift)
{
    shift = vec3(0.0);
    float k = sxcCopy.x;

    if (k < 0.5)
    {
        return 1.0;
    }

    vec4 d0 = regs.v[kSxcGrassDensity];
    vec4 d1 = regs.v[kSxcGrassDensity + 1];
    vec2 root = vec2(sxcPlant0.w, sxcPlant1.w);
    float radius = min(sxcPlantDensity.y, d0.w);
    float copies = min(sxcPlantDensity.x, d0.z);
    float amount = copies * clamp(2.0 - 2.0 * length(root - d0.xy) / max(radius, 0.001), 0.0, 1.0);

    // Four random numbers per plant and copy (from its position: the same every frame).
    uint seed = SxcHash(floatBitsToUint(root.x) ^ SxcHash(floatBitsToUint(root.y) ^ (uint(k) * 0x9e3779b9u)));
    vec4 r = vec4(uvec4(seed, SxcHash(seed + 1u), SxcHash(seed + 2u), SxcHash(seed + 3u)) >> 8u) * (1.0 / 16777216.0);

    // Copy k appears once the amount passes k - 1 (a random part later), grown in over a quarter copy.
    float grow = clamp((amount - (k - 1.0) - 0.75 * r.w) * 4.0, 0.0, 1.0);

    if (grow <= 0.0)
    {
        return 0.0;
    }

    // Its own turn about the model's up axis.
    float turn = r.z * 6.2831853;
    float c = cos(turn), s = sin(turn);
    m.xy = vec2(m.x * c - m.y * s, m.x * s + m.y * c);

    // The plant's triangle: the cell's centre and two corners; 0 (s <= t, s + t <= 1) the corners (0, 0)
    // and (0, 1), 1 (0, 0) and (1, 0), 2 (0, 1) and (1, 1), 3 (1, 0) and (1, 1).
    float tri = floor(sxcPlantCell.x * 0.5);
    vec2 spot = vec2(sxcPlantCell.x - 2.0 * tri, sxcPlantCell.y);
    vec2 a = vec2(tri == 3.0 ? 1.0 : 0.0, tri == 2.0 ? 1.0 : 0.0);
    vec2 b = vec2(tri == 0.0 ? 0.0 : 1.0, tri == 1.0 ? 0.0 : 1.0);

    // A uniform point of it (folded when u + v > 1), the way there in yards (s runs along -y, t along -x).
    vec2 uv = r.x + r.y > 1.0 ? 1.0 - r.xy : r.xy;
    vec2 target = vec2(0.5) + uv.x * (a - vec2(0.5)) + uv.y * (b - vec2(0.5));
    vec2 way = -(target - spot).yx * kSxcCellSize;
    float len = length(way);
    float spread = min(sxcPlantDensity.z, d1.x);
    way *= len > spread ? spread / max(len, 0.0001) : 1.0;

    // The height from the triangle's plane.
    shift = vec3(way, -(normal.x * way.x + normal.y * way.y) / max(normal.z, 0.1));
    return grow;
}
