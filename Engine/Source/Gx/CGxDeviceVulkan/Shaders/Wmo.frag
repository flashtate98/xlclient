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

layout(location = 0) out vec4 oC0;

layout(set = 0, binding = 0) uniform sampler2D s0;
layout(set = 0, binding = 1) uniform sampler2D s1;
layout(set = 0, binding = 2) uniform sampler2D s2;
layout(set = 0, binding = 4) uniform sampler2D s4;
layout(set = 0, binding = 5) uniform sampler2D s5;
layout(set = 0, binding = 6) uniform sampler2D s6;
layout(set = 0, binding = 7) uniform sampler2D s7;

layout(std140, set = 1, binding = 0) uniform Regs {
    vec4 p[16];
    vec4 misc;
    vec4 v[64];
} regs;

#include "Lights.glsl"

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

    // The alpha test (the stock's alpha-test permutations: the reference in p2.w).
    if (regs.p[2].w > 0.0 && alpha < regs.p[2].w)
    {
        discard;
    }

    // Times the sun shadow's 0.7..1.
    float shade = SunShadow() * 0.3 + 0.7;
    vec3 colour = light.rgb * shade * 2.0 * material + added;

    // The stock fog: toward the fog colour as the factor falls.
    colour = mix(regs.p[2].rgb, colour, inFog);
    oC0 = vec4(colour, alpha);
}
