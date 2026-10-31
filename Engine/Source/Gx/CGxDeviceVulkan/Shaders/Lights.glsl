// SXC: our point lights (port plan step 6; SXC/Lights, Vulkan/Lights), per pixel: the lights listed in
// the pixel's cell of a world grid around the camera. The light follows WXL's (TerrainLights): a ~1 yd
// source falling off like the client's 1 / (0.7 d + 0.03 d^2) on sqrt(d^2 + 1), windowed to 0 at the
// radius ((1 - (d/r)^2)^2 to the light's falloff power), wrapped Lambert ((N.L + 0.5) / 1.5), a spot
// cone; added to the stock light with a soft knee on the brightest channel (the hue kept).
//
// Set 3 of our terrain pipelines and of the translated model and WMO pixel modules (step 6b: the
// translator inlines this file; Tools/VulkanShaders).

struct Light
{
    vec4 posInvRadius;    // world position, 1 / radius
    vec4 colorFalloff;    // colour (x brightness, fade, flicker), the window's power
    vec4 spotDirCos;      // where a spot points (world), cos of its outer angle (-2: a point light)
    vec4 extra;           // 1 / (cos inner - cos outer), flags, its shadow map's slot + 1 (0: none), 0
};

layout(std430, set = 3, binding = 0) readonly buffer LightBlock {
    vec4  lightGrid;      // corner x, y, 1 / cell size, cells per side
    vec4  lightInfo;      // lights per cell, lights, on, debug view
    vec4  lightViewInv[3]; // the world scene's view back to the world: world = (dot(row, (view, 1))) per row
    vec4  lightShadowInfo; // the shadow maps: texels per face side, 1 / that, the near plane (yd), 0
    Light lights[];
};

// A view-space point of the world scene in the world (the models and WMOs light in view space).
vec3 LightsViewToWorld(vec3 v)
{
    vec4 p = vec4(v, 1.0);
    return vec3(dot(lightViewInv[0], p), dot(lightViewInv[1], p), dot(lightViewInv[2], p));
}

vec3 LightsViewDirToWorld(vec3 v)
{
    return vec3(dot(lightViewInv[0].xyz, v), dot(lightViewInv[1].xyz, v), dot(lightViewInv[2].xyz, v));
}

layout(std430, set = 3, binding = 1) readonly buffer LightCells {
    uint cellLights[];    // per cell (y * cells + x): light indices, 0xFFFFFFFF ends a list
};

// The draw's surfaces (step 6c, the device binds a set per class): x 0 any (models: every light),
// 1 outdoor (the terrain, outdoor WMO groups: the lights outside), 2 indoor (indoor WMO groups: the
// lights inside an indoor group; a light's extra.y is 1 there).
layout(std140, set = 3, binding = 2) uniform LightSurface {
    vec4 lightSurface;
};

// The cube shadow maps of the nearest still lights (Vulkan/Shadows.cpp): six layers per slot (face =
// axis x 2 + (negative ? 1 : 0)), drawn from the WMO and terrain triangles around the light
// (LightShadow.vert); compared in hardware (1 lit, 0 shadowed).
layout(set = 3, binding = 3) uniform sampler2DArrayShadow lightShadows;

const int kMaxLightsPerCell = 32;

bool LightsOn() { return lightInfo.z > 0.5; }
int LightsDebug() { return int(lightInfo.w + 0.5); }

