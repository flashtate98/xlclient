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
// The rivers' flow on the same cells (SXC/Water/Flow.h): the flow in world x and y (yd / s), roughness (0
// calm, 1 rapids), how much of a river is there (0..1).
layout(set = 2, binding = 8) uniform sampler2D flowMap;

const float kShoreHalfWorld = 32.0 * 533.33333;

// Whether this draw is terrain water (p14.w); WaterQuery.comp asks for terrain water only and sets it to 1.
#ifndef SHORE_TERRAIN
#define SHORE_TERRAIN regs.p[14].w
#endif

vec4 ShoreAt(vec2 world, out float weight)
{
    weight = 0.0;

    if (scene.shoreGrid.z < 0.5 || SHORE_TERRAIN < 0.5)
    {
        return vec4(64.0, 50.0, 0.0, 0.0);
    }

    vec2 g = vec2(kShoreHalfWorld - world.y, kShoreHalfWorld - world.x) / scene.shoreGrid.w;
    vec2 inside = g - scene.shoreGrid.xy;
    vec2 edge = min(inside, vec2(256.0) - inside);
    weight = clamp((min(edge.x, edge.y) - 2.0) / 14.0, 0.0, 1.0);
    return textureLod(shoreMap, g / 256.0, 0.0);
}

// The rivers' flow at a world point (as above); nothing outside the shore field or on other water.
vec4 FlowAt(vec2 world)
{
    if (scene.shoreGrid.z < 0.5 || SHORE_TERRAIN < 0.5)
    {
        return vec4(0.0);
    }

    vec2 g = vec2(kShoreHalfWorld - world.y, kShoreHalfWorld - world.x) / scene.shoreGrid.w;
    vec2 inside = g - scene.shoreGrid.xy;
    vec2 edge = min(inside, vec2(256.0) - inside);
    float weight = clamp((min(edge.x, edge.y) - 2.0) / 14.0, 0.0, 1.0);
    return textureLod(flowMap, g / 256.0, 0.0) * weight;
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

// The still surface's slope at a world point (dz per yd in world x and y), from the near mesh's cell heights
// around it (zero outside the window, where any of them isn't covered, or across two levels). Not from the
// screen derivatives of the moving surface: the mesh's waves, interpolated per triangle, made them jump at
// every triangle edge, and the side test flipped along them in long seams (Seyri, 2026-11-03).
vec2 StillGrade(vec2 world)
{
    if (scene.shoreGrid.z < 0.5)
    {
        return vec2(0.0);
    }

    vec2 u = vec2(kShoreHalfWorld - world.y, kShoreHalfWorld - world.x) / scene.shoreGrid.w - 0.5 - scene.shoreGrid.xy;
    ivec2 i0 = ivec2(floor(u));

    if (any(lessThan(i0, ivec2(0))) || any(greaterThan(i0, ivec2(254))))
    {
        return vec2(0.0);
    }

    vec2 f = u - vec2(i0);
    ivec2 o = ivec2(scene.shoreGrid.xy) + i0;
    vec4 c00 = ShoreCell(o), c10 = ShoreCell(o + ivec2(1, 0)), c01 = ShoreCell(o + ivec2(0, 1)), c11 = ShoreCell(o + ivec2(1, 1));

    if (min(min(c00.w, c10.w), min(c01.w, c11.w)) < 0.5)
    {
        return vec2(0.0);
    }

    float du = mix(c10.x - c00.x, c11.x - c01.x, f.y), dv = mix(c01.x - c00.x, c11.x - c10.x, f.x);

    if (max(abs(du), abs(dv)) > 2.0)
    {
        return vec2(0.0);   // two levels (Shore.cpp's kSlope)
    }

    // Cell x runs along world -y, cell y along world -x.
    return vec2(-dv, -du) / scene.shoreGrid.w;
}

// Value noise over the world (yd); hashed without sin (precise far from the origin: the lattice points are
// wrapped every 1024 before hashing, each on its own, so the noise repeats without a seam; wrapping the point
// first broke it where the wrap fell, along world x = 0 and y = 0 among others: Seyri, 2026-10-31).
float Hash(vec2 p)
{
    p = mod(p, 1024.0);
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float Noise(vec2 p)
{
    vec2 i = mod(floor(p), 1024.0), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(Hash(i), Hash(i + vec2(1.0, 0.0)), f.x), mix(Hash(i + vec2(0.0, 1.0)), Hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// A rate of whole turns per 600 s (the clock's wrap, p6.x) near `seconds` per turn, so nothing jumps
// when the clock wraps.
float TurnsPer600(float seconds)
{
    return max(round(600.0 / seconds), 1.0) * 6.2831853 / 600.0;
}

// Cellular noise over the world: the distances to the nearest and the second-nearest of one point per
// unit cell, and the nearest's own random value. The points wander around their places over time.
vec3 Cells(vec2 p, float time)
{
    vec2 i = mod(floor(p), 1024.0), f = fract(p);
    float d1 = 8.0, d2 = 8.0, id = 0.0;
    float w = TurnsPer600(3.0);

    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            vec2 o = vec2(float(x), float(y));
            vec2 h = vec2(Hash(i + o), Hash(i + o + 17.0));
            vec2 r = o + 0.5 + 0.35 * sin(w * time + 6.2831853 * h) - f;
            float d = dot(r, r);

            if (d < d1)
            {
                d2 = d1;
                d1 = d;
                id = h.x;
            }
            else if (d < d2)
            {
                d2 = d;
            }
        }
    }

    return vec3(sqrt(vec2(d1, d2)), id);
}

// The shore foam's pattern at a point (yd) and time (s), 0..1, higher where frothier (mean about a half):
// patches of froth (value noise) laced with bubbles (cells: dark holes at their centres, bright where they
// meet), and finer bubbles in them. Alive, not just carried (Seyri, 2026-10-30: it looked like a scrolling
// band): the patches churn (their noise warped by noise that circles), the bubbles wander, and each grows
// and pops on its own timing. `pixel`: yd per pixel; each scale fades to its mean where a pixel covers
// it, so distant foam doesn't shimmer. (Value noise alone looked soft and low-resolution.)
float FoamPattern(vec2 p, float pixel, float time)
{
    float churn = TurnsPer600(9.0) * time;
    vec2 circle = vec2(cos(churn), sin(churn)) * 3.0;
    vec2 warp = vec2(Noise(p * 0.3 + circle), Noise(p * 0.3 - circle.yx + 7.0)) - 0.5;
    vec2 q = p + warp * 2.0;
    float patches = Noise(q * 0.45) * 0.6 + Noise(q * 1.3) * 0.4;

    vec3 c = Cells(q * 1.6, time);
    // The hole grows from nothing to its full size, then pops (back to nothing): every 2.5 s, each
    // bubble at its own moment.
    float grown = fract(time * (round(600.0 / 2.5) / 600.0) + c.z);
    float hole = 1.0 - smoothstep(0.05 + 0.4 * grown - 0.12, 0.05 + 0.4 * grown, c.x);
    float bubbles = (1.0 - hole) * 0.6 + (1.0 - smoothstep(0.0, 0.2, c.y - c.x)) * 0.4;
    bubbles = mix(0.55, bubbles, 1.0 - smoothstep(0.15, 0.5, pixel * 1.6));
    float fine = smoothstep(0.1, 0.55, Cells(q * 5.0 + 31.0, time).x);
    fine = mix(0.6, fine, 1.0 - smoothstep(0.15, 0.5, pixel * 5.0));
    return patches * 0.55 + bubbles * 0.3 + fine * 0.15;
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
