// SXC: our terrain pixel shader (Vulkan/Pipelines: FragmentProgram::Terrain), in place of the client's
// "Terrain1" records (no shadow map: 32) and "Terrain3" records (shadow map modes 1-3: 96). One
// source, compiled once per layer count (LAYERS 1..4) and shadow group (SHADOW 0 none, 1 one cascade,
// 2 three cascades and the far one), because a module must have every sampler it declares bound; the
// finer options are specialization constants. The device sets them from the record the engine binds:
//   Terrain1:n  n = 16 unlit + 8 bigAlpha + 2 (layers - 1) + cube
//   Terrain3:n  n = 48 unlit + 24 bigAlpha + 8 (mode - 1) + 2 (layers - 1) + cube
// (CMap::GetTerrainPixelShader). Records with a cube layer stay the stock ones.
//
// Samplers: s0..s(LAYERS-1) the layers, s(LAYERS) the alpha map (rgb the layers' weights, a the baked
// shadow), s5..s8 the shadow cascades, s9..s(8+LAYERS) the layers' height textures. Pixel registers: p1
// the layers' lighting (0 = unlit), p2 the fog colour, p3..p10 the shadow taps' offsets, p14 / p15 the
// layers' height scales and offsets. The math follows the stock records operation by operation
// (5a: the stock look, unchanged); the covers (5b, TerrainCover.glsl) change the layers' colour and
// gloss before the light, and add a wet shine, only while a mod turns them on; our point lights (6a,
// Lights.glsl) join the diffuse light, only while a mod turns them on.
#version 450
#extension GL_GOOGLE_include_directive : require
#if COVER_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif

#ifndef LAYERS
#define LAYERS 1
#endif
#ifndef SHADOW
#define SHADOW 0
#endif

layout(constant_id = 0) const bool kBigAlpha = false; // layer 0 takes what the others leave (else lerped in turn)
layout(constant_id = 1) const bool kUnlit    = false; // layers may opt out of the diffuse light (p1 per layer)
layout(constant_id = 2) const bool kNineTaps = false; // shadow mode 3: the first cascade with nine taps, the cascades chained
layout(constant_id = 3) const bool kUnderlay = false; // the lowered underlay of a raised chunk: not the covers' surface

layout(location = 0) in vec4 color0;   // diffuse light
layout(location = 1) in vec4 color1;   // the sun's highlight
layout(location = 2) in vec4 texcoord0;
layout(location = 3) in vec4 texcoord1;
layout(location = 4) in vec4 texcoord2;
layout(location = 5) in vec4 texcoord3;
layout(location = 6) in vec4 texcoord4;
layout(location = 7) in vec4 texcoord5;
layout(location = 8) in vec4 texcoord6;
layout(location = 10) in float fog0;

layout(location = 0) out vec4 oC0;

layout(set = 0, binding = 0) uniform sampler2D s0;
#if LAYERS >= 2
layout(set = 0, binding = 1) uniform sampler2D s1;
#endif
#if LAYERS >= 3
layout(set = 0, binding = 2) uniform sampler2D s2;
#endif
#if LAYERS >= 4
layout(set = 0, binding = 3) uniform sampler2D s3;
#endif
layout(set = 0, binding = LAYERS) uniform sampler2D sAlpha;
#if SHADOW >= 1
layout(set = 0, binding = 5) uniform sampler2D s5;
#endif
#if SHADOW >= 2
layout(set = 0, binding = 6) uniform sampler2D s6;
layout(set = 0, binding = 7) uniform sampler2D s7;
layout(set = 0, binding = 8) uniform sampler2D s8;
#endif
// SXC: the layers' height textures (`_h`, its alpha the height; white where a layer has none).
layout(set = 0, binding = 9) uniform sampler2D sH0;
#if LAYERS >= 2
layout(set = 0, binding = 10) uniform sampler2D sH1;
#endif
#if LAYERS >= 3
layout(set = 0, binding = 11) uniform sampler2D sH2;
#endif
#if LAYERS >= 4
layout(set = 0, binding = 12) uniform sampler2D sH3;
#endif

layout(std140, set = 1, binding = 0) uniform Regs {
    vec4 p[16];
    vec4 misc;   // misc.x = alphaRef / 255 (0 = off)
    vec4 v[64];
} regs;

#include "TerrainCover.glsl"
#include "Lights.glsl"