// How much of a light with a shadow map reaches a world point with a world normal (unit): 1 lit, 0 in
// its shadow; `d` the distance to the light. The point is pushed out along its normal by a texel and a
// half at its distance (no acne on the surface the map saw), then the face its direction falls on is
// read as LightShadow.vert drew it, four taps a texel apart (the hardware filters each 2 x 2: soft edges).
float LightShadow(Light l, vec3 p, vec3 n, float d)
{
    int slot = int(l.extra.z + 0.5) - 1;

    if (slot < 0)
    {
        return 1.0;
    }

    float near = lightShadowInfo.z;
    float far = 1.0 / l.posInvRadius.w;
    vec3 v = p + n * (3.0 * d * lightShadowInfo.y) - l.posInvRadius.xyz;
    vec3 a = abs(v);
    int axis = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
    float m = a[axis];

    if (m <= near)
    {
        return 1.0;
    }

    vec2 uv = axis == 0 ? v.yz : (axis == 1 ? v.zx : v.xy);
    uv = uv / m * 0.5 + 0.5;
    float layer = float(slot * 6 + axis * 2 + (v[axis] < 0.0 ? 1 : 0));
    float depth = far / (far - near) - far * near / ((far - near) * m);
    // Explicit gradients (level 0): the lights' loop is not uniform control flow.
    float t = lightShadowInfo.y;
    vec2 g = vec2(0.0);
    float lit = textureGrad(lightShadows, vec4(uv + vec2(-0.5, -0.5) * t, layer, depth), g, g)
              + textureGrad(lightShadows, vec4(uv + vec2(0.5, -0.5) * t, layer, depth), g, g)
              + textureGrad(lightShadows, vec4(uv + vec2(-0.5, 0.5) * t, layer, depth), g, g)
              + textureGrad(lightShadows, vec4(uv + vec2(0.5, 0.5) * t, layer, depth), g, g);
    return lit * 0.25;
}

// The first entry of a world point's cell (clamped to the grid: a light past its radius adds nothing).
int LightCellFirst(vec2 xy)
{
    int cells = int(lightGrid.w + 0.5);
    ivec2 c = clamp(ivec2(floor((xy - lightGrid.xy) * lightGrid.z)), ivec2(0), ivec2(cells - 1));
    return (c.y * cells + c.x) * int(lightInfo.x + 0.5);
}

// The lights at a world point with a world normal (unit), summed (rgb); `count` how many reach it.
vec3 LightsAt(vec3 p, vec3 n, out int count)
{
    vec3 sum = vec3(0.0);
    count = 0;

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

        // Inside and outside apart: a light lights its own side's surfaces only (models both). A light
        // with a shadow map needs no such guess: its walls, floors and roofs are in the map.
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
        vec3 dir = toLight * inversesqrt(max(d2, 1e-8));
        float lambert = clamp((dot(n, dir) + 0.5) * (1.0 / 1.5), 0.0, 1.0);
        float spot = clamp((dot(-dir, l.spotDirCos.xyz) - l.spotDirCos.w) * l.extra.x, 0.0, 1.0);
        float s = sqrt(d2 + 1.0);
        float strength = window * lambert * spot / ((0.7 + 0.03 * s) * s);

        if (shadowed && strength > 0.0)
        {
            strength *= LightShadow(l, p, n, d);
        }

        sum += l.colorFalloff.rgb * strength;
        count += strength > 0.0 ? 1 : 0;
    }

    return sum;
}

// The stock light `stock` (0..1 per channel, as the client lights the surface) with the lamps added like
// the client adds its own lights; above a knee (0.8, or the stock light's brightest channel if higher)
// the brightest channel is compressed smoothly (knee + room (1 - e^(-over / room)), slope 1 at the knee:
// no ring) and all three scaled alike, so a flame's hue stays. Without lamps: the stock light.
vec3 LightsCombine(vec3 stock, vec3 lamps)
{
    vec3 total = stock + lamps;
    float top = max(total.r, max(total.g, total.b));
    float knee = max(max(stock.r, max(stock.g, stock.b)), 0.8);
    float over = max(top - knee, 0.0);
    float room = max(1.0 - knee, 0.001);
    float shaped = top - over + room * (1.0 - exp(-over / room));
    return total * (shaped / max(top, 0.001));
}


