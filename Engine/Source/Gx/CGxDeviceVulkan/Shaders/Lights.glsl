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
    vec3 lamps = LightsAt(LightsViewToWorld(viewPos.xyz), normalize(LightsViewDirToWorld(nv)), count);

    if (LightsDebug() == 1)
    {
        return vec4(vec3(0.15) + lamps, stock.a);
    }

    return vec4(LightsCombine(clamp(stock.rgb, 0.0, 1.0), lamps), stock.a);
}

#endif
