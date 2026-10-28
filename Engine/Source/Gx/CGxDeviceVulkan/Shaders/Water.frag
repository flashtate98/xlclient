// SXC: our water's pixel shader (Vulkan/Pipelines: FragmentProgram::Water; SXC/Water draws it), steps 1 and
// 2 of the water shader (kb: features/render/water-shader): the stock mesh, flat but for small ripples, with
//
//   the scene      what lies behind the water (set 2, Vulkan/Water: copied at the frame's first water draw),
//                  seen through it bent by the ripples and absorbed by the water the view crosses: red goes
//                  first, so shallows stay clear and the deep turns to the water's colour
//   the tint       the stock one, the water's own colour: the client's gradient by depth (s0:
//                  proceduralRiverDepthTex, proceduralOceanDepthTex or proceduralWmoWaterTex, from the light
//                  data's shallow and deep water colour and alpha), times a WMO interior's liquid colour; its
//                  alpha sets how murky the water is
//   reflection     screen-space, from the scene's copy; the sky's colours (p7 zenith, p8 horizon) where the
//                  ray leaves the screen or finds nothing; by Fresnel
//   the sun        a highlight from c33 (its direction in view space) and c36 (its specular colour)
//   our lights     set 3: on the water's body, and reflected on its surface
//   the fog        the device's (Push.glsl), on the water's own light (the scene behind is fogged already)
//
// From below: the world above inside Snell's window, the water mirrored outside it.
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

// The scene behind the water (Vulkan/Water.cpp): its colour, its depth as the view's w (1e6: nothing), and
// where the draws lie on the target (viewport x, y, width, height; the target's width, height, 1 / both).
layout(set = 2, binding = 0) uniform sampler2D sceneColour;
layout(set = 2, binding = 1) uniform sampler2D sceneW;
layout(std140, set = 2, binding = 2) uniform WaterScene {
    vec4 viewport;
    vec4 extent;
} scene;

const float kTau = 6.2831853;

// Travelling sines over the world xy (yd), their slopes summed: the surface's small movement until the FFT
// waves come (step 3). Four long ones and four short ones (the short ones break the sun's glint into
// sparkle), all within 45 degrees of one direction like a wind sea (spread wider they crossed into a
// hatched grid). Each frequency is a whole number of turns per 600 s, so p6.x's wrap doesn't show. The short
// ones fade out sooner with distance (no shimmer).
vec2 RippleSlope(vec2 xy, float t, float distance)
{
    const vec2 dirs[8] = vec2[8](vec2(1.0, 0.0), vec2(0.906, 0.423), vec2(0.866, -0.5), vec2(0.707, 0.707),
                                 vec2(0.966, -0.259), vec2(0.819, 0.574), vec2(0.766, -0.643), vec2(0.985, 0.174));
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

// A world direction in the view (the inverse of LightsViewDirToWorld: the rows' transpose).
vec3 WorldDirToView(vec3 w)
{
    return lightViewInv[0].xyz * w.x + lightViewInv[1].xyz * w.y + lightViewInv[2].xyz * w.z;
}

// Where a view-space point lands on the scene's copy, and its clip w.
vec3 ToScene(vec3 view)
{
    vec4 clip = regs.v[0] * view.x + regs.v[1] * view.y + regs.v[2] * view.z + regs.v[3];
    vec2 ndc = clip.xy / max(clip.w, 1e-4);
    vec2 pixel = scene.viewport.xy + vec2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y) * scene.viewport.zw;
    return vec3(pixel * scene.extent.zw, clip.w);
}