// Coordinate k (the vertex shader's packing): one per varying, or two per varying with the cascades.
vec2 Coord(int k)
{
    vec4 tc[5] = vec4[5](texcoord0, texcoord1, texcoord2, texcoord3, texcoord4);

    if (SHADOW == 0 || k == 0)
    {
        return tc[k].xy;
    }

    return (k & 1) == 1 ? tc[(k + 1) / 2].xy : tc[k / 2].wz;
}

// A layer's colour; with unlit layers, the diffuse light applied here per layer unless p1 says unlit.
vec4 Layer(vec4 c, float lighting, vec3 twice)
{
    if (kUnlit)
    {
        vec3 lit = c.xyz * twice;
        c.xyz = abs(lighting) <= 0.0 ? c.xyz : lit;
    }

    return c;
}

#if SHADOW >= 1
// One cascade's lit share: the centre and the taps (offsets p3, p5, p7, p9; or p3..p10 with nine),
// each 1 where the shadow map's depth is at or beyond the pixel's, summed.
float Taps(sampler2D map, vec2 coord, float depth, bool nine)
{
    vec2 uv = coord * 0.5 + 0.5;
    float sum = textureLod(map, uv, 0.0).x - depth >= 0.0 ? 1.0 : 0.0;

    for (int i = 3; i <= 10; i++)
    {
        if (!nine && (i & 1) == 0)
        {
            continue;
        }

        sum += textureLod(map, uv + regs.p[i].xy, 0.0).x - depth >= 0.0 ? 1.0 : 0.0;
    }

    return sum;
}
#endif

// SXC: the layers' coordinates as the layers and their height textures are read: Coord(k) moved by the
// parallax (Parallax), read with the gradients of the unmoved ones (the moved ones jump where the
// stepped ray does: mip levels flickered along the stones' edges otherwise). Set at main's start.
vec2 gUV[4];
vec2 gDx[4];
vec2 gDy[4];

vec4 Tex(sampler2D s, int k)
{
    return textureGrad(s, gUV[k], gDx[k], gDy[k]);
}

// SXC: parallax (TerrainTextureParams' ParallaxDepth in v59 per layer: yards from the height texture's
// top, white, to its bottom, black; all zero: none). Near the camera the view ray is stepped through the
// height map, so stones hide the gaps behind them; farther out one offset; beyond, none.
const float kParallaxNear = 15.0;   // yd: stepped within
const float kParallaxFar  = 35.0;   // yd: the offset faded out by

bool ParallaxOn()
{
    return any(notEqual(regs.v[59], vec4(0.0)));
}

// SXC: covers and the pattern (parallax and bumps). Untouched, a cover buries it: thin snow still shows
// it, then it fades as the cover deepens, gone at v61.x ([Terrain] PatternBuried, default 2) times a
// layer's relief (its parallax depth or normal strength: ~5 cm cobbles by ~10 cm). In a trail the cover
// is pressed flat (no pattern on it), down to what's left of it: where the stones' tops rise above that,
// they poke through (the ground with its pattern shows there, the pressed cover stays in the gaps).
float gCoverLeft = 0.0;   // yd: the cover's height here after the trails
float gInTrail = 0.0;     // how much this spot is a trail (its cover pressed down)
const float kPokeEdge = 0.005;   // yd: the soft edge where a stone's top meets the pressed cover

vec4 Relief()
{
    return max(regs.v[59], regs.v[58]);
}

// The stones' tops here (yd above the gaps): the layers' heights (at gUV) times their reliefs, by weight.
float StoneTop(vec4 w)
{
    vec4 h = vec4(Tex(sH0, 0).a, 0.0, 0.0, 0.0);
#if LAYERS >= 2
    h.y = Tex(sH1, 1).a;
#endif
#if LAYERS >= 3
    h.z = Tex(sH2, 2).a;
#endif
#if LAYERS >= 4
    h.w = Tex(sH3, 3).a;
#endif
    return dot(w * Relief(), h);
}

