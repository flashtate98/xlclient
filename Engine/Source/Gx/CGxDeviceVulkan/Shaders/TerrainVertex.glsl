// SXC: our terrain shader's vertex math (the stock records', operation by operation), shared by the
// vertex shader (Terrain.vert: the flat terrain) and the tessellation's evaluation shader (Terrain.tese:
// the same math at the point raised onto the covers, step 5c). The includer declares the stage's inputs.
//
// The translated interface (Tools/VulkanShaders): varyings at their semantics' fixed locations, the
// engine's constant registers in Regs (SVertexShaderConstants, MapRenderChunk.h).
//
// Registers: v0..v3 world x view (rows), v4..v7 projection, v8..v10 view transposed, v12 fog (x, y,
// exponent), v13..v17 the coordinates' offsets, v18..v22 their scales, v23 the chunk's position, v24
// the sun's direction (view space), v25 ambient, v26 diffuse, v27 specular (w = exponent), v28..v36
// three point lights (position, colour, attenuation), v37..v48 the shadow cascades' rows.

layout(constant_id = 0) const int  kLayers      = 1;     // texture layers, 1..4 (their coordinates + the alpha map's)
layout(constant_id = 1) const bool kShadow      = false; // shadow cascades' coordinates (and the layers' packed two per varying)
layout(constant_id = 2) const bool kEnv         = false; // layer 0 by the reflection vector (a reflective layer)
layout(constant_id = 3) const bool kVertexColor = false; // the vertex colour scales the light (else 0.5)
layout(constant_id = 4) const bool kSpecular    = false; // the sun's highlight in color1
layout(constant_id = 5) const bool kPointLights = false; // three point lights

layout(location = 0) out vec4 color0;
layout(location = 1) out vec4 color1;
layout(location = 2) out vec4 texcoord0;
layout(location = 3) out vec4 texcoord1;
layout(location = 4) out vec4 texcoord2;
layout(location = 5) out vec4 texcoord3;
layout(location = 6) out vec4 texcoord4;
layout(location = 7) out vec4 texcoord5;
layout(location = 8) out vec4 texcoord6;
layout(location = 10) out float fog0;
// For the covers (Terrain.frag, TerrainCover.glsl): the world position and how much the terrain faces
// up, the view position and normal (the wet shine per pixel), the factor the vertex colour put into
// color0 (rows that don't use it take it out again) with the terrain's own height in w.
layout(location = 11) out vec4 coverWorld;
layout(location = 12) out vec4 coverView;
layout(location = 13) out vec4 coverNormal;
layout(location = 14) out vec4 coverTint;

#include "TerrainRegs.glsl"

// D3D9 semantics of the scalar ops (as the translator writes them).
float dx_rsq(float x) { return inversesqrt(abs(x)); }
float dx_pow(float x, float y) { return x == 0.0 ? 0.0 : exp2(y * log2(abs(x))); }
vec3 dx_nrm(vec3 v) { return v * inversesqrt(dot(v, v)); }

// A world position in view space.
vec4 TerrainViewPosition(vec4 inPosition)
{
    vec4 pos = regs.v[1] * inPosition.yyyy;
    pos = inPosition.xxxx * regs.v[0] + pos;
    pos = inPosition.zzzz * regs.v[2] + pos;
    return pos + regs.v[3];
}

// A world normal in view space (not normalised, as stock).
vec3 TerrainViewNormal(vec4 inNormal)
{
    vec3 n = regs.v[1].xyz * inNormal.yyy;
    n = inNormal.xxx * regs.v[0].xyz + n;
    return inNormal.zzz * regs.v[2].xyz + n;
}

// Fog: min(pow(max(z x + y, 0), exponent), 1).
float TerrainFog(vec4 pos)
{
    float f = pos.z * regs.v[12].x + regs.v[12].y;
    f = max(f, 0.0);
    f = dx_pow(f, regs.v[12].z);
    return min(f, 1.0);
}

// The light at a view position with a view normal: color0 (the sun's diffuse and the ambient, the
// point lights, clamped, x the vertex colour or 0.5) and color1 (the sun's highlight).
void TerrainLight(vec4 pos, vec3 n, vec4 inColor, out vec4 c0, out vec4 c1)
{
    // The sun's highlight.
    vec4 spec = vec4(0.0);

    if (kSpecular)
    {
        float r = dot(-pos.xyz, -pos.xyz);
        r = dx_rsq(r);
        vec3 h = -pos.xyz * r + regs.v[24].xyz;
        h = dx_nrm(h);
        float s = dot(h, n);
        s = max(s, 0.0);
        spec.xyz = dx_pow(s, regs.v[27].w) * regs.v[27].xyz;
        spec.w = 1.0;
    }

    // The sun's diffuse light and the ambient, then the point lights.
    float diffuse = clamp(dot(regs.v[24].xyz, n), 0.0, 1.0);
    vec3 light = regs.v[26].xyz * diffuse + regs.v[25].xyz;

    if (kPointLights)
    {
        for (int j = 0; j < 3; j++)
        {
            int b = 28 + 3 * j;
            vec3 lp = regs.v[1].xyz * regs.v[b].yyy;
            lp = regs.v[b].xxx * regs.v[0].xyz + lp;
            lp = regs.v[b].zzz * regs.v[2].xyz + lp;
            lp = -pos.xyz + lp;
            float r = dot(lp, lp);
            r = dx_rsq(r);
            lp = lp * r;
            float dist = 1.0 / r;
            float ndl = clamp(dot(lp, n), 0.0, 1.0);
            vec3 c = ndl * regs.v[b + 1].xyz;
            float att = regs.v[b + 2].y * dist + regs.v[b + 2].x;
            float dist2 = dist * dist;
            att = dist2 * regs.v[b + 2].z + att;
            att = 1.0 / att;
            light = c * att + light;
        }
    }

    light = min(light, vec3(1.0));
    c0 = vec4(light * (kVertexColor ? inColor.xyz : vec3(0.5)), 1.0);
    c1 = spec;
}

