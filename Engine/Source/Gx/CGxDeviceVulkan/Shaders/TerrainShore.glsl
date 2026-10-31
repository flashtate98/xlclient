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

struct Swash
{
    float wet;    // 0..1: the sand's wetness (1 under the sheet)
    float foam;   // 0..1
};

Swash TerrainSwash(vec3 world)
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

    float h = world.z - cell.x;   // above the still water

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
    float sheet = smoothstep(0.0, 0.02, lead);
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
    r.foam = band * mix(0.35, 1.0, rising) * smoothstep(0.38, 0.55, pattern) * cover.shoreLook.x * weight;
    return r;
}
