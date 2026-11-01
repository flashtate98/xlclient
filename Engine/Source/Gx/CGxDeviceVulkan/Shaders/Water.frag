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
// The registers SXC/Water adds: p6 = (time in s, wrapping every 600 s; the debug view; ripple strength; 0), p7
// the sky's zenith, p8 its horizon, p9..p13 the draw's own look, p14 = (screen-space reflections on; the
// sea's foam threshold (Jacobian); 1 when the look comes from the look grid; 1 for terrain water, which reads
// the shore field); not p15, which the device fills with the fixed-function fog. The look itself (its
// LiquidSurface row, blended between places for terrain water) comes from Water.vert: vLook[0] = (clarity:
// absorption per yd at the stock alpha 0 and 1; own colour; refraction), [1] = (reflectivity looking straight
// down; reflection strength; Snell's window (cos); glint), [2] = (shallow colour; Hue weight), [3] = (deep
// colour; Replace weight), [4] = (glint sharpness; wave scale; foam; movement).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"
#include "TerrainRegs.glsl"
#include "Lights.glsl"

layout(location = 0) in vec4 vView;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vTint;
layout(location = 3) in float vEase;
layout(location = 4) in vec4 vLook[5];
layout(location = 9) in float vNear;   // the near mesh: 1 inside its cells (a triangle reaching outside them is dropped)

layout(location = 0) out vec4 oC0;

layout(set = 0, binding = 0) uniform sampler2D s0;

// The scene behind the water (Vulkan/Water.cpp): its colour, its depth as the view's w (1e6: nothing), and
// where the draws lie on the target (viewport x, y, width, height; the target's width, height, 1 / both).
layout(set = 2, binding = 0) uniform sampler2D sceneColour;
layout(set = 2, binding = 1) uniform sampler2D sceneW;
#include "WaterScene.glsl"
#include "WaterShore.glsl"
// The waves (step 3, WaterWaves.glsl): per cascade the height's slopes, the Jacobian (below 1 the crest
// folds), the height.
layout(set = 2, binding = 3) uniform sampler2DArray waveSlopes;
layout(set = 2, binding = 4) uniform sampler2DArray waveDisp;   // per cascade: dx, dy, height (yd): how the surface moves
#include "WaterMove.glsl"

// The waves at a world point: the slopes summed over the cascades (the finer ones fading with distance),
// and how much the crests fold there (0 none .. 1 breaking).
vec2 WaveSlope(vec2 xy, float distance, out float folding)
{
    vec2 slope = vec2(0.0);
    float jacobian = 1.0;

    for (int c = 0; c < 3; c++)
    {
        float fade = c == 0 ? 1.0 : clamp(1.0 - distance / scene.waveLook[c - 1], 0.0, 1.0);

        if (fade <= 0.0)
        {
            continue;
        }

        vec4 w = texture(waveSlopes, vec3(xy / scene.waveSizes[c], float(c)));
        slope += w.xy * fade;
        jacobian += (w.z - 1.0) * fade;
    }

    folding = clamp((regs.p[14].y - jacobian) * 2.5, 0.0, 1.0);
    return slope;
}

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
// How the last search ended (the debug view "Reflection search outcome"): 0 towards the camera, 1 left the
// screen, 2 out of steps, 3 a hit; `thick` whether it passed behind something too deep to count as a hit.
int ssrOutcome = 0;
bool ssrThick = false;
int ssrSteps = 0;

