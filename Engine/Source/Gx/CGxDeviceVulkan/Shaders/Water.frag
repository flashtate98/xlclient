// SXC: our water's pixel shader (Vulkan/Pipelines: FragmentProgram::Water; SXC/Water draws it), step 1 of
// the water shader (kb: features/render/water-shader): the stock mesh, flat but for small ripples, with
//
//   the tint      the stock one: the client's gradient by depth (s0: proceduralRiverDepthTex,
//                 proceduralOceanDepthTex or proceduralWmoWaterTex, rebuilt from the light data's shallow and
//                 deep water colour and alpha), times a WMO interior's liquid colour
//   reflection    the sky's colours (p7 zenith, p8 horizon, from the light data) by Fresnel
//   the sun       a highlight from c33 (its direction in view space) and c36 (its specular colour)
//   our lights    set 3: on the water's body, and reflected on its surface
//   the fog       the device's (Push.glsl)
//
// The registers SXC/Water adds: p6 = (time in s, wrapping every 600 s; 0; ripple strength; 0), p7, p8.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"
#include "TerrainRegs.glsl"
#include "Lights.glsl"

layout(location = 0) in vec4 vView;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vTint;

layout(location = 0) out vec4 oC0;

layout(set = 0, binding = 0) uniform sampler2D s0;

const float kTau = 6.2831853;

// Travelling sines over the world xy (yd), their slopes summed: the surface's small movement until the FFT
// waves come (step 3). Four long ones and four short ones (the short ones break the sun's glint into
// sparkle; without them it lay in soft blobs). Each frequency is a whole number of turns per 600 s, so
// p6.x's wrap doesn't show. The short ones fade out sooner with distance (no shimmer).
vec2 RippleSlope(vec2 xy, float t, float distance)
{
    const vec2 dirs[8] = vec2[8](vec2(0.8, 0.6), vec2(-0.406, 0.914), vec2(0.0995, -0.995), vec2(-0.949, -0.316),
                                 vec2(0.574, -0.819), vec2(0.985, 0.174), vec2(-0.643, -0.766), vec2(-0.259, 0.966));
    const float lengths[8] = float[8](7.0, 4.3, 2.9, 1.7, 1.1, 0.83, 0.61, 0.47);              // yd
    const float turns[8] = float[8](170.0, 223.0, 271.0, 359.0, 443.0, 509.0, 593.0, 677.0); // per 600 s
    vec2 slope = vec2(0.0);

    for (int i = 0; i < 8; i++)
    {
        float k = kTau / lengths[i];
        float reach = i < 4 ? 160.0 : 40.0;
        float fade = clamp(1.0 - distance / reach, 0.0, 1.0);
        float phase = dot(dirs[i], xy) * k - t * turns[i] * (kTau / 600.0);
        // Amplitude in proportion to the length: the same steepness at every scale.
        slope += dirs[i] * (k * lengths[i] * (i < 4 ? 0.012 : 0.008) * fade * cos(phase));
    }

    return slope;
}

// Our lights reflected on the surface: each light of the point's cell, as LightsAt finds them (the same
// sides, window, cone and shadow), through a tight highlight around the mirror direction.
vec3 LampsReflected(vec3 p, vec3 n, vec3 v)
{
    vec3 sum = vec3(0.0);

    if (!LightsOn())
    {
        return sum;
    }

    int first = LightCellFirst(p.xy);
    int perCell = int(lightInfo.x + 0.5);
    int surface = int(lightSurface.x + 0.5);

    for (int k = 0; k < kMaxLightsPerCell; k++)
    {
        if (k >= perCell)
        {
            break;
        }

        uint index = cellLights[first + k];

        if (index == 0xFFFFFFFFu)
        {
            break;
        }

        Light l = lights[index];
        bool shadowed = l.extra.z > 0.5;

        if (!shadowed && surface != 0 && (l.extra.y > 0.5 ? 2 : 1) != surface)
        {
            continue;
        }

        vec3 toLight = l.posInvRadius.xyz - p;
        float d2 = dot(toLight, toLight);
        float d = sqrt(d2);
        float t = clamp(d * l.posInvRadius.w, 0.0, 1.0);
        float window = (1.0 - t * t) * (1.0 - t * t);

        if (window <= 0.0)
        {
            continue;
        }

        window = pow(window, max(l.colorFalloff.w, 0.01));
        vec3 dir = toLight / max(d, 1e-4);
        float spot = clamp((dot(-dir, l.spotDirCos.xyz) - l.spotDirCos.w) * l.extra.x, 0.0, 1.0);
        float highlight = pow(max(dot(n, normalize(dir + v)), 0.0), 180.0) * 6.0;
        float strength = window * spot * highlight;

        if (shadowed && strength > 0.0)
        {
            strength *= LightShadow(l, p, n, d);
        }

        sum += l.colorFalloff.rgb * strength;
    }

    return sum;
}

void main()
{
    vec4 tint = texture(s0, vTint);
    vec3 world = LightsViewToWorld(vView.xyz);
    vec3 eye = LightsViewToWorld(vec3(0.0));
    vec3 toEye = eye - world;
    float distance = length(toEye);
    vec3 V = toEye / max(distance, 1e-4);
    bool below = V.z < 0.0;

    vec2 slope = RippleSlope(world.xy, regs.p[6].x, distance) * regs.p[6].z;
    vec3 N = normalize(vec3(-slope, 1.0));

    if (below)
    {
        N = -N;
    }

    vec3 body = tint.rgb * vColor.rgb;
    int lampCount;
    vec3 lamps = LightsAt(world, N, lampCount);

    if (LightsDebug() == 1)
    {
        oC0 = vec4(vec3(0.15) + lamps, 1.0);
        return;
    }

    body = LightsCombine(body, lamps * 0.35);

    float cosV = clamp(dot(N, V), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - cosV, 5.0);
    vec3 R = reflect(-V, N);
    // From below, past the critical angle, the surface mirrors the water itself.
    vec3 reflected = below ? body : mix(regs.p[8].rgb, regs.p[7].rgb, sqrt(clamp(R.z, 0.0, 1.0)));

    vec3 toSun = normalize(LightsViewDirToWorld(-regs.v[33].xyz));
    float sun = below ? 0.0 : pow(max(dot(R, toSun), 0.0), 600.0) * smoothstep(-0.05, 0.1, toSun.z);
    vec3 highlight = regs.v[36].rgb * sun * 1.5 + (below ? vec3(0.0) : LampsReflected(world, N, V));
    float glow = max(highlight.r, max(highlight.g, highlight.b));

    vec3 colour = mix(body, reflected, fresnel) + highlight;
    float alpha = clamp(mix(tint.a, 1.0, fresnel) + glow, 0.0, 1.0);

    oC0 = vec4(ApplyFog(colour, FogFactor(vView.w)), alpha);
}
