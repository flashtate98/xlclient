// SXC: how finely a raised terrain chunk's triangles are subdivided (step 5c). An edge is split only
// where cover lies along it (the covers' height at its ends and middle): elsewhere it stays whole, so
// bare ground (rock, road) keeps exactly the stock vertices and their light. A covered edge is split by
// its length against its distance from the camera, so a segment covers about the same part of the
// screen; whole beyond the reach. An edge's level depends on that edge alone, so the triangles on both
// sides of it split it alike (across a chunk border the two chunks' heights can differ a little: the
// underlay beneath shows ground there).
//
// Compiled once per layer count (the alpha map's binding is the layer count); big alpha (7) from the
// pixel record.
#version 450
#extension GL_GOOGLE_include_directive : require

#ifndef LAYERS
#define LAYERS 1
#endif
#define COVER_DEPTH_ONLY 1

layout(vertices = 3) out;

layout(constant_id = 7) const bool kBigAlpha = false;

#include "TerrainRegs.glsl"
#include "TerrainCover.glsl"

layout(set = 0, binding = LAYERS) uniform sampler2D sAlpha;

layout(location = 0) in vec3 patchPosition[];
layout(location = 1) in vec3 patchNormal[];
layout(location = 2) in vec4 patchColor[];

layout(location = 0) out vec3 tessPosition[];
layout(location = 1) out vec3 tessNormal[];
layout(location = 2) out vec4 tessColor[];

// The view-space position of a world point (v0..v3: world x view, rows).
vec3 View(vec3 p)
{
    return (regs.v[0] * p.x + regs.v[1] * p.y + regs.v[2] * p.z + regs.v[3]).xyz;
}

// The clip position of a world point, as TerrainVertex.glsl makes it (v0..v3 world x view, v4..v7 the
// projection; D3D clip space: x, y within +-w, z within 0..w).
vec4 Clip(vec3 p)
{
    vec4 pos = regs.v[0] * p.x + regs.v[1] * p.y + regs.v[2] * p.z + regs.v[3];
    return regs.v[4] * pos.x + regs.v[5] * pos.y + regs.v[6] * pos.z + regs.v[7] * pos.w;
}

// The most the covers raise a point and the most the trails press it below the ground (yd), with room.
const float kMostRaise = 6.0;
const float kMostBelow = 2.0;

// True when no point of the triangle, raised or pressed anywhere within those, can be in view: all six
// corners of its prism beyond one side of the view. The chunks are culled whole before they are drawn;
// a chunk at the edge of the view or under the camera is mostly out of it, and its triangles there were
// subdivided all the same (FlashWOW, 2026-11-23: about 18 ms of GPU on the snow at Everlook).
bool OutOfView()
{
    vec4 c[6];

    for (int k = 0; k < 3; k++)
    {
        c[k * 2] = Clip(patchPosition[k] - vec3(0.0, 0.0, kMostBelow));
        c[k * 2 + 1] = Clip(patchPosition[k] + vec3(0.0, 0.0, kMostRaise));
    }

    bool left = true, right = true, below = true, above = true, near = true, far = true;

    for (int k = 0; k < 6; k++)
    {
        left = left && c[k].x < -c[k].w;
        right = right && c[k].x > c[k].w;
        below = below && c[k].y < -c[k].w;
        above = above && c[k].y > c[k].w;
        near = near && c[k].z < 0.0;
        far = far && c[k].z > c[k].w;
    }

    return left || right || below || above || near || far;
}

// Cover thinner than this is drawn flat (its look stays: the colour, the wet, the mud): splitting the
// ground for a centimetre of rise cost the most of the terrain's GPU where a thin cover lies everywhere
// (Elwynn's dirt is 0.01 yd: about 7.5 of 10.8 ms in Goldshire, FlashWOW 2026-11-23). Snow and sand
// drifts are tenths of a yard, and trails show in them as before.
const float kSplitFrom = 0.03;

// SXC (FlashWOW): a painted cover below the terrain (mud: only its top shows) counts toward the split only
// where trails lie: it is there for prints to press into. Counted everywhere, rain's wet mud split all the
// covered ground in reach (Goldshire in rain, 24 players: terrain 12.7 ms of GPU with Depth on, 3.5 off).
// Set per triangle from the coarse trail layer, which every print is stamped into (SXC/Cover/Trails.cpp).
bool g_trailNear = true;

// Whether a trail lies in the box (xy, a cell's margin): the coarse layer's cells there, untouched ones
// at 1e6 with no rim (Vulkan/Cover's kUntouched). A box past 24 cells is taken to hold one.
bool TrailIn(vec2 lo, vec2 hi)
{
    vec4 m = cover.trailCoarse;
    int n = int(m.y + 0.5);

    if (cover.trail.w < 0.5 || n < 1)
    {
        return false;
    }

    ivec2 first = ivec2(m.zw);
    ivec2 a = max(ivec2(floor(lo / m.x)) - 1, first);
    ivec2 b = min(ivec2(floor(hi / m.x)) + 1, first + ivec2(n - 1));

    if (a.x > b.x || a.y > b.y)
    {
        return false;
    }

    if (b.x - a.x > 24 || b.y - a.y > 24)
    {
        return true;
    }

    int lift = n * (int(17100.0 / (m.x * float(n))) + 2);   // as CoverTrailSample: the cell mod n of a positive number

    for (int j = a.y; j <= b.y; j++)
    {
        for (int i = a.x; i <= b.x; i++)
        {
            vec4 t = texelFetch(sTrailCoarse, ivec2(uvec2(ivec2(i, j) + lift) % uint(n)), 0);

            if (t.x < 0.5e6 || t.z > 0.0)
            {
                return true;
            }
        }
    }

    return false;
}