// Screen-space reflection: the reflected ray marched through the scene's depth from the surface point; the
// colour where it passes behind something, and how sure that is (0 none: the sky takes over).
vec4 ScreenReflection(vec3 origin, vec3 dir)
{
    // A ray towards the camera finds nothing the copy holds.
    float facing = smoothstep(-0.05, 0.15, dir.z);

    if (facing <= 0.0)
    {
        return vec4(0.0);
    }

    float stride = 0.4;
    float travelled = 0.0;
    vec3 prev = origin;

    for (int i = 0; i < 40; i++)
    {
        travelled += stride;
        vec3 p = origin + dir * travelled;
        vec3 at = ToScene(p);

        if (any(lessThan(at.xy, vec2(0.0))) || any(greaterThan(at.xy, vec2(1.0))) || at.z <= 0.0)
        {
            break;
        }

        float behind = at.z - textureLod(sceneW, at.xy, 0.0).r;

        if (behind > 0.0 && behind < max(1.5, stride * 2.0))
        {
            // Halve back onto the surface it passed.
            vec3 lo = prev, hi = p;

            for (int k = 0; k < 5; k++)
            {
                vec3 mid = 0.5 * (lo + hi);
                vec3 m = ToScene(mid);

                if (m.z - textureLod(sceneW, m.xy, 0.0).r > 0.0)
                {
                    hi = mid;
                }
                else
                {
                    lo = mid;
                }
            }

            vec3 hit = ToScene(hi);
            vec2 edge = min(hit.xy, 1.0 - hit.xy);
            float sure = facing * smoothstep(0.0, 0.08, min(edge.x, edge.y)) * (1.0 - float(i) / 40.0);
            return vec4(textureLod(sceneColour, hit.xy, 0.0).rgb, sure);
        }

        prev = p;
        stride *= 1.15;
    }

    return vec4(0.0);
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

    // The scene behind, bent by the ripples alone (not onto what stands in front of the water). Bent by the
    // whole normal, the flat surface's tilt towards the camera shifted the scene up the screen: a copy of
    // everything a little lower, moving with the camera (Seyri, 2026-10-28).
    vec2 here = gl_FragCoord.xy * scene.extent.zw;
    vec3 tilt = WorldDirToView(N) - WorldDirToView(below ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 0.0, 1.0));
    vec2 bent = here + vec2(tilt.x, -tilt.y) * (0.6 / max(vView.w, 1.0));
    float sceneBent = textureLod(sceneW, bent, 0.0).r;

    if (sceneBent < vView.w)
    {
        bent = here;
        sceneBent = textureLod(sceneW, bent, 0.0).r;
    }

    vec3 behind = textureLod(sceneColour, bent, 0.0).rgb;

    // The water the view crosses: from the surface to what lies behind it (above), or from the camera up to
    // the surface (below). Murkier where the stock alpha is higher; red absorbed first.
    float crossed = below ? distance : max(sceneBent - vView.w, 0.0) * (distance / max(vView.w, 1e-3));
    // (First tuning: 0.025..0.35 per yd was milky on WoW's steep shores, Seyri 2026-10-28.)
    float murk = mix(0.01, 0.12, clamp(tint.a, 0.0, 1.0));
    vec3 absorb = murk * (1.25 - clamp(body, 0.0, 1.0));
    vec3 through = exp(-absorb * crossed);
    // The light the water itself sends back: darker than the stock tint (as a surface colour it is light).
    vec3 scatter = body * 0.65;

    // From below, past the critical angle, the surface mirrors the water. Real water's (cos 0.66) leaves a
    // third-person camera seeing up only when it looks steeply up; this one is wider, for the game.
    const float kWindowCos = 0.3;
    float window = below ? smoothstep(kWindowCos - 0.06, kWindowCos + 0.06, abs(dot(N, V))) : 1.0;
    vec3 seen = mix(scatter, behind * through + scatter * (1.0 - through), window);

    float cosV = clamp(dot(N, V), 0.0, 1.0);
    float fresnel = below ? 1.0 - window : 0.02 + 0.98 * pow(1.0 - cosV, 5.0);
    vec3 R = reflect(-V, N);
    vec3 sky = mix(regs.p[8].rgb, regs.p[7].rgb, sqrt(clamp(R.z, 0.0, 1.0)));
    vec3 reflected = scatter;

    if (!below)
    {
        vec4 ssr = ScreenReflection(vView.xyz, normalize(WorldDirToView(R)));
        reflected = mix(sky, ssr.rgb, ssr.a);
    }

    vec3 toSun = normalize(LightsViewDirToWorld(-regs.v[33].xyz));
    float sun = below ? 0.0 : pow(max(dot(R, toSun), 0.0), 600.0) * smoothstep(-0.05, 0.1, toSun.z);
    vec3 highlight = regs.v[36].rgb * sun * 1.5 + (below ? vec3(0.0) : LampsReflected(world, N, V));

    // The fog on the water's own light; the scene seen through it carries its own fog already.
    vec3 clear = behind * through * window * (1.0 - fresnel);
    vec3 own = mix(seen, reflected, fresnel) + highlight - clear;
    float coverage = 1.0 - max(through.r, max(through.g, through.b)) * window * (1.0 - fresnel);
    float fog = FogFactor(vView.w);
    vec3 fogged = clear + own * fog + ApplyFog(vec3(0.0), 0.0) * (1.0 - fog) * coverage;

    oC0 = vec4(fogged, 1.0);
}
