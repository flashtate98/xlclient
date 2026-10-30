// SXC: the shore field (SXC/Water/Shore.h; Vulkan/Water.cpp binding 5), shared by Water.vert and Water.frag:
// around the player, per cell of a sixteenth of a terrain chunk, the signed distance to the shore (yd, positive
// over water), the depth (surface minus ground, yd) and the direction to the shore (world x, y). Stored
// toroidally (a cell at global (gx, gy) at (gx & 255, gy & 255)), so it is read with a repeating sampler at the
// global cell coordinates; `weight` is 0 outside the field's window and fades in over its last 16 cells.
// Terrain liquid only: WMO water would read the terrain's shores, so it passes terrain = false (p14.w 0).
layout(set = 2, binding = 5) uniform sampler2D shoreMap;
// The near mesh's cells (Shore.h's ShoreMap::cells), read exactly: the surface (world z), the ground, the
// LiquidType, 0 where the near mesh doesn't cover the cell, else 1 plus the stock mesh's depth coordinate.
layout(set = 2, binding = 6) uniform sampler2D shoreCells;

const float kShoreHalfWorld = 32.0 * 533.33333;

vec4 ShoreAt(vec2 world, out float weight)
{
    weight = 0.0;

    if (scene.shoreGrid.z < 0.5 || regs.p[14].w < 0.5)
    {
        return vec4(64.0, 50.0, 0.0, 0.0);
    }

    vec2 g = vec2(kShoreHalfWorld - world.y, kShoreHalfWorld - world.x) / scene.shoreGrid.w;
    vec2 inside = g - scene.shoreGrid.xy;
    vec2 edge = min(inside, vec2(256.0) - inside);
    weight = clamp((min(edge.x, edge.y) - 2.0) / 14.0, 0.0, 1.0);
    return textureLod(shoreMap, g / 256.0, 0.0);
}

// The cell a world point lies in (global cell coordinates) and its near-mesh data.
ivec2 ShoreCellOf(vec2 world)
{
    return ivec2(floor(vec2(kShoreHalfWorld - world.y, kShoreHalfWorld - world.x) / scene.shoreGrid.w));
}

vec4 ShoreCell(ivec2 g)
{
    return texelFetch(shoreCells, g & 255, 0);
}

// Whether the near mesh draws at a world point: inside the window (the cells are stored toroidally, so
// outside it they would answer for the window's other side: Seyri, 2026-10-30, the stock water cut into a
// repeating coast), in a triangle of its grid (vertices at the cell centres, each quad split from (i+1, j)
// to (i, j+1), as SXC/Water's NearBuffers builds it) with all three corners covered. Exactly where the near
// mesh is, so the stock water gives way without a gap.
bool NearCovers(vec2 world)
{
    if (scene.shoreGrid.z < 0.5)
    {
        return false;
    }

    vec2 u = vec2(kShoreHalfWorld - world.y, kShoreHalfWorld - world.x) / scene.shoreGrid.w - 0.5 - scene.shoreGrid.xy;
    ivec2 i0 = ivec2(floor(u));

    if (any(lessThan(i0, ivec2(0))) || any(greaterThan(i0, ivec2(254))))
    {
        return false;
    }

    vec2 f = u - vec2(i0);
    ivec2 o = ivec2(scene.shoreGrid.xy) + i0;
    ivec2 a = f.x + f.y <= 1.0 ? o : o + ivec2(1, 1);
    return ShoreCell(a).w > 0.5 && ShoreCell(o + ivec2(1, 0)).w > 0.5 && ShoreCell(o + ivec2(0, 1)).w > 0.5;
}

// Value noise over the world (yd); hashed without sin (precise far from the origin: the world is wrapped every
// 1024 yd first).
float Hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float Noise(vec2 p)
{
    p = mod(p, 1024.0);
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(Hash(i), Hash(i + vec2(1.0, 0.0)), f.x), mix(Hash(i + vec2(0.0, 1.0)), Hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

const float kShoreTau = 6.2831853;

// The shore waves' wave number and speed: one speed for every pixel (a speed by depth would shear the bands
// as the clock runs), a whole number of turns per 600 s (the clock's wrap, p6.x).
float ShoreK()
{
    return kShoreTau / max(scene.shoreLook.z, 1.0);
}

float ShoreOmega()
{
    return max(round(ShoreK() * 3.0 * 600.0 / kShoreTau), 1.0) * kShoreTau / 600.0;   // ~3 yd/s
}

// The phase at the waterline here: a crest arrives when its sine peaks; bent by noise so the bands aren't
// drawn with a ruler.
float ShoreArrival(vec2 world, float time)
{
    return ShoreOmega() * time + 2.5 * Noise(world / 24.0);
}

// Exposure: waves come from the open sea, downwind. A shore gets them by how much it faces into the waves'
// direction and how much open water lies upwind of it (the field read 20 and 50 yd upwind): the lee of an
// island and a sheltered bay stay calm (Seyri, 2026-10-30: islands were ringed). A stronger wind, higher.
float ShoreExposure(vec2 world, vec4 shore)
{
    vec2 wind = scene.shoreWind.xy;
    float facing = smoothstep(-0.2, 0.6, dot(wind, shore.zw));
    float w1, w2;
    float up1 = ShoreAt(world - wind * 20.0, w1).x;
    float up2 = ShoreAt(world - wind * 50.0, w2).x;
    float open = 0.5 * mix(1.0, smoothstep(0.0, 15.0, up1), w1) + 0.5 * mix(1.0, smoothstep(0.0, 15.0, up2), w2);
    return mix(0.1, 1.0, facing * open) * clamp(scene.shoreWind.z / 8.0, 0.25, 1.5);
}

// The swash at the waterline (0..1): a crest arriving pushes it out a moment after it breaks, then it slides
// back.
float ShoreSwash(float arrival)
{
    float s = 0.5 + 0.5 * sin(arrival - 0.8);
    return s * s;
}
