// SXC: the swash on the beach (Seyri, 2026-10-30): the thin sheet of water each wave pushes up the sand past
// the water's own edge, a line of foam at its front, and the sand it leaves wet. Drawn by the terrain from
// the water's shore field (Vulkan/Water.hpp's Shore(), bound by Vulkan/Cover.cpp at set 2 bindings 24 and
// 25), on the water's own timing (WaterShore.glsl's ShoreArrival and ShoreSwash, reproduced here exactly),
// so the sheet runs up as the near mesh's water rises. Not from the ground moisture: rain and everything
// else feed that too.
//
// How far up: by height, not by distance. The front climbs to a height above the still water (the
// Water panel's run-up, times the shore's exposure to the waves), and it reaches wherever the sand is lower
// than that: a flat beach far, a steep one hardly, from the terrain itself.
//
// Included by Terrain.frag after TerrainCover.glsl (its CoverBlock carries shoreGrid, shoreLook, shoreWind).

layout(set = 2, binding = 24) uniform sampler2D sShoreField;   // signed distance (yd, + over water), depth, direction to the other kind
layout(set = 2, binding = 25) uniform sampler2D sShoreCells;   // the nearest water's surface (world z), ground, LiquidType (+ 0.25 not water), covered
layout(set = 2, binding = 26) uniform sampler2D sShoreWaves;   // the long waves' displacement (cascade 0: dx, dy, height)
layout(set = 2, binding = 27) uniform sampler2D sShoreRipples; // the short waves' slopes (cascade 2: slope x, y, Jacobian, height)

const float kSwashHalfWorld = 32.0 * 533.33333;
const float kSwashTau = 6.2831853;
const float kSwashFarthest = 24.0;   // yd onto the land at most