vec4 PatternKept(vec4 alpha)
{
    if (cover.enable.x < 0.5)
    {
        return vec4(1.0);
    }

    vec4 w = CoverLayerWeights(alpha, kBigAlpha);
    CoverSpot s = CoverEvaluate(coverWorld.xy, coverTint.w, coverWorld.w, w);
    float h = s.paintedHeight + s.snowHeight;
    float buried = regs.v[61].x > 0.0 ? regs.v[61].x : 2.0;
    vec4 kept = vec4(1.0) - smoothstep(vec4(0.0), max(Relief(), vec4(1e-4)) * buried, vec4(h));

    if (h > 0.0)
    {
        gCoverLeft = max(CoverTrailed(coverWorld.xy, coverTint.w, h, s), 0.0);
        gInTrail = smoothstep(0.05, 0.3, (h - gCoverLeft) / h);
        float poke = smoothstep(gCoverLeft - kPokeEdge, gCoverLeft + kPokeEdge, StoneTop(w));
        kept = mix(kept, vec4(poke), gInTrail);
    }

    return kept;
}

// Layer k's coordinates moved for a view direction (world, toward the eye) over a surface of normal n.
// `smoothing` (ParallaxSmooth, v60): the height is read that many mip levels blurrier (noise in a height
// map made for blending turned into bulbs on the stones).
vec2 Parallax(sampler2D h, int k, float depth, float smoothing, vec3 view, vec3 n, float distance)
{
    vec2 uv = gUV[k];

    if (depth == 0.0 || distance >= kParallaxFar)
    {
        return uv;
    }

    float blur = exp2(max(smoothing, 0.0));
    vec2 gx = gDx[k] * blur, gy = gDy[k] * blur;

    // Down into the surface the ray moves along it by -view's part along it / view's part across it;
    // the layer's coordinates run u along world y and v along world x, at v18+k per yard.
    vec3 t = normalize(vec3(1.0, 0.0, 0.0) - n * n.x);
    vec3 b = normalize(vec3(0.0, 1.0, 0.0) - n * n.y);
    float across = max(dot(view, n), 0.2);
    vec2 along = -vec2(dot(view, t), dot(view, b)) / across;
    vec4 perYard = regs.v[18 + k];
    vec2 full = vec2(along.y * perYard.y, along.x * perYard.x) * depth;   // the move at the full depth

    if (distance > kParallaxNear)
    {
        float fade = 1.0 - smoothstep(kParallaxNear, kParallaxFar, distance);
        return uv + full * (1.0 - textureGrad(h, uv, gx, gy).a) * fade;
    }

    // Stepped: down until the ray is deeper than the surface there (1 - height).
    int steps = int(mix(8.0, 24.0, 1.0 - across));
    float step = 1.0 / float(steps);
    float ray = 0.0;
    float surface = 1.0 - textureGrad(h, uv, gx, gy).a;

    for (int i = 0; i < 24 && ray < surface; i++)
    {
        ray += step;
        surface = 1.0 - textureGrad(h, uv + full * ray, gx, gy).a;
    }

    // Then halving the last step around the crossing: a straight line between two steps left ripples on
    // the stones' tops at larger depths (the steps showing).
    float above = max(ray - step, 0.0), below = ray;

    for (int i = 0; i < 5; i++)
    {
        float middle = (above + below) * 0.5;

        if (middle < 1.0 - textureGrad(h, uv + full * middle, gx, gy).a)
        {
            above = middle;
        }
        else
        {
            below = middle;
        }
    }

    return uv + full * ((above + below) * 0.5);
}

// SXC: height blending (retail since MoP). p14 each layer's height scale, p15 its offset - 1
// (TerrainTextureParams; retail's scale 0 and offset 1 everywhere, all zero here, turn it off and the
// stock blend stays: a draw that doesn't set them blends as before).
bool HeightsOn()
{
    return LAYERS >= 2 && (any(notEqual(regs.p[14], vec4(0.0))) || any(notEqual(regs.p[15], vec4(0.0))));
}

// The layers' weights after their heights, as retail blends them: each weight times its layer's height
// there (scaled and offset), the ones well below the strongest faded out, normalised. Where two layers
// meet, the higher texture (stones in sand) shows instead of a soft fade.
vec4 HeightWeights(vec4 w)
{
    vec4 h = vec4(Tex(sH0, 0).a, 0.0, 0.0, 0.0);
#if LAYERS >= 2
    h.y = Tex(sH1, 1).a;
#endif
#if LAYERS >= 3
    h.z = Tex(sH2, 2).a;
#endif
#if LAYERS >= 4
    h.w = Tex(sH3, 3).a;
#endif
    vec4 pct = w * (h * regs.p[14] + regs.p[15] + 1.0);
    float strongest = max(max(pct.x, pct.y), max(pct.z, pct.w));
    pct *= 1.0 - clamp(strongest - pct, 0.0, 1.0);
    return pct / max(dot(pct, vec4(1.0)), 1e-5);
}