vec4 ScreenReflection(vec3 origin, vec3 dir, float upward)
{
    // A ray towards the camera finds nothing the copy holds.
    float facing = smoothstep(-0.05, 0.15, dir.z);
    ssrOutcome = 0;
    ssrThick = false;

    if (facing <= 0.0)
    {
        return vec4(0.0);
    }

    ssrOutcome = 2;

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
            ssrOutcome = 1;
            break;
        }

        float behind = at.z - textureLod(sceneW, at.xy, 0.0).r;

        ssrThick = ssrThick || behind >= max(1.5, stride * 2.0);

        if (behind > 0.0 && behind < max(1.5, stride * 2.0))
        {
            ssrOutcome = 3;
            ssrSteps = i;
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

    // Nothing in the way: the ray goes to the sky. Where its direction is on screen and the copy shows
    // sky there (nothing drawn: the far plane), that is the sky it sees, clouds, moons and the sun included
    // (the sun's glitter path at sundown). Elsewhere the sky's colours stand in (the caller).
    // Only rays that really rise: the water isn't in the copy, so below the horizon it shows the sky dome
    // (the set sun with it), which rays the waves tip downwards found ("the sun through the ocean", Seyri
    // 2026-10-28).
    vec3 far = ToScene(origin + dir * 4000.0);

    if (upward > 0.02 && far.z > 0.0 && all(greaterThanEqual(far.xy, vec2(0.0))) && all(lessThanEqual(far.xy, vec2(1.0)))
        && textureLod(sceneW, far.xy, 0.0).r > 5e5)
    {
        vec2 edge = min(far.xy, 1.0 - far.xy);
        ssrOutcome = 4;
        return vec4(textureLod(sceneColour, far.xy, 0.0).rgb, facing * smoothstep(0.0, 0.06, min(edge.x, edge.y)));
    }

    return vec4(0.0);
}

// The surface's rise at a world point (its still height in z) as Water.vert moves it (WaterQuery.comp's).
float RiseHere(vec3 p)
{
    ivec2 g = ShoreCellOf(p.xy);
    bool near = NearCovers(p.xy);
    vec4 cell = near ? ShoreCell(g) : vec4(0.0);
    float depthY = near ? max(cell.w - 1.0, 0.0) : 1.0;
    float ease;
    return SurfaceMove(p, near, cell, depthY, LookMoveAt(p, vLook[4]), regs.p[5].xyz, regs.p[6].x, ease).z;
}