// The covers' height at a point of the triangle (position and normal interpolated like Terrain.tese does):
// CoverHeight's, the part below the terrain only where trails lie (g_trailNear).
float HeightAt(vec3 position, vec3 normal)
{
    if (cover.enable.x < 0.5)
    {
        return 0.0;
    }

    vec3 n = normal * inversesqrt(max(dot(normal, normal), 1e-12));
    vec4 alpha = textureLod(sAlpha, TerrainCoord(LAYERS, position.xy), 0.0);
    CoverSpot s = CoverEvaluate(position.xy, position.z, n.z, CoverLayerWeights(alpha, kBigAlpha));
    return s.paintedHeight + s.snowHeight + (g_trailNear && cover.trail.w > 0.5 ? s.below : 0.0);
}

// An edge from corner a to corner b: split where cover lies along it and it's within the reach.
float EdgeLevel(int a, int b, float ha, float hb)
{
    vec3 pa = patchPosition[a], pb = patchPosition[b];
    float distance = length(View((pa + pb) * 0.5));

    if (cover.tess.w < 0.5 || distance > cover.tess.y)
    {
        return 1.0;
    }

    float hm = HeightAt((pa + pb) * 0.5, (patchNormal[a] + patchNormal[b]) * 0.5);

    if (max(max(ha, hb), hm) < kSplitFrom)
    {
        return 1.0;
    }

    float segment = max(distance * cover.tess.x, 0.05);
    return clamp(length(pb - pa) / segment, 1.0, max(cover.tess.z, 1.0));
}

void main()
{
    tessPosition[gl_InvocationID] = patchPosition[gl_InvocationID];
    tessNormal[gl_InvocationID] = patchNormal[gl_InvocationID];
    tessColor[gl_InvocationID] = patchColor[gl_InvocationID];

    if (gl_InvocationID == 0)
    {
        // Out of view: levels of 0 drop the triangle before anything is made from it.
        if (OutOfView())
        {
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelInner[0] = 0.0;
            return;
        }

        // Beyond the reach (or no raising): whole, without reading the covers at its corners. Each edge's
        // level checks its own middle's distance; that is at most half the longest edge nearer than the
        // nearest corner.
        float d0 = length(View(patchPosition[0])), d1 = length(View(patchPosition[1])), d2 = length(View(patchPosition[2]));
        float longest = max(length(patchPosition[1] - patchPosition[0]),
                            max(length(patchPosition[2] - patchPosition[1]), length(patchPosition[0] - patchPosition[2])));

        if (cover.tess.w < 0.5 || min(d0, min(d1, d2)) - longest * 0.5 > cover.tess.y)
        {
            gl_TessLevelOuter[0] = 1.0;
            gl_TessLevelOuter[1] = 1.0;
            gl_TessLevelOuter[2] = 1.0;
            gl_TessLevelInner[0] = 1.0;
            return;
        }

        vec2 lo = min(patchPosition[0].xy, min(patchPosition[1].xy, patchPosition[2].xy));
        vec2 hi = max(patchPosition[0].xy, max(patchPosition[1].xy, patchPosition[2].xy));
        g_trailNear = TrailIn(lo, hi);

        float h0 = HeightAt(patchPosition[0], patchNormal[0]);
        float h1 = HeightAt(patchPosition[1], patchNormal[1]);
        float h2 = HeightAt(patchPosition[2], patchNormal[2]);
        // Outer level k belongs to the edge opposite corner k.
        float e0 = EdgeLevel(1, 2, h1, h2);
        float e1 = EdgeLevel(2, 0, h2, h0);
        float e2 = EdgeLevel(0, 1, h0, h1);
        float inner = max(e0, max(e1, e2));

        // Cover inside the triangle only (none along its edges): split the inside alone.
        if (inner <= 1.0)
        {
            vec3 centre = (patchPosition[0] + patchPosition[1] + patchPosition[2]) / 3.0;
            float distance = length(View(centre));

            if (cover.tess.w > 0.5 && distance <= cover.tess.y && HeightAt(centre, patchNormal[0] + patchNormal[1] + patchNormal[2]) >= kSplitFrom)
            {
                float side = max(length(patchPosition[1] - patchPosition[0]), length(patchPosition[2] - patchPosition[0]));
                inner = clamp(side / max(distance * cover.tess.x, 0.05), 1.0, max(cover.tess.z, 1.0));
            }
        }

        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelInner[0] = inner;
    }
}