// SXC: bumps from the height textures (TerrainTextureParams' NormalStrength in v58 per layer: yards of
// relief over a texture's full height; all zero: none, the light as stock).
bool BumpsOn()
{
    return any(notEqual(regs.v[58], vec4(0.0)));
}

// A layer's height slope in world yards per yard (x, y), from its height texture's neighbours. Its
// coordinates run u along world y and v along world x, at v18+k per yard (TerrainRegs.glsl).
vec2 BumpSlope(sampler2D h, int k, float strength)
{
    if (strength == 0.0)
    {
        return vec2(0.0);
    }

    vec2 uv = gUV[k], gx = gDx[k], gy = gDy[k];
    vec2 t = 1.0 / vec2(textureSize(h, 0));
    float du = textureGrad(h, uv + vec2(t.x, 0.0), gx, gy).a - textureGrad(h, uv - vec2(t.x, 0.0), gx, gy).a;
    float dv = textureGrad(h, uv + vec2(0.0, t.y), gx, gy).a - textureGrad(h, uv - vec2(0.0, t.y), gx, gy).a;
    vec4 perYard = regs.v[18 + k];
    return strength * vec2(dv / (2.0 * t.y) * perYard.x, du / (2.0 * t.x) * perYard.y);
}

// The world normal tilted by the layers' slopes (strength: each layer's), mixed by their weights, along
// the surface.
vec3 BumpNormal(vec3 n, vec4 w, vec4 strength)
{
    vec2 g = BumpSlope(sH0, 0, strength.x) * w.x;
#if LAYERS >= 2
    g += BumpSlope(sH1, 1, strength.y) * w.y;
#endif
#if LAYERS >= 3
    g += BumpSlope(sH2, 2, strength.z) * w.z;
#endif
#if LAYERS >= 4
    g += BumpSlope(sH3, 3, strength.w) * w.w;
#endif
    vec3 slope = vec3(g, 0.0);
    return normalize(n - (slope - n * dot(n, slope)));
}