// --- Caustics (docs/kb underwater) --------------------------------------------------------------------------
// The sunlight the waves focus into a moving net of light under the water, shared by the terrain (its own set 2,
// TerrainShore.glsl) and the lit models, WMOs and grass (set 3 below, LightsModelColor). Where the water's shore
// field says water lies above (terrain water), from the depth under the water as it stands; bent by the short
// waves' slopes above and brighter under crests; widening and dimming with depth, the weather and the pixel.

struct CausticInput
{
    vec4 grid;      // the shore field's first cell (x, y; x along world -y), 1 when there, the cell (yd)
    vec4 caustic;   // strength, yd per cell, yd they reach, the debug view
    vec4 sun;       // toward the sun (world, unit), how cloudy (0..1)
    vec4 wave;      // the long waves: movement x wave scale, 1 / cascade 0's size (1 / yd), 1 when they run; the clock (s)
    vec4 camera;    // the camera (world): the waves' movement fades with its distance
};

const float kCausticHalfWorld = 32.0 * 533.33333;
const float kCausticTau = 6.2831853;

// Hashed and value noise over the world, the lattice wrapped every 1024 per point (seamless, precise far out).
float CausticHash(vec2 p)
{
    p = mod(p, 1024.0);
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float CausticNoise(vec2 p)
{
    vec2 i = mod(floor(p), 1024.0), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(CausticHash(i), CausticHash(i + vec2(1.0, 0.0)), f.x), mix(CausticHash(i + vec2(0.0, 1.0)), CausticHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// A whole number of turns per 600 s (the water's clock wraps there) near `seconds` per turn.
float CausticTurns(float seconds)
{
    return max(round(600.0 / seconds), 1.0) * kCausticTau / 600.0;
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
            vec2 h = vec2(CausticHash(i + o), CausticHash(i + o + 17.0));
            vec2 r = o + 0.5 + 0.4 * sin(w * time + kCausticTau * h) - f;
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

// A world point in the shore field's global cells.
vec2 CausticCell(CausticInput c, vec2 world)
{
    return vec2(kCausticHalfWorld - world.y, kCausticHalfWorld - world.x) / c.grid.w;
}

// The water's rise above its still surface (yd) as the near mesh draws it: the long waves (WaterMove.glsl: cascade
// 0 averaged as over its vertices, eased by the water's depth, fading with distance). The easing blended between
// the four nearest cell centres, as the mesh's triangles blend its vertices' (from one cell it stepped at every
// cell's edge: Seyri, 2026-10-31). `g` the point in global cells.
float CausticRise(CausticInput c, sampler2D cells, sampler2D waves, vec3 world, vec2 g)
{
    if (c.wave.z < 0.5)
    {
        return 0.0;
    }

    vec2 u = g - 0.5;
    ivec2 i0 = ivec2(floor(u));
    vec2 f = u - vec2(i0);
    vec4 c00 = texelFetch(cells, i0 & 255, 0), c10 = texelFetch(cells, (i0 + ivec2(1, 0)) & 255, 0);
    vec4 c01 = texelFetch(cells, (i0 + ivec2(0, 1)) & 255, 0), c11 = texelFetch(cells, (i0 + ivec2(1, 1)) & 255, 0);
    vec4 depth = vec4(c00.x - c00.y, c10.x - c10.y, c01.x - c01.y, c11.x - c11.y);
    vec4 ease = mix(vec4(0.6), vec4(1.0), smoothstep(vec4(0.0), vec4(4.0), depth));
    float deep = mix(mix(ease.x, ease.y, f.x), mix(ease.z, ease.w, f.x), f.y);
    vec2 uv = world.xy * c.wave.y;
    float s2 = 2.0 * c.wave.y;
    float d = (textureLod(waves, uv, 0.0).z * 2.0
             + textureLod(waves, uv + vec2(s2, 0.0), 0.0).z + textureLod(waves, uv - vec2(s2, 0.0), 0.0).z
             + textureLod(waves, uv + vec2(0.0, s2), 0.0).z + textureLod(waves, uv - vec2(0.0, s2), 0.0).z) / 6.0;
    d = clamp(d, -1.0, 1.0) * (1.0 - smoothstep(150.0, 260.0, length(world - c.camera.xyz)));
    return d * c.wave.x * deep;
}

// The caustics' pattern here (0 none, about 1 on a bright line), for the sun's light and our lamps' alike. `pixel`
// yd per pixel (an even glow where a pixel covers a cell, so they don't shimmer). The samplers: the shore field
// (signed distance, depth, direction), its cells (surface, ground, type, covered), cascade 0's displacement and
// cascade 2's slopes, all stored toroidally.
float CausticsAt(CausticInput c, sampler2D field, sampler2D cells, sampler2D waves, sampler2D ripples, vec3 world, float pixel)
{
    if (c.grid.z < 0.5 || c.caustic.x <= 0.0 || c.grid.w <= 0.0)
    {
        return 0.0;
    }

    vec2 g = CausticCell(c, world.xy);
    vec2 inside = g - c.grid.xy;
    vec2 edge = min(inside, vec2(256.0) - inside);
    float weight = clamp((min(edge.x, edge.y) - 2.0) / 14.0, 0.0, 1.0);

    if (weight <= 0.0 || textureLod(field, g / 256.0, 0.0).x < -1.0)
    {
        return 0.0;   // outside the field, or not under water (its distance is positive over water)
    }

    vec4 cell = texelFetch(cells, ivec2(floor(g)) & 255, 0);

    if (cell.z < 0.5 || fract(cell.z) > 0.1)
    {
        return 0.0;   // no water, or lava and slime
    }

    float reach = max(c.caustic.z, 0.1);
    float depth = cell.x - world.z;

    if (depth > reach * 1.5 || depth < -1.0)
    {
        return 0.0;
    }

    depth += CausticRise(c, cells, waves, world, g);   // under the water as it stands (a trough baring the floor: none)

    if (depth <= 0.02)
    {
        return 0.0;
    }

    // Where this light came in: up the light as the surface bent it (refracted, so steeper below); straight down
    // for a low or missing sun (our lamps', and night's).
    vec3 toward = c.sun.z > 0.05 ? c.sun.xyz : vec3(0.0, 0.0, 1.0);
    vec3 down = refract(-toward, vec3(0.0, 0.0, 1.0), 1.0 / 1.33);
    vec2 entry = world.xy + down.xy * (depth / min(down.z, -0.05));

    // The short waves over the entry: their slopes bend the light (the net shifts with them, more the deeper it
    // falls), their curvature focuses it (brighter under crests: the slopes' divergence negative), their
    // steepness sets how lively the net is. This water's wave scale on top.
    const float kRippleSize = 9.0;   // yd: cascade 2 (Vulkan/Water.hpp's kWaveSizes)
    const float kStep = 0.35;        // yd
    float waveScale = c.wave.x;
    vec2 ruv = entry / kRippleSize;
    float rs = kStep / kRippleSize;
    vec2 slope = textureLod(ripples, ruv, 0.0).xy * waveScale;
    float divergence = (textureLod(ripples, ruv + vec2(rs, 0.0), 0.0).x - textureLod(ripples, ruv - vec2(rs, 0.0), 0.0).x
                      + textureLod(ripples, ruv + vec2(0.0, rs), 0.0).y - textureLod(ripples, ruv - vec2(0.0, rs), 0.0).y)
                       * waveScale / (2.0 * kStep);
    float focus = clamp(1.0 - divergence * depth * 0.6, 0.35, 2.2);
    float lively = mix(0.35, 1.0, smoothstep(0.03, 0.25, length(slope)));

    float scale = max(c.caustic.y, 0.25);
    vec2 p = (entry + slope * depth * 1.2) / scale;

    // The net churns as the waves pass: the pattern warped by noise that circles, two layers wandering at different
    // paces; the lines widen with depth (the light spreads), with the weather and with the pixel.
    float time = c.wave.w;
    float churn = CausticTurns(11.0) * time;
    vec2 circle = vec2(cos(churn), sin(churn)) * 2.0;
    p += (vec2(CausticNoise(p * 0.35 + circle), CausticNoise(p * 0.35 - circle.yx + 9.0)) - 0.5) * 0.8;
    float clouds = clamp(c.sun.w, 0.0, 1.0);
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
    return net * fade * c.caustic.x * weight * (1.0 - 0.9 * clouds);
}

// The water's shore for the lit models, WMOs and grass (Vulkan/Lights.cpp from the water's Shore()): its values
// and the four images; `grid.z` 0 without them (a white image bound instead).
layout(std140, set = 3, binding = 4) uniform LightShore {
    vec4 grid;
    vec4 caustic;
    vec4 sun;
    vec4 sunColour;   // the sun's direct light (the day's diffuse)
    vec4 wave;
    vec4 camera;
} lightShore;
layout(set = 3, binding = 5) uniform sampler2D lightShoreField;
layout(set = 3, binding = 6) uniform sampler2D lightShoreCells;
layout(set = 3, binding = 7) uniform sampler2D lightShoreWaves;
layout(set = 3, binding = 8) uniform sampler2D lightShoreRipples;

float LightsCaustics(vec3 world, float pixel)
{
    CausticInput c = CausticInput(lightShore.grid, lightShore.caustic, lightShore.sun, lightShore.wave, lightShore.camera);
    return CausticsAt(c, lightShoreField, lightShoreCells, lightShoreWaves, lightShoreRipples, world, pixel);
}

#ifndef LIGHTS_COMPUTE // the fog's light volume (FogLights.comp): no screen-space derivatives in compute

// A translated model or WMO pixel's lit colour (step 6b): `stock` is the vertex module's lit colour
// (color0: the sun, the ambient, the client's own point lights, x the material), `viewPos` /
// `viewNormal` what the translator captured in the vertex module (w 1 when found: the projection's
// input, the normal the sun lights); without a normal (WMO groups drawn with baked vertex colours) the
// surface's own from the position's slopes. Our lights join the light the texture is multiplied by.
vec4 LightsModelColor(vec4 stock, vec4 viewPos, vec4 viewNormal)
{
    // The slopes first: derivatives must be taken in uniform control flow.
    vec3 faceted = cross(dFdx(viewPos.xyz), dFdy(viewPos.xyz));
    vec3 worldPos = LightsViewToWorld(viewPos.xyz);
    float pixel = length(fwidth(worldPos.xy));   // yd per pixel: the caustics' fine lines

    if (!LightsOn() || viewPos.w < 0.5)
    {
        return stock;
    }

    vec3 nv = viewNormal.w > 0.5 ? viewNormal.xyz : faceted;
    nv = dot(nv, nv) > 1e-12 ? normalize(nv) : vec3(0.0, 0.0, 1.0);

    // A slope normal has no set side: the one facing the camera.
    if (viewNormal.w < 0.5 && dot(nv, -viewPos.xyz) < 0.0)
    {
        nv = -nv;
    }

    int count;
    vec3 nWorld = normalize(LightsViewDirToWorld(nv));
    vec3 lamps = LightsAt(worldPos, nWorld, count);

    if (LightsDebug() == 1)
    {
        return vec4(vec3(0.15) + lamps, stock.a);
    }

    // The caustics under the water (docs/kb underwater): more of the sun's light where the waves focus it, on
    // what faces it (rocks, wrecks, creatures' backs), and of our lamps' in their colour.
    float caustic = LightsCaustics(worldPos, pixel);

    if (caustic > 0.0)
    {
        vec3 sun = lightShore.sun.xyz;
        float facing = clamp(dot(nWorld, sun), 0.0, 1.0) * smoothstep(0.05, 0.2, sun.z);
        lamps += caustic * (lightShore.sunColour.rgb * facing + lamps * 0.8);
    }

    return vec4(LightsCombine(clamp(stock.rgb, 0.0, 1.0), lamps), stock.a);
}

#endif