// Every varying of a vertex at world position `inPosition` with normal `inNormal` (the light's) and
// colour `inColor`; `groundZ` and `up`: the terrain's own height and how much it faces up there.
// `given` (0..1) blends the light toward `c0Given` / `c1Given`, and with `fogGiven` the fog is
// `fogValue`: a raised chunk's points take the stock Gouraud light and fog of their triangle's corners
// where they lie on bare ground (Terrain.tese).
void TerrainVertexLit(vec4 inPosition, vec4 inNormal, vec4 inColor, float groundZ, float up, float given, vec4 c0Given, vec4 c1Given, bool fogGiven, float fogValue)
{
    // The position in view space, then clip space.
    vec4 pos = TerrainViewPosition(inPosition);
    vec4 clip = pos.yyyy * regs.v[5];
    clip = pos.xxxx * regs.v[4] + clip;
    clip = pos.zzzz * regs.v[6] + clip;
    clip = pos.wwww * regs.v[7] + clip;

    vec3 n = TerrainViewNormal(inNormal);

    vec4 tc[7] = vec4[7](vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));

    // The coordinates: one per varying, or packed two per varying when the cascades need the slots
    // (odd ones in .xy, even ones from 2 in .zw, read back as .wz).
    for (int k = 0; k <= kLayers; k++)
    {
        if (k == 0 && kEnv)
        {
            continue;
        }

        vec2 c = TerrainCoord(k, inPosition.xy);

        if (!kShadow)
        {
            tc[k].xy = c;
        }
        else if (k == 0)
        {
            tc[0].xy = c;
        }
        else if ((k & 1) == 1)
        {
            tc[(k + 1) / 2].xy = c;
        }
        else
        {
            tc[k / 2].zw = c.yx;
        }
    }

    // A reflective layer 0: the reflection vector, back to world orientation.
    if (kEnv)
    {
        float d = dot(pos.xyz, n);
        d = d + d;
        vec3 refl = n * -d + pos.xyz;
        vec3 t = refl.yyy * regs.v[9].xzy;
        t = refl.xxx * regs.v[8].xzy + t;
        tc[0].xyz = refl.zzz * regs.v[10].xzy + t;
    }

    fog0 = fogGiven ? fogValue : TerrainFog(pos);

    // The light: this point's own, blended toward the given one.
    vec4 c0, c1;
    TerrainLight(pos, n, inColor, c0, c1);

    if (given > 0.0)
    {
        c0 = mix(c0, c0Given, given);
        c1 = mix(c1, c1Given, given);
    }

    color0 = c0;
    color1 = c1;

    // The shadow cascades: the view position, and the cascades' coordinates (xyz each, the far
    // cascade's in the three w).
    if (kShadow)
    {
        tc[3] = vec4(pos.xyz, 0.0);
        tc[4] = vec4(dot(pos, regs.v[37]), dot(pos, regs.v[38]), dot(pos, regs.v[39]), dot(pos, regs.v[46]));
        tc[5] = vec4(dot(pos, regs.v[40]), dot(pos, regs.v[41]), dot(pos, regs.v[42]), dot(pos, regs.v[47]));
        tc[6] = vec4(dot(pos, regs.v[43]), dot(pos, regs.v[44]), dot(pos, regs.v[45]), dot(pos, regs.v[48]));
    }

    texcoord0 = tc[0];
    texcoord1 = tc[1];
    texcoord2 = tc[2];
    texcoord3 = tc[3];
    texcoord4 = tc[4];
    texcoord5 = tc[5];
    texcoord6 = tc[6];

    // The covers' extras (the terrain's vertices and normals are in world space: the world matrix
    // moves them by the camera).
    coverWorld = vec4(inPosition.xyz, up);
    coverView = vec4(pos.xyz, 0.0);
    coverNormal = vec4(n, 0.0);
    coverTint = vec4(kVertexColor ? inColor.xyz : vec3(0.5), groundZ);

    // D3D clip space has +y up, Vulkan +y down; z is already in [0, 1].
    gl_Position = vec4(clip.x, -clip.y, clip.z, clip.w);
}

// A vertex with its own light and fog (the flat terrain: Terrain.vert).
void TerrainVertex(vec4 inPosition, vec4 inNormal, vec4 inColor, float groundZ, float up)
{
    TerrainVertexLit(inPosition, inNormal, inColor, groundZ, up, 0.0, vec4(0.0), vec4(0.0), false, 0.0);
}
