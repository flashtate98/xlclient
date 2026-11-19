#version 450
#extension GL_GOOGLE_include_directive : require
// SXC: our WMO pixel shader (Vulkan/Pipelines: FragmentProgram::Wmo; kb: our-shaders), in place of the
// translated stock and Cataclysm WMO pixel records. The material per shader id is retail's (the Mac
// builds' GLSL, Shaders\Pixel\glfs_420\MapObj*.bls): the diffuse = the lit colour x 2 x the material, times
// the sun shadow's 0.7..1, plus the id's added term (environment, emissive). The registers it reads:
//   p2        the fog colour, w the alpha test's reference (0: none)
//   p3        the cascades' cut-off plane (view space)        p4  the sun's direction (view space)
//   p5..p12   the shadow taps' offsets
// Samplers: s0..s2 the material's textures (s2 Cataclysm's third), s4..s7 the shadow cascades.
// Not yet: the sun's highlight of Specular, Metal and TwoLayerTerrain (1, 2, 8), retail's distance fade of
// the environment.

layout(constant_id = 0) const int  kShader     = 0;
layout(constant_id = 1) const bool kLit        = false;
layout(constant_id = 2) const int  kShadowMode = 0;
layout(constant_id = 5) const bool kAlphaKey   = false; // blend mode 1: cut where texture 0's alpha < 128/255

layout(location = 0) in vec4 inColor;
layout(location = 1) in float inBlend;
layout(location = 2) in vec2 inTex0;
layout(location = 3) in vec2 inTex1;
layout(location = 4) in vec2 inTex2;
layout(location = 5) in vec3 inView;
layout(location = 6) in vec3 inNormal;
layout(location = 7) in vec4 inShadow0;
layout(location = 8) in vec4 inShadow1;
layout(location = 9) in vec4 inShadow2;
layout(location = 10) in float inFog;
layout(location = 11) in vec2 inTex3;     // 23: the fourth UV set
layout(location = 12) in vec4 inWeights;  // 23: MOC2, bytes in file order

layout(location = 0) out vec4 oC0;

layout(set = 0, binding = 0) uniform sampler2D s0;
layout(set = 0, binding = 1) uniform sampler2D s1;
layout(set = 0, binding = 2) uniform sampler2D s2;
layout(set = 0, binding = 4) uniform sampler2D s4;
layout(set = 0, binding = 5) uniform sampler2D s5;
layout(set = 0, binding = 6) uniform sampler2D s6;
layout(set = 0, binding = 7) uniform sampler2D s7;
// Retail's 22 and 23: the material's further textures (color_3, flags_3, runTimeData[0..3]; MapObjCata::Apply).
layout(set = 0, binding = 8) uniform sampler2D s8;
layout(set = 0, binding = 9) uniform sampler2D s9;
layout(set = 0, binding = 10) uniform sampler2D s10;
layout(set = 0, binding = 11) uniform sampler2D s11;
layout(set = 0, binding = 12) uniform sampler2D s12;
layout(set = 0, binding = 13) uniform sampler2D s13;

layout(std140, set = 1, binding = 0) uniform Regs {
    vec4 p[16];
    vec4 misc;
    vec4 v[64];
} regs;

#include "Lights.glsl"
#include "Push.glsl"

// Parallax (22): the view in the surface's tangent space, as retail's MapObjParallax vertex shader computes it
// from its generated tangents (out_viewTS = the view position x (tangent, bitangent, normal)); here from the
// pixel's derivatives (a cotangent frame along +u and +v), exact on the flat windows it draws.
vec3 ViewInTangentSpace(vec3 n, vec2 uv)
{
    vec3 dp1 = dFdx(inView), dp2 = dFdy(inView);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    t = dot(t, t) > 0.0 ? normalize(t) : vec3(1.0, 0.0, 0.0);
    b = dot(b, b) > 0.0 ? normalize(b) : vec3(0.0, 1.0, 0.0);
    return vec3(dot(inView, t), dot(inView, b), dot(inView, n));
}

// One cascade's lit share: the centre and the taps (p5, p7, p9, p11; or p5..p12 with nine), each 1 where
// the map's depth is at or beyond the pixel's, summed (MapObjDiffuse's shadow permutations).
float Taps(sampler2D map, vec2 coord, float depth, bool nine)
{
    vec2 uv = coord * 0.5 + 0.5;
    float sum = textureLod(map, uv, 0.0).x - depth >= 0.0 ? 1.0 : 0.0;

    for (int i = 5; i <= 12; i++)
    {
        if (!nine && (i & 1) == 0)
        {
            continue;
        }

        sum += textureLod(map, uv + regs.p[i].xy, 0.0).x - depth >= 0.0 ? 1.0 : 0.0;
    }

    return sum;
}