void main()
{
    vec3 world = LightsViewToWorld(vView.xyz);

    // The near mesh (v55.x 1) keeps only its own cells' triangles; the stock terrain water (p14.w 2: the near
    // mesh drew this frame) gives way wherever the near mesh covers. First, before any other work: the near
    // mesh spans the whole window.
    if (regs.v[55].x > 0.5 ? vNear < 0.999 : regs.p[14].w > 1.5 && NearCovers(world.xy))
    {
        discard;
    }

    vec4 tint = texture(s0, vTint);

    // The water's colour by its look (shallow and deep, the Hue and Replace weights: one of them 1 for a row,
    // between where places blend): stock, or our colours from the shore to a few yards down, either with the
    // stock brightness (Hue: it still darkens through the day) or as they are (Replace). The alpha (the
    // murk) stays the stock one.
    float hue = vLook[2].w, replace = vLook[3].w;

    if (hue + replace > 0.001)
    {
        const vec3 kLuma = vec3(0.299, 0.587, 0.114);
        vec3 ours = mix(vLook[2].rgb, vLook[3].rgb, clamp(vTint.y, 0.0, 1.0));
        vec3 hued = ours * (dot(tint.rgb, kLuma) / max(dot(ours, kLuma), 1e-3));
        tint.rgb = tint.rgb * max(1.0 - hue - replace, 0.0) + hued * hue + ours * replace;
    }
    vec3 eye = LightsViewToWorld(vec3(0.0));
    vec3 toEye = eye - world;
    float distance = length(toEye);
    vec3 V = toEye / max(distance, 1e-4);
    // Which side of the surface the camera sees this point from. The surface is a height field, so the side
    // its ray starts on: the pixel's point on the near plane against the surface there, as the water moves
    // it (WaterMove.glsl; the underwater medium decides the same way). Not the camera's height against the
    // point's (with the camera at the surface every crest above its eye was shaded as the underside, cut
    // level through the waves), nor the drawn triangle's side (noisy at grazing angles: stripes; Seyri
    // 2026-11-01).
    bool below;
    {
        float zNear = -regs.v[3].z / regs.v[2].z;
        vec3 nearWorld = LightsViewToWorld(vView.xyz * (zNear / vView.z));

        if (regs.p[14].w > 0.5)
        {
            float still = world.z - RiseHere(world);
            below = nearWorld.z < still + RiseHere(nearWorld);
        }
        else
        {
            below = nearWorld.z < world.z;   // WMO water doesn't move
        }
    }
    float pixel = length(fwidth(world.xy));   // yd per pixel on the surface (the foam's detail fades by it)

    // The FFT waves when the device runs them, else the small ripples.
    float folding = 0.0;
    // This water's share of the shared sea (its look's wave scale).
    vec2 slope = (scene.waveSizes.w > 0.5 ? WaveSlope(world.xy, distance, folding) : RippleSlope(world.xy, regs.p[6].x, distance) * regs.p[6].z) * vLook[4].y;

    // The shore (the shore field, terrain water near the player): the open sea calms over the shallows, and
    // shore waves roll in toward the beach, steepening as they come, breaking into whitewater that runs on
    // to the waterline. One speed for every pixel (a speed by depth would shear the bands as the clock
    // runs), a whole number of turns per 600 s (p6.x's wrap); bent a little by noise so the bands aren't
    // drawn with a ruler.
    //
    // Exposure: waves come from the open sea, downwind. A shore gets them by how much it faces into the
    // waves' direction and how much open water lies upwind of it (the field read 20 and 50 yd upwind):
    // the lee of an island and a sheltered bay stay calm (Seyri, 2026-10-30: islands were ringed). A
    // stronger wind, higher waves.
    float shoreWeight;
    vec4 shore = ShoreAt(world.xy, shoreWeight);
    float shoreDist = shore.x, shoreDepth = shore.y;
    float shoreCrest = 0.0;
    slope *= mix(1.0, mix(0.35, 1.0, smoothstep(0.5, 5.0, shoreDepth)), shoreWeight);
    float shoreK = ShoreK();
    float shoreOmega = ShoreOmega();
    float arrival = ShoreArrival(world.xy, regs.p[6].x);
    float shoreExposure = shoreWeight > 0.0 ? ShoreExposure(world.xy, shore) : 1.0;

    if (shoreWeight > 0.0 && shoreDist > 0.0)
    {
        float phase = shoreK * shoreDist + arrival;
        // Taller toward the shore until they break about a yard out; steeper (a narrower crest) as they go.
        float amp = scene.shoreLook.y * vLook[4].y * shoreExposure * smoothstep(0.3, 1.5, shoreDist)
                  * mix(1.4, 1.0, smoothstep(1.0, 10.0, shoreDist))
                  * (1.0 - smoothstep(scene.shoreLook.w * 0.4, max(scene.shoreLook.w, 1.0), shoreDist));
        float sharp = mix(4.0, 1.5, smoothstep(1.0, 12.0, shoreDist));
        float rise = 0.5 + 0.5 * sin(phase);
        // h = amp * rise^sharp along the distance; the distance grows away from the shore (-toShore).
        float dh = amp * sharp * pow(rise, sharp - 1.0) * 0.5 * cos(phase) * shoreK;
        slope -= shore.zw * dh * shoreWeight;
        shoreCrest = pow(rise, 6.0) * smoothstep(0.02, 0.1, amp);
    }
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
    vec2 bent = here + vec2(tilt.x, -tilt.y) * (vLook[0].w / max(vView.w, 1.0));
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
    float murk = mix(vLook[0].x, vLook[0].y, clamp(tint.a, 0.0, 1.0));
    vec3 absorb = murk * (1.25 - clamp(body, 0.0, 1.0));
    vec3 through = exp(-absorb * crossed);
    // The light the water itself sends back: darker than the stock tint (as a surface colour it is light).
    vec3 scatter = body * vLook[0].z;

    // From below, past the critical angle, the surface mirrors the water. Real water's (cos 0.66) leaves a
    // third-person camera seeing up only when it looks steeply up; this one is wider, for the game.
    float kWindowCos = vLook[1].z;
    float window = below ? smoothstep(kWindowCos - 0.06, kWindowCos + 0.06, abs(dot(N, V))) : 1.0;
    vec3 seen = mix(scatter, behind * through + scatter * (1.0 - through), window);

    float cosV = clamp(dot(N, V), 0.0, 1.0);
    float fresnel = below ? 1.0 - window : min((vLook[1].x + (1.0 - vLook[1].x) * pow(1.0 - cosV, 5.0)) * vLook[1].y, 1.0);
    vec3 R = reflect(-V, N);
    vec3 sky = mix(regs.p[8].rgb, regs.p[7].rgb, sqrt(clamp(R.z, 0.0, 1.0)));
    vec3 reflected = scatter;

    vec4 ssr = vec4(0.0);

    if (!below && regs.p[14].x > 0.5)
    {
        ssr = ScreenReflection(vView.xyz, normalize(WorldDirToView(R)), R.z);
        reflected = mix(sky, ssr.rgb, ssr.a);
    }
    else if (below && regs.p[14].x > 0.5)
    {
        // From below, outside the window: the surface mirrors the water and what stands in it (the floor, rocks,
        // swimmers), the deep's own colour where the search finds nothing. The mirrored path is longer than the
        // surface's own: a little of the water's colour over it.
        // Where the search finds nothing, the deep as the underwater medium sees it far off (its glow); the
        // water's own shallow colour was a near, bright layer along the surface (Seyri, 2026-11-01).
        ssr = ScreenReflection(vView.xyz, normalize(WorldDirToView(R)), -1.0);
        reflected = mix(scene.underGlow.w > 0.5 ? scene.underGlow.rgb : scatter, ssr.rgb, ssr.a * 0.75);
    }

    vec3 toSun = normalize(LightsViewDirToWorld(-regs.v[33].xyz));
    // The sun's own glint only while it stands clearly above the horizon: the light's direction doesn't set
    // with the visible sun (it went on glinting "through the bottom of the ocean" at sundown, Seyri
    // 2026-10-28); low suns, the moons and the clouds come from the sky on screen (ScreenReflection), which
    // the glint gives way to.
    // The sky's bodies reflected (the sun, the moons, the mod's own), as the sky drew them this frame: their
    // disc where the mirror direction meets it, and a glow by the glint's sharpness; setting with the body
    // below the horizon. Off screen too (high moons). Without bodies, the light's direction stands in, only
    // while clearly up (it needn't set with the visible sun: it glinted "through the bottom of the ocean" at
    // sundown, Seyri 2026-10-28). The sky on screen (ScreenReflection) shows them already where it reaches.
    int bodies = int(scene.depthRange.w + 0.5);
    vec3 shine = vec3(0.0);

    for (int b = 0; b < 4 && b < bodies; b++)
    {
        vec3 d = scene.bodyDir[b].xyz;
        float r = max(scene.bodyDir[b].w, 0.002);
        float c = dot(R, d);
        float disc = smoothstep(cos(r * 1.3 + 0.004), cos(r * 0.7), c);
        float glow = pow(max(c, 0.0), vLook[4].x);
        vec3 colour = scene.bodyColour[b].rgb;
        shine += colour * (disc * 1.5 + glow * max(colour.r, max(colour.g, colour.b))) * smoothstep(-r, r, d.z);
    }

    float sun = below || bodies > 0 ? 0.0 : pow(max(dot(R, toSun), 0.0), vLook[4].x) * smoothstep(0.1, 0.3, toSun.z);
    shine += regs.v[36].rgb * sun;
    // From below, inside the window: the sky's bodies through the surface, along the view refracted out of the
    // water (water to air, 1.33), so the sun stands where it would, wobbling with the waves.
    vec3 through_ = vec3(0.0);

    if (below)
    {
        vec3 T = refract(-V, N, 1.333);

        if (dot(T, T) > 0.0)
        {
            for (int b = 0; b < 4 && b < bodies; b++)
            {
                vec3 d = scene.bodyDir[b].xyz;
                float r = max(scene.bodyDir[b].w, 0.002);
                float c = dot(T, d);
                float disc = smoothstep(cos(r * 1.3 + 0.004), cos(r * 0.7), c);
                float glow = pow(max(c, 0.0), vLook[4].x);
                vec3 colour = scene.bodyColour[b].rgb;
                through_ += colour * (disc * 1.5 + glow * max(colour.r, max(colour.g, colour.b))) * smoothstep(-r, r, d.z);
            }

            if (bodies == 0)
            {
                through_ += regs.v[36].rgb * pow(max(dot(T, toSun), 0.0), vLook[4].x) * smoothstep(0.1, 0.3, toSun.z);
            }
        }
    }

    vec3 highlight = below ? through_ * vLook[1].w * window : shine * vLook[1].w * (1.0 - ssr.a) + LampsReflected(world, N, V);

    // Foam where the crests fold: white, lit by the sun and the ambient, over the rest. At the shore: a band
    // at the waterline, widening and narrowing with the waves, and the crests where they break, both torn by
    // noise.
    float foam = below ? 0.0 : folding * vLook[4].z;

    if (!below && shoreWeight > 0.0 && scene.shoreLook.x > 0.0)
    {
        float d = max(shoreDist, 0.0);
        // Near the waterline, the distance from the water's real depth under this pixel (the surface
        // down to the scene behind it, along the view ray: the ground, a rock, a leg), so the band hugs
        // every bump and moves with the waves; the field's 2 yd cells alone drew it soft and blocky.
        // As if the floor fell a yard per 6 yd; further out, the field.
        float sceneHere = textureLod(sceneW, here, 0.0).r;
        float floorZ = eye.z + (world.z - eye.z) * (sceneHere / max(vView.w, 1e-3));
        float dLap = min(d, max(world.z - floorZ, 0.0) * 6.0);
        // The whitewater band at the waterline on the waves' own timing: a crest arriving pushes it out
        // (the run-up, a moment after it breaks), then it slides back. Narrower and calmer where the
        // shore is sheltered.
        float lapWidth = (0.5 + 2.2 * ShoreSwash(arrival)) * mix(0.5, 1.0, min(shoreExposure, 1.0));
        float lap = (1.0 - smoothstep(0.0, lapWidth, dLap)) * mix(0.4, 1.0, min(shoreExposure, 1.0));
        float breaking = shoreCrest * (1.0 - smoothstep(1.5, 7.0, d)) * smoothstep(0.3, 1.2, d);
        // The foam's pattern carried with the wind (the waves come downwind): two copies half a cycle apart,
        // each fading out as it starts over (a new pattern each cycle), so neither the reset nor the clock's
        // wrap shows (600 s is a whole number of cycles). One direction everywhere: carried toward the
        // nearest shore instead, the pattern stretched round every headland and bunched up in the bay
        // after it, which on WoW's ragged coasts is most places (Seyri, 2026-10-30).
        const float kCycle = 4.0;   // s
        vec2 drift = scene.shoreWind.xy * (shoreOmega / shoreK) * kCycle;
        float t = regs.p[6].x / kCycle;
        float phaseA = fract(t), phaseB = fract(t + 0.5);
        vec2 seedA = vec2(37.1, 17.3) * mod(floor(t), 150.0), seedB = vec2(37.1, 17.3) * mod(floor(t + 0.5), 150.0) + vec2(11.0, 5.0);
        vec2 pa = world.xy - drift * phaseA + seedA, pb = world.xy - drift * phaseB + seedB;
        float weightA = 1.0 - abs(2.0 * phaseA - 1.0);
        float pattern = mix(FoamPattern(pb, pixel, regs.p[6].x), FoamPattern(pa, pixel, regs.p[6].x), weightA);
        // Thinning out as it travels: it tears more the older the copy is. A narrow edge for crisp
        // froth, widened where a pixel covers more of the pattern.
        float age = mix(phaseB, phaseA, weightA);
        float tear = 0.42 + 0.12 * age, edge = 0.07 + clamp(pixel * 0.4, 0.0, 0.2);
        float torn = smoothstep(tear - edge, tear + edge, pattern);
        foam = max(foam, scene.shoreLook.x * shoreWeight * max(lap, breaking) * torn);
    }
    vec3 foamColour = clamp(regs.v[34].rgb + regs.v[35].rgb * max(toSun.z, 0.0), 0.0, 1.0) * 0.9 + lamps * 0.3;
    vec3 surface = mix(mix(seen, reflected, fresnel) + highlight, foamColour, foam);

    // The fog on the water's own light; the scene seen through it carries its own fog already.
    float open = window * (1.0 - fresnel) * (1.0 - foam);
    vec3 clear = behind * through * open;
    vec3 own = surface - clear;
    float coverage = 1.0 - max(through.r, max(through.g, through.b)) * open;
    // The liquid's own fog, as the stock vsLiquidWater computes it: c4 = (scale, offset, power) of the view
    // depth (1 no fog .. 0 all fog), the colour in pixel c4 (Liquid::SetShaderConstants). Not the device's
    // linear fog (GxRs_FogStart / End): the liquid pass leaves those as they stood, its end a few yards out,
    // which painted everything past it in the fog colour (the "lavender layer", Seyri 2026-10-28).
    float fog = regs.v[4].z > 0.0 ? min(pow(max(vView.z * regs.v[4].x + regs.v[4].y, 0.0), regs.v[4].z), 1.0) : 1.0;
    fog = pc.fog.w > 0.5 ? fog : 1.0;
    vec3 fogged = clear + own * fog + regs.p[4].rgb * (1.0 - fog) * coverage;

    // The water's edge fades into the ground behind it over the last half yard of water crossed (no hard
    // line along the beach), but for its foam.
    if (!below)
    {
        fogged = mix(behind, fogged, max(smoothstep(0.0, 0.5, crossed), foam));
    }

    // The Water panel's debug views (p6.y): 1 the screen-space reflection (its colour where it found
    // something, red where the sky stands in), 2 the water crossed (white: 20 yd and more), 3 the normal,
    // 4 the crests' folding and the foam, 5 the reflection alone (Fresnel as grey), 6 the scene behind.
    int debugView = int(regs.p[6].y + 0.5);

    if (debugView == 1)
    {
        fogged = mix(vec3(0.4, 0.0, 0.0), ssr.rgb, ssr.a);
    }
    else if (debugView == 2)
    {
        fogged = vec3(clamp(crossed / 20.0, 0.0, 1.0));
    }
    else if (debugView == 3)
    {
        fogged = N * 0.5 + 0.5;
    }
    else if (debugView == 4)
    {
        fogged = vec3(folding, foam, 0.0);
    }
    else if (debugView == 5)
    {
        fogged = reflected * 0.85 + vec3(fresnel) * 0.15;
    }
    else if (debugView == 6)
    {
        fogged = behind;
    }
    else if (debugView == 8)
    {
        // Green a hit (darker the more steps), red left the screen, magenta towards the camera, yellow out
        // of steps; cyan mixed in where it passed behind something too deep to count.
        vec3 c = ssrOutcome == 4 ? vec3(0.2, 0.4, 1.0)   // blue: the sky on screen
               : ssrOutcome == 3 ? vec3(0.0, 1.0 - float(ssrSteps) / 50.0, 0.0)
               : ssrOutcome == 1 ? vec3(1.0, 0.0, 0.0)
               : ssrOutcome == 0 ? vec3(1.0, 0.0, 1.0)
                                 : vec3(1.0, 1.0, 0.0);
        fogged = ssrThick && ssrOutcome != 3 ? mix(c, vec3(0.0, 1.0, 1.0), 0.6) : c;
    }
    else if (debugView == 9)
    {
        // The depth conversion's self-test: this pixel's own depth turned back into w the capture's way,
        // against its true w (black: right; red: off, full at 10 %).
        float ndc = (gl_FragCoord.z - scene.depthRange.x) / max(scene.depthRange.y - scene.depthRange.x, 1e-6);
        float z = (scene.depthProj.y - ndc * scene.depthProj.w) / (ndc * scene.depthProj.z - scene.depthProj.x);
        float w = z * scene.depthProj.z + scene.depthProj.w;
        fogged = vec3(clamp(abs(w - vView.w) / max(vView.w, 1e-3) * 10.0, 0.0, 1.0), 0.0, 0.0);
    }
    else if (debugView == 11)
    {
        // The shore field: red over land by its distance to the water (16 yd), blue over water by its distance
        // to the shore (32 yd), green the depth (8 yd); grey outside the field.
        fogged = mix(vec3(0.25), vec3(clamp(-shoreDist / 16.0, 0.0, 1.0), clamp(shoreDepth / 8.0, 0.0, 1.0), clamp(shoreDist / 32.0, 0.0, 1.0)), shoreWeight);
    }
    else if (debugView == 12)
    {
        // The shore waves: their crests (red) and the foam (white).
        fogged = mix(vec3(shoreCrest, 0.0, 0.0), vec3(1.0), foam);
    }
    else if (debugView == 10)
    {
        // The surface movement's easing from the depth byte: black still (the shore), white moving fully.
        fogged = vec3(vEase);
    }
    else if (debugView == 7)
    {
        // The screen mapping's check: the pixel's own point through ToScene against where the pixel is
        // (black: they agree; red / green: off in x / y, full at 1/20 of the screen), the depth compared
        // in blue (its w against the copy's at the same spot: blue where the water lies behind the copy).
        vec3 self = ToScene(vView.xyz);
        vec2 off = abs(self.xy - here) * 20.0;
        fogged = vec3(clamp(off, 0.0, 1.0), textureLod(sceneW, here, 0.0).r < vView.w - 0.05 ? 1.0 : 0.0);
    }

    oC0 = vec4(fogged, 1.0);
}
