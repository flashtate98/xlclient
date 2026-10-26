// SXC: the volumetric fog over the scene: the pixel's distance from its depth, then the fog up to it --
// marched along the pixel's own ray (thin layers stay sharp), or read from the froxel volume (trilinear)
// -- blended as dst x transmittance + scattered light (blend ONE, SRC_ALPHA).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Fog.glsl"

layout(set = 0, binding = 4) uniform sampler2D sceneDepth;
layout(set = 0, binding = 5) uniform sampler3D volume;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 oC0;

// Interleaved gradient noise: a per-pixel offset 0..1 for the march's steps, so what banding is left
// becomes fine grain instead of rings.
float Dither(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// The fog along the pixel's ray from the camera to `distance`: rgb scattered light, a transmittance.
// Steps of fp.colour.a yd near the camera, growing with the distance (fp.march.y of it), at most
// fp.march.z of them; each step's medium averaged exactly (Fog.glsl). Past the march's reach, up to the
// terrain drawn there (`surface`), one long step through the far map (WXL's): flying, the fog then
// covers the ground below instead of ending in mid-air.
vec4 March(vec3 dir, vec2 uv, float distance, float surface, float jitter)
{
    vec3 scattered = vec3(0.0);
    float transmittance = 1.0;
    float t = 0.0;
    vec3 a = fp.origin.xyz;
    Field fa = SampleField(a.xy);
    int most = int(fp.march.z);

    for (int i = 0; i < most && t < distance && transmittance > 0.004; i++)
    {
        float len = max(fp.colour.a, t * fp.march.y);

        // The first step is shortened by the pixel's offset: every later step boundary moves with it.
        if (i == 0)
        {
            len *= 0.25 + 0.75 * jitter;
        }

        float next = min(t + len, distance);
        // The last allowed step reaches the end, however long.
        if (i == most - 1)
        {
            next = distance;
        }

        vec3 b = fp.origin.xyz + dir * next;
        Field fb = SampleField(b.xy);
        vec4 medium = StepMedium(a, b, fa, fb, false);
        float sigma = medium.a;
        float step = exp(-sigma * (next - t));
        // The fog's own colour, and our lights where they reach (only where there is fog to scatter it).
        vec3 colour = sigma > 0.0 ? medium.rgb / sigma + FogLightIn(uv, 0.5 * (t + next)) : fp.colour.rgb;
        scattered += transmittance * (1.0 - step) * colour;
        transmittance *= step;
        a = b;
        fa = fb;
        t = next;
    }

    if (fp.maps.x > 0.5 && t >= distance && surface > distance + 1.0 && transmittance > 0.004)
    {
        vec3 b = fp.origin.xyz + dir * surface;
        Field fb = SampleField(b.xy);
        vec4 medium = StepMedium(a, b, fa, fb, true);
        float sigma = medium.a;
        float step = exp(-sigma * (surface - distance));
        vec3 colour = sigma > 0.0 ? medium.rgb / sigma : fp.colour.rgb;
        scattered += transmittance * (1.0 - step) * colour;
        transmittance *= step;
    }

    return vec4(scattered, transmittance);
}

void main()
{
    vec2 pixel = gl_FragCoord.xy;
    vec2 uv = (pixel - fp.viewport.xy) / fp.viewport.zw;
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float d = texelFetch(sceneDepth, ivec2(pixel), 0).r;

    // The sky (nothing drawn, or the far plane): the fog's own far edge.
    float distance = fp.depth.w;
    float surface = 0.0; // the terrain drawn at the pixel (0: the sky)
    vec3 dir;

    if (d < fp.depth.y * 0.99999)
    {
        float z = (d - fp.depth.x) / max(fp.depth.y - fp.depth.x, 1e-6);
        vec3 p = Unproject(ndc, z);
        surface = length(p);
        distance = min(surface, fp.depth.w);
        dir = normalize(p);
    }
    else
    {
        dir = normalize(Unproject(ndc, 1.0) - Unproject(ndc, 0.0));
    }

    vec4 fog;

    if (fp.march.x > 0.5)
    {
        fog = March(dir, uv, distance, surface, Dither(pixel));
    }
    else
    {
        // Texel k holds the fog up to slice edge k + 1; closer than the first edge, a share of it.
        float s = SliceOf(distance);
        float first = 1.0;

        if (s < 1.0)
        {
            first = distance / SliceEdge(1.0);
            s = 1.0;
        }

        fog = textureLod(volume, vec3(uv, (s - 0.5) / float(kSlices)), 0.0);
        fog.rgb *= first;
        fog.a = mix(1.0, fog.a, first);
    }

    if (fp.misc.y > 0.5 && fp.misc.y < 1.5)
    {
        fog.rgb = vec3(1.0, 0.0, 1.0) * (1.0 - fog.a);
    }

    oC0 = vec4(fog.rgb, fog.a);
}