// The stock sun shadow, 0 shadowed .. 1 lit (MapObjDiffuse:1..3).
float SunShadow()
{
    if (kShadowMode == 0)
    {
        return 1.0;
    }

    // The first cascade fading out toward its edge; nine taps close to the camera (or always in mode 3).
    float fade = clamp(max(abs(inShadow0.x), abs(inShadow0.y)) * -3.44828 + 3.41379, 0.0, 1.0);
    float first = 1.0;

    if (fade > 0.01)
    {
        bool nine = kShadowMode == 3 || inView.z <= 10.0;
        float lit = nine ? Taps(s4, inShadow0.xy, inShadow0.z, true) * 0.111111 : Taps(s4, inShadow0.xy, inShadow0.z, false) * 0.2;
        first = min(fade * (lit - 1.0) + 1.0, 1.0);
    }

    // Lit beyond the cut-off plane.
    first = mix(first, 1.0, clamp(dot(inView, regs.p[3].xyz) + regs.p[3].w, 0.0, 1.0));
    float shadow = first;

    if (kShadowMode >= 2)
    {
        // The next cascade that holds the pixel, the far one fading out toward its edge; the darker counts.
        float rest;

        if (max(abs(inShadow1.x), abs(inShadow1.y)) < 1.0)
        {
            rest = Taps(s5, inShadow1.xy, inShadow1.z, false) * 0.2;
        }
        else if (max(abs(inShadow2.x), abs(inShadow2.y)) < 1.0)
        {
            rest = Taps(s6, inShadow2.xy, inShadow2.z, false) * 0.2;
        }
        else
        {
            vec2 far = vec2(inShadow0.w, inShadow1.w);
            float farFade = clamp(max(abs(far.x), abs(far.y)) * -11.1111 + 11.0, 0.0, 1.0);
            rest = farFade * (Taps(s7, far, inShadow2.w, false) * 0.2 - 1.0) + 1.0;
        }

        shadow = min(rest, first);
    }

    // None where the surface faces along the sun's rays edge-on (the stock's fade by N.L).
    float facing = 1.2 - abs(dot(regs.p[4].xyz, inNormal));
    facing = clamp(facing * facing * facing * facing, 0.0, 1.0);
    return mix(shadow, 1.0, facing);
}