// WaterShore.glsl's Hash and Noise, the same, so the timing matches the water's.
float SwashHash(vec2 p)
{
    p = mod(p, 1024.0);
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float SwashNoise(vec2 p)
{
    vec2 i = mod(floor(p), 1024.0), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(SwashHash(i), SwashHash(i + vec2(1.0, 0.0)), f.x), mix(SwashHash(i + vec2(0.0, 1.0)), SwashHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

vec4 SwashField(vec2 world, out float weight)
{
    vec2 g = vec2(kSwashHalfWorld - world.y, kSwashHalfWorld - world.x) / cover.shoreGrid.w;
    vec2 inside = g - cover.shoreGrid.xy;
    vec2 edge = min(inside, vec2(256.0) - inside);
    weight = clamp((min(edge.x, edge.y) - 2.0) / 14.0, 0.0, 1.0);
    return textureLod(sShoreField, g / 256.0, 0.0);
}

// The water's rise above its still surface here (yd) as the near mesh draws it: the long waves (WaterMove.glsl:
// cascade 0 averaged as over its vertices, eased by the water's depth, fading with distance). `g` the point
// in global cells. The easing blended between the four nearest cell centres, as the mesh's triangles blend
// its vertices': taken from one cell, it stepped at every cell's edge and the swash broke into cells.
// (Seyri, 2026-10-31: the swash stayed at the still waterline while the waves climbed and dropped a cliff.)
float WaterRise(vec3 world, vec2 g)
{
    if (cover.shoreWave.z < 0.5)
    {
        return 0.0;
    }

    vec2 u = g - 0.5;
    ivec2 i0 = ivec2(floor(u));
    vec2 f = u - vec2(i0);
    vec4 c00 = texelFetch(sShoreCells, i0 & 255, 0), c10 = texelFetch(sShoreCells, (i0 + ivec2(1, 0)) & 255, 0);
    vec4 c01 = texelFetch(sShoreCells, (i0 + ivec2(0, 1)) & 255, 0), c11 = texelFetch(sShoreCells, (i0 + ivec2(1, 1)) & 255, 0);
    vec4 depth = vec4(c00.x - c00.y, c10.x - c10.y, c01.x - c01.y, c11.x - c11.y);
    vec4 ease = mix(vec4(0.6), vec4(1.0), smoothstep(vec4(0.0), vec4(4.0), depth));
    float deep = mix(mix(ease.x, ease.y, f.x), mix(ease.z, ease.w, f.x), f.y);
    vec2 uv = world.xy * cover.shoreWave.y;
    float s2 = 2.0 * cover.shoreWave.y;
    float d = (textureLod(sShoreWaves, uv, 0.0).z * 2.0
             + textureLod(sShoreWaves, uv + vec2(s2, 0.0), 0.0).z + textureLod(sShoreWaves, uv - vec2(s2, 0.0), 0.0).z
             + textureLod(sShoreWaves, uv + vec2(0.0, s2), 0.0).z + textureLod(sShoreWaves, uv - vec2(0.0, s2), 0.0).z) / 6.0;
    d = clamp(d, -1.0, 1.0) * (1.0 - smoothstep(150.0, 260.0, length(world - cover.shoreCamera.xyz)));
    return d * cover.shoreWave.x * deep;
}

struct Swash
{
    float wet;    // 0..1: the sand's wetness (1 under the sheet)
    float foam;   // 0..1
};

// `up`: the ground normal's z. Swash runs up beaches; on steep rock the waves hit and fall back (spray,
// later: water-contact) on their own timing, which the swash's (the shore waves') isn't, so it climbed a cliff
// out of step with the waves seen there (Seyri, 2026-10-31). There only the moving waterline and its wet.
Swash TerrainSwash(vec3 world, float up)
{
    Swash r;
    r.wet = 0.0;
    r.foam = 0.0;

    if (cover.shoreGrid.z < 0.5 || cover.shoreLook.w <= 0.0)
    {
        return r;
    }

    float weight;
    vec4 field = SwashField(world.xy, weight);
    float land = -field.x;   // yd from the waterline onto the land

    if (weight <= 0.0 || land < -2.0 || land > kSwashFarthest)
    {
        return r;
    }

    vec2 g = vec2(kSwashHalfWorld - world.y, kSwashHalfWorld - world.x) / cover.shoreGrid.w;
    vec4 cell = texelFetch(sShoreCells, ivec2(floor(g)) & 255, 0);

    if (cell.z < 0.5 || fract(cell.z) > 0.1)
    {
        return r;   // no water near, or lava and slime (no wet sand there)
    }

    float rise = WaterRise(world, g);

    float h = world.z - (cell.x + rise);   // above the water as it stands now

    // Only where the water could climb to: no ground between here and the waterline higher than both. Past a
    // lake's rim, the hollow behind it and the outer face of a cliff stay dry (Seyri, 2026-10-31). Below the
    // surface as the near mesh decides (only the cells it covers), and any rise over the water is a rim; on
    // the sand above it, small bumps don't count.
    if (h < 0.0 && cell.w < 0.5)
    {
        return r;
    }
    float rim = max(world.z, cell.x) + mix(0.05, 0.3, smoothstep(0.0, 0.5, h));
    for (float along = 1.0; along < land; along += 2.0)
    {
        vec2 q = world.xy + field.zw * along;
        vec2 gq = vec2(kSwashHalfWorld - q.y, kSwashHalfWorld - q.x) / cover.shoreGrid.w;
        if (texelFetch(sShoreCells, ivec2(floor(gq)) & 255, 0).y > rim)
        {
            return r;
        }
    }

    // Exposure as the water reckons it (WaterShore.glsl's ShoreExposure): facing the waves, open water
    // upwind. On land the field's direction points to the water.
    vec2 wind = cover.shoreWind.xy;
    float facing = smoothstep(-0.2, 0.6, dot(wind, -field.zw));
    float w1, w2;
    float up1 = SwashField(world.xy - wind * 20.0, w1).x;
    float up2 = SwashField(world.xy - wind * 50.0, w2).x;
    float open = 0.5 * mix(1.0, smoothstep(0.0, 15.0, up1), w1) + 0.5 * mix(1.0, smoothstep(0.0, 15.0, up2), w2);
    float exposure = mix(0.1, 1.0, facing * open) * clamp(cover.shoreWind.z / 8.0, 0.25, 1.5);

    // The waves' timing (WaterShore.glsl's ShoreOmega, ShoreArrival, ShoreSwash).
    float time = cover.shoreWind.w;
    float k = kSwashTau / max(cover.shoreLook.z, 1.0);
    float omega = max(round(k * 3.0 * 600.0 / kSwashTau), 1.0) * kSwashTau / 600.0;
    float arrival = omega * time + 2.5 * SwashNoise(world.xy / 24.0);
    float s = 0.5 + 0.5 * sin(arrival - 0.8);
    float reach = cover.shoreLook.w * exposure;   // the highest the front climbs (yd)
    float front = reach * s * s;
    float lead = front - h;                       // how deep the sheet is here (yd); < 0 ahead of it

    // The sheet, and the sand it left: wet up to about the highest front, drying above.
    float beach = smoothstep(0.45, 0.75, up);   // ~63..41 degrees: the sheet and its foam fade on steeper rock
    float sheet = smoothstep(0.0, 0.02, lead) * beach;
    float left = 1.0 - smoothstep(0.75, 1.1, h / max(reach, 0.01));
    r.wet = max(sheet, 0.6 * left) * weight * (h > -0.3 ? 1.0 : 0.0);

    // Foam along the front while it climbs, torn by a pattern carried with the wind (two copies half a 4 s
    // cycle apart, as the water's).
    float rising = smoothstep(-0.3, 0.4, cos(arrival - 0.8));
    float band = smoothstep(0.0, 0.012, lead) * (1.0 - smoothstep(0.03, 0.03 + 0.06 * max(reach, 0.2), lead));
    float t = time / 4.0;
    float phaseA = fract(t), phaseB = fract(t + 0.5);
    vec2 drift = wind * 3.0;
    vec2 pa = world.xy - drift * phaseA + vec2(37.1, 17.3) * mod(floor(t), 150.0);
    vec2 pb = world.xy - drift * phaseB + vec2(37.1, 17.3) * mod(floor(t + 0.5), 150.0) + vec2(11.0, 5.0);
    float na = SwashNoise(pa * 1.4) * 0.6 + SwashNoise(pa * 4.1) * 0.4;
    float nb = SwashNoise(pb * 1.4) * 0.6 + SwashNoise(pb * 4.1) * 0.4;
    float pattern = mix(nb, na, 1.0 - abs(2.0 * phaseA - 1.0));
    r.foam = band * beach * mix(0.35, 1.0, rising) * smoothstep(0.38, 0.55, pattern) * cover.shoreLook.x * weight;
    return r;
}

// --- Caustics (Seyri, 2026-10-31; docs/kb underwater) ---------------------------------------------------
// The sunlight the waves focus into a moving net of light on the floor under the water. Drawn by the terrain
// where the shore field says water lies above it (terrain water, not WMO water), from the depth under the
// still surface; the settings are the row's of the water the player is in (strength, cell size, how deep).

// A whole number of turns per 600 s (the water's clock wraps there) near `seconds` per turn.
float CausticTurns(float seconds)
{
    return max(round(600.0 / seconds), 1.0) * kSwashTau / 600.0;
}

// How far from the edges of a layer of cells whose points wander about their places (0 on an edge).
float CausticEdges(vec2 p, float time, float seconds)
{
    vec2 i = floor(p), f = fract(p);
    float d1 = 8.0, d2 = 8.0;
    float w = CausticTurns(seconds);

    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            vec2 o = vec2(float(x), float(y));
            vec2 h = vec2(SwashHash(i + o), SwashHash(i + o + 17.0));
            vec2 r = o + 0.5 + 0.4 * sin(w * time + kSwashTau * h) - f;
            float d = dot(r, r);

            if (d < d1)
            {
                d2 = d1;
                d1 = d;
            }
            else if (d < d2)
            {
                d2 = d;
            }
        }
    }

    return sqrt(d2) - sqrt(d1);
}

// The caustics' pattern here (0 none, about 1 on a bright line), for the sun's light and our lamps' alike:
// `sun` toward the sun (world, unit; straight down where it's low), `pixel` yd per pixel (they settle to an
// even glow where a pixel covers a cell, so they don't shimmer).
//
// Driven by the waves above (Seyri, 2026-10-31): the net is bent by the short waves' slopes over it, so it
// ripples with them, brighter under crests (where the surface curves into a lens) and faint under calm water;
// measured under the water as it stands (the long waves' rise), so a trough that bares the floor takes them
// away; blurred and dimmed as the weather thickens (rain breaks the surface up, clouds take the direct light).
float TerrainCaustics(vec3 world, vec3 sun, float pixel)
{
    if (cover.shoreGrid.z < 0.5 || cover.shoreCaustic.x <= 0.0)
    {
        return 0.0;
    }

    float weight;
    vec4 field = SwashField(world.xy, weight);

    if (weight <= 0.0 || field.x < -1.0)
    {
        return 0.0;   // not under water (the field's distance is positive over it)
    }

    vec2 g = vec2(kSwashHalfWorld - world.y, kSwashHalfWorld - world.x) / cover.shoreGrid.w;
    vec4 cell = texelFetch(sShoreCells, ivec2(floor(g)) & 255, 0);

    if (cell.z < 0.5 || fract(cell.z) > 0.1)
    {
        return 0.0;   // no water, or lava and slime
    }

    float reach = max(cover.shoreCaustic.z, 0.1);
    float depth = cell.x - world.z;

    if (depth > reach * 1.5 || depth < -1.0)
    {
        return 0.0;
    }

    depth += WaterRise(world, g);   // under the water as it stands now

    if (depth <= 0.02)
    {
        return 0.0;
    }

    // Where this light came in: up the light as the surface bent it (refracted, so steeper below); straight
    // down for a low or missing sun (our lamps', and night's).
    vec3 toward = sun.z > 0.05 ? sun : vec3(0.0, 0.0, 1.0);
    vec3 down = refract(-toward, vec3(0.0, 0.0, 1.0), 1.0 / 1.33);
    vec2 entry = world.xy + down.xy * (depth / min(down.z, -0.05));

    // The short waves over the entry: their slopes bend the light (the net shifts with them, more the deeper
    // it falls), their curvature focuses it (brighter under crests, the slopes' divergence negative), their
    // steepness sets how lively the net is. This water's wave scale on top.
    const float kRippleSize = 9.0;   // yd: cascade 2 (Vulkan/Water.hpp's kWaveSizes)
    const float kStep = 0.35;        // yd
    float waveScale = cover.shoreWave.x;
    vec2 ruv = entry / kRippleSize;
    float rs = kStep / kRippleSize;
    vec2 slope = textureLod(sShoreRipples, ruv, 0.0).xy * waveScale;
    float divergence = (textureLod(sShoreRipples, ruv + vec2(rs, 0.0), 0.0).x - textureLod(sShoreRipples, ruv - vec2(rs, 0.0), 0.0).x
                      + textureLod(sShoreRipples, ruv + vec2(0.0, rs), 0.0).y - textureLod(sShoreRipples, ruv - vec2(0.0, rs), 0.0).y)
                       * waveScale / (2.0 * kStep);
    float focus = clamp(1.0 - divergence * depth * 0.6, 0.35, 2.2);
    float lively = mix(0.35, 1.0, smoothstep(0.03, 0.25, length(slope)));

    float scale = max(cover.shoreCaustic.y, 0.25);
    vec2 p = (entry + slope * depth * 1.2) / scale;

    // The net churns as the waves pass: the pattern warped by noise that circles, two layers wandering at
    // different paces; the lines widen with depth (the light spreads), with the weather and with the pixel.
    float time = cover.shoreWind.w;
    float churn = CausticTurns(11.0) * time;
    vec2 circle = vec2(cos(churn), sin(churn)) * 2.0;
    p += (vec2(SwashNoise(p * 0.35 + circle), SwashNoise(p * 0.35 - circle.yx + 9.0)) - 0.5) * 0.8;
    float clouds = clamp(cover.shoreSun.w, 0.0, 1.0);
    float spread = 0.05 + depth * 0.012 + clouds * 0.15;
    float aa = pixel / scale;
    float width = spread + aa * 1.5;
    float a = 1.0 - smoothstep(0.0, width, CausticEdges(p, time, 2.6));
    float b = 1.0 - smoothstep(0.0, width, CausticEdges(p * 1.37 + vec2(3.1, 7.7), time, 3.7));
    float net = (a + b) * 0.45 + a * b * 0.9;
    net *= 0.05 / max(spread, 0.05);                     // spread light is dimmer
    net = mix(net, 0.3, smoothstep(0.15, 0.6, aa));      // far away: their mean, an even glow
    net *= focus * lively;

    float fade = smoothstep(0.02, 0.4, depth) * exp(-2.5 * depth / reach) * (1.0 - smoothstep(reach, reach * 1.5, depth));
    return net * fade * cover.shoreCaustic.x * weight * (1.0 - 0.9 * clouds);
}
