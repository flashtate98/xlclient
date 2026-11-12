// SXC: the client's projected textures on the raised covers (step 5e): selection and spell target
// circles, blob shadows (fixed function: coordinates from the position through the texgen and texture
// matrices) and models' projected batches (Consecration: from the view position with their vertex
// constants c6..c9). The stock draw lays them on the terrain's flat triangles, under raised cover; this
// one projects them onto what the terrain drew this frame, so they follow the cover, its trails and
// its filling in exactly.
//
// Per pixel of the decal's box: the terrain's view depth there (TerrainCover.glsl's sCoverDepth; nothing
// where no terrain was drawn or its cover isn't raised: the stock decal shows there), the view-space
// point at that depth along the box's ray (the depths compared as clip w: no projection terms), the two
// stages' coordinates from it (pixel rows 0..3: u and v of stage 0, then of stage 1, as columns over (x,
// y, z, 1)), D3D9's stages 0 and 1 over the decal's colour (row 4; the ops from the push constants), the
// alpha test, the fixed-function fog. The pixel takes the terrain point's depth (row 5: the projection's
// z column; row 6: the viewport's depth range, the debug view), so whatever stands in front of the
// terrain still hides the decal.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"
#include "Stage.glsl"
#include "AlphaTest.glsl"
#include "TerrainRegs.glsl"

layout(location = 0) in vec4 vView;

layout(location = 0) out vec4 oC0;

layout(set = 0, binding = 0) uniform sampler2D s0;
layout(set = 0, binding = 1) uniform sampler2D s1;
layout(set = 2, binding = 6, r32ui) uniform readonly uimage2D sCoverDepth;

void main()
{
    uint terrain = imageLoad(sCoverDepth, ivec2(gl_FragCoord.xy)).x;
    float terrainW = uintBitsToFloat(terrain & ~1u);
    vec4 at = vec4(vView.xyz * (terrainW / max(vView.w, 1e-6)), 1.0);
    vec2 uv0 = vec2(dot(at, regs.p[0]), dot(at, regs.p[1]));
    vec2 uv1 = vec2(dot(at, regs.p[2]), dot(at, regs.p[3]));
    // The gradients before any pixel leaves (they're taken across neighbours).
    vec2 dx0 = dFdx(uv0), dy0 = dFdy(uv0), dx1 = dFdx(uv1), dy1 = dFdy(uv1);

    // The covers' debug view 7 (row 6.z): the box solid, in front of everything: red no terrain here,
    // yellow terrain without raised cover, the stage 0 coordinates as green / cyan inside the decal's
    // texture, blue outside it.
    if (regs.p[6].z > 0.5)
    {
        bool inside = all(greaterThanEqual(uv0, vec2(0.0))) && all(lessThanEqual(uv0, vec2(1.0)));
        oC0 = terrain == 0xFFFFFFFFu ? vec4(1.0, 0.0, 0.0, 1.0)
            : (terrain & 1u) == 0u   ? vec4(1.0, 1.0, 0.0, 1.0)
            : inside                 ? vec4(0.0, 0.5 + 0.5 * uv0.x, uv0.y, 1.0)
                                     : vec4(0.0, 0.0, 1.0, 1.0);
        gl_FragDepth = 0.0;
        return;
    }

    if (terrain == 0xFFFFFFFFu || (terrain & 1u) == 0u)
    {
        discard;
    }

    vec4 cur = Stages(clamp(regs.p[4], 0.0, 1.0), textureGrad(s0, uv0, dx0, dy0), textureGrad(s1, uv1, dx1, dy1));
    AlphaTest(cur.a);
    cur.rgb = ApplyFog(cur.rgb, FogFactor(terrainW));
    // The terrain point's depth through this draw's projection and depth range, a hair nearer (the test
    // against the terrain's own: equal but for rounding).
    float ndc = dot(at, regs.p[5]) / terrainW;
    gl_FragDepth = clamp(mix(regs.p[6].x, regs.p[6].y, ndc) - 2e-6, 0.0, 1.0);
    oC0 = cur;
}