void main()
{
    // The light first (its derivatives want uniform control flow, before any discard): the lit colour with
    // our lamps (Lights.glsl).
    vec4 light = LightsModelColor(inColor, vec4(inView, 1.0), vec4(inNormal, 1.0));
    vec4 t0 = texture(s0, inTex0);
    vec3 material = t0.rgb;
    vec3 added = vec3(0.0);
    float alpha = inColor.a * t0.a;
    float blend = inBlend;

    // The material per retail shader id (retail's MapObj* pixel shaders).
    if (kShader == 1 || kShader == 2 || kShader == 4)
    {
        // Specular, Metal, Opaque: the stock takes the vertex's alpha alone (the texture's is the shine mask).
        alpha = inColor.a;
    }

    if (kShader == 3)
    {
        added = texture(s1, inTex1).rgb * t0.a;                                   // Env
        alpha = inColor.a;
    }
    else if (kShader == 5)
    {
        added = t0.rgb * t0.a * texture(s1, inTex1).rgb;                         // EnvMetal
        alpha = inColor.a;
    }
    else if (kShader == 6)
    {
        vec4 t1 = texture(s1, inTex1);                                           // TwoLayerDiffuse
        vec3 second = mix(t0.rgb, t1.rgb, t1.a);
        material = mix(second, t0.rgb, blend);
    }
    else if (kShader == 7)
    {
        vec4 layered = mix(texture(s1, inTex1), t0, blend);                     // TwoLayerEnvMetal
        material = layered.rgb;
        added = layered.rgb * layered.a * texture(s2, inTex2).rgb;
        alpha = inColor.a;
    }
    else if (kShader == 8)
    {
        material = mix(texture(s1, inTex1).rgb, t0.rgb, blend);                 // TwoLayerTerrain
    }
    else if (kShader == 9)
    {
        vec4 t1 = texture(s1, inTex1);                                           // DiffuseEmissive
        added = t1.rgb * t1.a * blend;
    }
    else if (kShader == 11)
    {
        vec4 t2 = texture(s2, inTex2);                                           // MaskedEnvMetal
        vec3 env = texture(s1, inTex1).rgb;
        material = mix(mix(t0.rgb * env * 2.0, t2.rgb, clamp(t2.a * blend, 0.0, 1.0)), t0.rgb, t0.a);
        alpha = inColor.a;
    }
    else if (kShader == 12)
    {
        vec4 t2 = texture(s2, inTex2);                                           // EnvMetalEmissive
        added = t0.rgb * t0.a * texture(s1, inTex1).rgb + t2.rgb * t2.a * blend;
        alpha = inColor.a;
    }
    else if (kShader == 13)
    {
        material = mix(texture(s1, inTex1).rgb, t0.rgb, blend);                 // TwoLayerDiffuseOpaque
        alpha = inColor.a;
    }
    else if (kShader == 15)
    {
        vec4 t1 = texture(s1, inTex1);                                           // TwoLayerDiffuseEmissive:
        material = mix(t1.rgb * (1.0 - t1.a), t0.rgb, blend);                   // the second layer's alpha
        added = t1.rgb * t1.a * (1.0 - blend);                                   // glows where it shows
    }
    else if (kShader == 18)
    {
        // TwoLayerDiffuseMod2x: the second layer over the first by its alpha, the first back by the blend,
        // times twice the third texture (through the third UV set).
        vec4 t1 = texture(s1, inTex1);
        vec4 t2 = texture(s2, inTex2);
        vec3 layered = mix(mix(t0.rgb, t1.rgb, t1.a), t0.rgb, blend);
        material = layered * t2.rgb * 2.0;
    }
    else if (kShader == 20)
    {
        // TwoLayerDiffuseAlpha: as 18, but the third texture's alpha brings the first layer back.
        vec4 t1 = texture(s1, inTex1);
        vec4 t2 = texture(s2, inTex2);
        vec3 layered = mix(mix(t0.rgb, t1.rgb, t1.a), t0.rgb, t2.a);
        material = layered * t2.rgb * 2.0;
    }
    else if (kShader == 19)
    {
        material = mix(t0.rgb, t0.rgb * texture(s1, inTex1).rgb * 2.0, blend);  // TwoLayerDiffuseMod2xNA
    }
    else if (kShader == 22)
    {
        // The goblin city's windows: OUR RECONSTRUCTION. Retail draws them with a 22 that takes six textures;
        // its program isn't in any file we can name (retail's MapObjParallax binds three; kb: our-shaders).
        // From the material's textures and retail's parallax: the glass (texture_1, its alpha how much it
        // hides) with the shine reflected in it (texture_2, sphere-mapped); behind it the curtains (flags_3's
        // fabric where color_3's alpha says, at the first depth) and the lit room between them (runTimeData[0]
        // at the second depth). Depths, scales, the mask's sense and the glow from the push block (SXC::Wmo::
        // ParallaxParams).
        vec3 viewTS = ViewInTangentSpace(normalize(inNormal), inTex0);
        vec2 shift = viewTS.xy / (abs(viewTS.z) < 1.0e-4 ? -1.0e-4 : viewTS.z);
        vec4 glass = texture(s0, inTex0 * pc.xform[1].xy);
        int glassMode = int(pc.xform[1].z + 0.5);
        vec2 curtainUv = (inTex0 - shift * pc.xform[0].x) * pc.xform[2].xy + pc.xform[2].zw;
        float curtain = texture(s8, curtainUv).a;
        curtain = pc.xform[0].z > 0.5 ? 1.0 - curtain : curtain;
        vec3 fabric = texture(s9, curtainUv).rgb;
        vec3 room = texture(s10, (inTex0 - shift * pc.xform[0].y) * pc.xform[3].xy + pc.xform[3].zw).rgb;
        // How the glass sits over what's behind it (Wmo/ParallaxGlassMode, retail's unknown): 0 its alpha covers
        // (retail's parallax), 1 its alpha is how much shows through, 2 the glass added over all of it.
        float cover = glassMode == 0 ? glass.a : glassMode == 1 ? 1.0 - glass.a : 0.0;
        float through = glassMode == 2 ? 1.0 : 1.0 - cover;
        float glowShare = clamp(pc.xform[0].w, 0.0, 1.0);
        vec3 roomSeen = room * (1.0 - curtain) * through;
        material = (glassMode == 2 ? glass.rgb : glass.rgb * cover) + fabric * curtain * through + roomSeen * (1.0 - glowShare);
        added = roomSeen * pc.xform[0].w + texture(s1, inTex1).rgb * (glassMode == 2 ? glass.a : cover);
        alpha = inColor.a;
    }
    else if (kShader == 23)
    {
        // Dragonflight's 23: OUR RECONSTRUCTION, after wow.export's MapObjDFShader (retail's program isn't in
        // any file we can name; kb: our-shaders). Four layers (texture_2, texture_3, color_3, flags_3 on the
        // four UV sets) weighted by MOC2 (bytes 2, 1, 0; the fourth takes the rest) times each layer's height
        // (the alpha of runTimeData[0..3], its _s map), blended as retail's terrain: the highest wins with a
        // soft edge. MOC2's alpha darkens. texture_1 is a stand-in for older clients and isn't read.
        // The layers and heights the material has (MapObjCata::LayerMask in pc.xform[0].z): an empty layer gets no
        // weight (its stage would read the device's white default), a missing height counts as neutral (1).
        uint layers = uint(pc.xform[0].z + 0.5);
        vec4 has = vec4((layers & 1u) != 0u, (layers & 2u) != 0u, (layers & 4u) != 0u, (layers & 8u) != 0u);
        vec4 hasHeight = vec4((layers & 16u) != 0u, (layers & 32u) != 0u, (layers & 64u) != 0u, (layers & 128u) != 0u);
        vec4 l0 = texture(s1, inTex0), l1 = texture(s2, inTex1), l2 = texture(s8, inTex2), l3 = texture(s9, inTex3);
        vec4 h = vec4(texture(s10, inTex0).a, texture(s11, inTex1).a, texture(s12, inTex2).a, texture(s13, inTex3).a);
        h = max(mix(vec4(1.0), h, hasHeight), 0.004);
        vec4 w = vec4(inWeights.zyx, 1.0 - clamp(dot(inWeights.zyx, vec3(1.0)), 0.0, 1.0)) * has;
        w = dot(w, vec4(1.0)) > 0.0 ? w : has;
        vec4 a = w * h;
        float top = max(max(a.x, a.y), max(a.z, a.w));
        vec4 a2 = (1.0 - clamp(vec4(top) - a, 0.0, 1.0)) * a;
        a2 /= max(dot(a2, vec4(1.0)), 1.0e-5);
        vec4 mixed = l0 * a2.x + l1 * a2.y + l2 * a2.z + l3 * a2.w;
        material = mix(mixed.rgb, vec3(0.0), inWeights.w);
        alpha = inColor.a;
    }

    // The alpha test. An alpha-key material (blend mode 1) as retail cuts it: texture 0's alpha below 128/255,
    // whatever the vertex alpha (MapObjTwoLayerDiffuseMod2xNA and the others' alpha-tested programs); neither
    // the stock nor Cataclysm's records cut the two-layer ones (Kul Tiras' hanging roots, 2026-11-19). Else
    // the stock's alpha-test reference in p2.w.
    if (kAlphaKey ? t0.a < 0.501960814 : (regs.p[2].w > 0.0 && alpha < regs.p[2].w))
    {
        discard;
    }

    // Times the sun shadow's 0.7..1.
    float sunShadow = SunShadow();
    float shade = sunShadow * 0.3 + 0.7;

    // Retail's sun highlight on Specular, Metal and TwoLayerTerrain (3.3.5 computed one in the vertex and
    // never used it): 1.25 (N.H)^8, a Fresnel term toward 1 by the texture's mask (TwoLayerTerrain: the
    // terrain texture's alpha where it shows), the direct light (v11: none indoors) where the surface faces
    // the sun, the shadow's share.
    if (kShader == 1 || kShader == 2 || kShader == 8)
    {
        vec3 n = normalize(inNormal);
        vec3 l = -normalize(regs.v[12].xyz);
        vec3 h = normalize(l + normalize(-inView));
        float spec = 1.25 * pow(clamp(dot(n, h), 0.0, 1.0), 8.0);
        float mask = kShader == 1 ? t0.a : kShader == 2 ? t0.r * 4.0 * t0.a : texture(s1, inTex1).a * (1.0 - blend);
        float fresnel = mix(pow(1.0 - clamp(dot(l, h), 0.0, 1.0), 5.0), 1.0, mask);
        added += fresnel * spec * regs.v[11].xyz * clamp(dot(n, l), 0.0, 1.0) * shade;
    }

    vec3 colour = light.rgb * shade * 2.0 * material + added;

    // The stock fog: toward the fog colour as the factor falls.
    colour = mix(regs.p[2].rgb, colour, inFog);
    oC0 = vec4(colour, alpha);
}
