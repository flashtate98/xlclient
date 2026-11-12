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

// The covers' height at a point of the triangle (position and normal interpolated like Terrain.tese does).
float HeightAt(vec3 position, vec3 normal)
{
    vec3 n = normal * inversesqrt(max(dot(normal, normal), 1e-12));
    vec4 alpha = textureLod(sAlpha, TerrainCoord(LAYERS, position.xy), 0.0);
    return CoverHeight(position.xy, position.z, n.z, CoverLayerWeights(alpha, kBigAlpha));
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

    if (max(max(ha, hb), hm) <= 0.0)
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

            if (cover.tess.w > 0.5 && distance <= cover.tess.y && HeightAt(centre, patchNormal[0] + patchNormal[1] + patchNormal[2]) > 0.0)
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