void main()
{
    // SXC: our point lights (step 6a) in the stock diffuse light. color0 is the stock light (clamped)
    // x the vertex colour's factor, so twice it is the light the layers get; the lamps join that light
    // (LightsCombine), and the factor stays: color0 x combined / stock.
    vec4 lit0 = color0;
    vec3 lamps = vec3(0.0);
    int lampCount = 0;

    // SXC: the layers' coordinates and their gradients (Tex), before anything branches.
    for (int k = 0; k < LAYERS; k++)
    {
        gUV[k] = Coord(k);
        gDx[k] = dFdx(gUV[k]);
        gDy[k] = dFdy(gUV[k]);
    }

    vec4 alpha = texture(sAlpha, Coord(LAYERS));

    // The normal back to world orientation (v0..v2: world x view, a rotation).
    vec3 nv = normalize(coverNormal.xyz);
    vec3 n = normalize(vec3(dot(regs.v[0].xyz, nv), dot(regs.v[1].xyz, nv), dot(regs.v[2].xyz, nv)));

    // SXC: how much of each layer's pattern (parallax, bumps) the covers leave showing.
    bool parallax = ParallaxOn(), bumps = BumpsOn();
    vec4 kept = parallax || bumps ? PatternKept(alpha) : vec4(1.0);

    // SXC: the parallax moves the coordinates of the layers that have a depth and show here.
    if (parallax)
    {
        vec3 vv = -normalize(coverView.xyz);
        vec3 view = vec3(dot(regs.v[0].xyz, vv), dot(regs.v[1].xyz, vv), dot(regs.v[2].xyz, vv));
        float distance = length(coverView.xyz);
        vec4 shown = CoverLayerWeights(alpha, kBigAlpha);
        vec4 depth = regs.v[59] * kept;

        if (shown.x > 0.01) gUV[0] = Parallax(sH0, 0, depth.x, regs.v[60].x, view, n, distance);
#if LAYERS >= 2
        if (shown.y > 0.01) gUV[1] = Parallax(sH1, 1, depth.y, regs.v[60].y, view, n, distance);
#endif
#if LAYERS >= 3
        if (shown.z > 0.01) gUV[2] = Parallax(sH2, 2, depth.z, regs.v[60].z, view, n, distance);
#endif
#if LAYERS >= 4
        if (shown.w > 0.01) gUV[3] = Parallax(sH3, 3, depth.w, regs.v[60].w, view, n, distance);
#endif
    }

    // The layers' weights (SXC: by their heights where a layer has any).
    vec4 weights = CoverLayerWeights(alpha, kBigAlpha);
    bool heights = HeightsOn();

    if (heights)
    {
        weights = HeightWeights(weights);
    }

    // SXC: the normal tilted by the layers' bumps, and the sun's diffuse light redone per pixel for it
    // (the stock light comes per vertex: color0 changes by the difference, so a flat spot keeps it
    // exactly).
    if (bumps)
    {
        n = BumpNormal(n, weights, regs.v[58] * kept);
        vec3 nb = n.x * regs.v[0].xyz + n.y * regs.v[1].xyz + n.z * regs.v[2].xyz;
        float change = clamp(dot(regs.v[24].xyz, nb), 0.0, 1.0) - clamp(dot(regs.v[24].xyz, nv), 0.0, 1.0);
        lit0.xyz = max(lit0.xyz + regs.v[26].xyz * change * coverTint.xyz, vec3(0.0));
    }

    if (LightsOn())
    {
        lamps = LightsAt(coverWorld.xyz, n, lampCount);
        vec3 stock = min(lit0.xyz + lit0.xyz, vec3(1.0));
        lit0.xyz = lit0.xyz * LightsCombine(stock, lamps) / max(stock, vec3(1e-4));
    }

    vec3 twice = lit0.xyz + lit0.xyz;
    vec4 c = Layer(Tex(s0, 0), regs.p[1].x, twice);

    // SXC: the layers by their heights (HeightWeights), where a layer of the chunk has any.
    if (heights)
    {
        c = c * weights.x;
#if LAYERS >= 2
        c = Layer(Tex(s1, 1), regs.p[1].y, twice) * weights.y + c;
#endif
#if LAYERS >= 3
        c = Layer(Tex(s2, 2), regs.p[1].z, twice) * weights.z + c;
#endif
#if LAYERS >= 4
        c = Layer(Tex(s3, 3), regs.p[1].w, twice) * weights.w + c;
#endif
    }
    // The layers: lerped in turn by the alpha map's channels, or weighted with layer 0 taking the rest.
    else if (kBigAlpha)
    {
        float rest = 1.0 - clamp(dot(alpha.xyz, vec3(1.0)), 0.0, 1.0);
        c = c * rest;
#if LAYERS >= 2
        c = Layer(Tex(s1, 1), regs.p[1].y, twice) * alpha.x + c;
#endif
#if LAYERS >= 3
        c = Layer(Tex(s2, 2), regs.p[1].z, twice) * alpha.y + c;
#endif
#if LAYERS >= 4
        c = Layer(Tex(s3, 3), regs.p[1].w, twice) * alpha.z + c;
#endif
    }
    else
    {
#if LAYERS >= 2
        c = mix(c, Layer(Tex(s1, 1), regs.p[1].y, twice), alpha.x);
#endif
#if LAYERS >= 3
        c = mix(c, Layer(Tex(s2, 2), regs.p[1].z, twice), alpha.y);
#endif
#if LAYERS >= 4
        c = mix(c, Layer(Tex(s3, 3), regs.p[1].w, twice), alpha.z);
#endif
    }

    // SXC: the covers over the layers (painted, fallen snow, wetness); unchanged while they're off. In a
    // trail the stones that rise above the pressed cover poke through it (PatternKept).
    if (gInTrail > 0.0)
    {
        coverPoke = gInTrail * smoothstep(gCoverLeft - kPokeEdge, gCoverLeft + kPokeEdge, StoneTop(weights));
    }

    CoverResult cov = Covers(c, weights, twice, !kUnlit);
    c = cov.c;

    // SXC: the covers' surface here into the surface grid, and this pixel's depth for the projected
    // textures (step 5e; the underlay lies below the surface).
    if (!kUnderlay && cover.enable.x > 0.5)
    {
        CoverWriteSurface(coverWorld.xyz, coverTint.w);
        CoverWriteDepth(abs(coverWorld.z - coverTint.w) > 0.005);   // raised, or a print sunk below it
    }

    // The shadow: the alpha map's baked one, or the cascades'.
    float shadow = alpha.w;

#if SHADOW == 1
    // Mode 1: one cascade fading out toward its edge, never brighter than the baked shadow.
    float fade = clamp(max(abs(texcoord4.x), abs(texcoord4.y)) * -3.4482758 + 3.41379309, 0.0, 1.0);

    if (fade > 0.0)
    {
        float lit = Taps(s5, texcoord4.xy, texcoord4.z, false) * 0.2 - 1.0;
        shadow = min(fade * lit + 1.0, alpha.w);
    }
#elif SHADOW == 2
    // The far cascade: its coordinates in the w of the three, fading out toward its edge.
    float fade0 = clamp(max(abs(texcoord4.x), abs(texcoord4.y)) * -3.4482758 + 3.41379309, 0.0, 1.0);
    float rest;

    if (max(abs(texcoord5.x), abs(texcoord5.y)) < 1.0)
    {
        rest = Taps(s6, texcoord5.xy, texcoord5.z, false) * 0.2;
    }
    else if (max(abs(texcoord6.x), abs(texcoord6.y)) < 1.0)
    {
        rest = Taps(s7, texcoord6.xy, texcoord6.z, false) * 0.2;
    }
    else
    {
        vec2 far = vec2(texcoord4.w, texcoord5.w);
        float farFade = clamp(max(abs(far.x), abs(far.y)) * -11.1111107 + 11.0, 0.0, 1.0);
        rest = farFade * (Taps(s8, far, texcoord6.w, false) * 0.2 - 1.0) + 1.0;
    }

    if (kNineTaps)
    {
        // Mode 3: the nearest cascade that holds the pixel, the first one with nine taps, unfaded.
        shadow = fade0 > 0.0 ? Taps(s5, texcoord4.xy, texcoord4.z, true) * 0.111111112 : rest;
    }
    else
    {
        // Mode 2: the first cascade fading out, and the next one holding the pixel; the darker.
        float first = fade0 > 0.0 ? fade0 * (Taps(s5, texcoord4.xy, texcoord4.z, false) * 0.2 - 1.0) + 1.0 : 1.0;
        shadow = min(rest, first);
    }
#endif

    // Lit: the layers x (the shadow's 0.7..1) x twice the diffuse light, plus the highlight in the
    // layers' alpha (their gloss) where unshadowed. Unlit layers carry their light already.
    vec3 colour;
    float shade = shadow * 0.3 + 0.7;
    // SXC: the wet shine, on top of the stock highlight (zero while the covers are off or it's dry).
    vec3 shine = regs.v[27].xyz * CoverWetShine(cov.wet, c.w, color1.w);

    if (kUnlit)
    {
        vec3 highlight = c.www * color1.xyz + shine;
        highlight = shadow * highlight;
        colour = c.xyz * shade + highlight;
    }
    else
    {
        colour = c.xyz * shade;
        colour = colour * lit0.xyz;
        colour = colour + colour;
        vec3 highlight = c.www * color1.xyz + shine;
        colour = highlight * shadow + colour;
    }

    // SXC: the covers' debug views, unlit and unfogged (7, the decals', leaves the terrain as it is).
    if (cover.enable.x > 0.5 && cover.enable.y > 0.5 && cover.enable.y < 6.5)
    {
        oC0 = vec4(CoverDebug(cov, int(cover.enable.y + 0.5)), 1.0);
        return;
    }

    // SXC: the lights' debug views (unfogged): 1 our lights alone on grey ground, 2 how full the pixel's
    // cell is (red: lights reaching this pixel, green: lights the cell lists, both of what it holds).
    if (LightsOn() && LightsDebug() == 1)
    {
        oC0 = vec4(vec3(0.15) + lamps, 1.0);
        return;
    }

    if (LightsOn() && LightsDebug() == 2)
    {
        int first = LightCellFirst(coverWorld.xy);
        int perCell = int(lightInfo.x + 0.5);
        int listed = 0;

        for (int k = 0; k < kMaxLightsPerCell && k < perCell && cellLights[first + k] != 0xFFFFFFFFu; k++)
        {
            listed++;
        }

        oC0 = vec4(float(lampCount) / float(max(perCell, 1)), float(listed) / float(max(perCell, 1)), 0.1, 1.0);
        return;
    }

    // The fog: toward its colour by the vertex fog factor.
    colour = colour + -regs.p[2].xyz;
    oC0 = vec4(fog0 * colour + regs.p[2].xyz, 1.0);

    // The alpha test (GxRs_AlphaRef), as the translated modules do it.
    if (regs.misc.x > 0.0 && oC0.a < regs.misc.x) discard;
}
