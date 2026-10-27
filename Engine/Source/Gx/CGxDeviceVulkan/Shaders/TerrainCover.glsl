// SXC: surface covers in our terrain shader (Vulkan/Cover.hpp, SXC/Api/sxc_cover.h). Per spot the covers
// stack over the stock layers: the painted cover (each layer's entry where that layer is painted, from
// the chunk's alpha map), fallen snow on top, the ground showing through where they're thin; then
// wetness darkens what shows and makes it shine. Their height (step 5c) raises the surface where the
// terrain is tessellated (Terrain.tese); the look (Terrain.frag) follows the same rules.
//
// Included by Terrain.frag (everything) and Terrain.tese (COVER_DEPTH_ONLY: the height). The includer
// declares `regs` first and defines LAYERS.
//
// Pixel registers (set per chunk by SXC::Cover::BeforeChunkDraw): p11 each layer's painted entry, p12
// each layer's fallen-snow look (entry indices as floats, 0 = none), p13.x whether the chunk is raised,
// p13.y its liquid surface (world z; -100000 none), p13.z the deepest its painted covers' bases lie below
// the terrain (yd; the underlay sinks under it, Terrain.vert).
// Set 2: the parameters and entries, the near and far maps (fallen snow, painted snow's kept share,
// moisture above rest, the water surface), the cover textures. The look follows WXL's SurfaceCover
// (Shape, the noises, the slope limit, the melt).

#ifndef LAYERS
#define LAYERS 1
#endif

layout(std140, set = 2, binding = 0) uniform CoverBlock {
    vec4 enable;      // on, debug view, wet darkening, plain brightness
    vec4 nearMap;     // cell (yd), size, first i, first j
    vec4 farMap;
    vec4 outside;     // beyond the far map: fallen snow, kept share, moisture above rest; near map present
    vec4 tuning;      // thin depth (yd), 1 / texture tile (1/yd), depth scale, far map present
    vec4 tuning2;     // breakup scale, drift scale, fallen snow on, entries
    vec4 tess;        // the tessellation (Terrain.tesc): yd per segment per yd of distance, reach (yd), most segments per edge, on
    vec4 trailFine;   // trails (step 5d): cell (yd), size (0 none), first i, first j
    vec4 trailCoarse;
    vec4 trail;       // now (s), the coarse layer's persist (s), slur (yd of edge softening per yd of depth), on
    vec4 trail2;      // x the rims
    vec4 surface;     // the surface grid (step 5e): cell (yd), size (0 none), first i, first j
    vec4 texInfo[256]; // texture k + 1 at [k / 4][k % 4]: its slot x 4 (0 = none: drawn plain), + 2 its alpha is a gloss mask
    vec4 entries[127 * 5];
} cover;

layout(set = 2, binding = 1) uniform sampler2D sCoverNear;
layout(set = 2, binding = 2) uniform sampler2D sCoverFar;
layout(set = 2, binding = 3) uniform sampler2D sTrailFine;    // per cell: pressed to (world z; 1e6 untouched), when, rim strength, when
layout(set = 2, binding = 4) uniform sampler2D sTrailCoarse;

const uint kCoverPreventUnderwater = 1u;
const uint kCoverPreventOnLand     = 2u;
const uint kCoverUseVertexColor    = 4u;
const uint kCoverSnow              = 8u;
const float kShoreFade = 0.3;               // yd under the surface over which "prevent" covers taper

// --- noise (WXL's SurfaceCoverFill: the same hash, so a spot looks the same as it did there) ---------
float CoverHash(ivec2 p, uint seed)
{
    uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0;
}

float CoverValueNoise(vec2 p, uint seed)
{
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    ivec2 q = ivec2(i);
    float a = CoverHash(q, seed), b = CoverHash(q + ivec2(1, 0), seed);
    float c = CoverHash(q + ivec2(0, 1), seed), d = CoverHash(q + ivec2(1, 1), seed);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Two scales: broad drifts and small lumps (the depth); fine noise for ragged, patchy edges.
float CoverDrift(vec2 p) { return 0.65 * CoverValueNoise(p / 9.0, 0u) + 0.35 * CoverValueNoise(p / 2.5, 0u); }
float CoverEdgeNoise(vec2 p) { return 0.6 * CoverValueNoise(p / 1.6, 3u) + 0.4 * CoverValueNoise(p / 0.7, 4u); }

// A coverage made steeper by the breakup, its threshold moved by the edge noise: the transition breaks
// into patches; fully covered and bare stay so (WXL's Shape).
float CoverShape(float coverage, float breakup, float edge)
{
    float m = (coverage - 0.5) * (1.0 + 2.0 * breakup) + 0.5 + 1.5 * breakup * (edge - 0.5);
    m = clamp(m, 0.0, 1.0);
    return m * m * (3.0 - 2.0 * m);
}

// 1 on ground flatter than (max - fade) degrees, 0 steeper than max, smooth between.
float CoverSlope(float up, float maxSlope, float fade)
{
    float cosMax = cos(radians(clamp(maxSlope, 0.0, 90.0)));
    float cosFull = cos(radians(clamp(maxSlope - fade, 0.0, 90.0)));
    float t = cosFull > cosMax ? (up - cosMax) / (cosFull - cosMax) : (up >= cosMax ? 1.0 : 0.0);
    t = clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// --- the maps ----------------------------------------------------------------------------------------
const float kCoverNoWater = -100000.0;

// A map's value at a world position, and how much it holds there: 1 inside, fading out over its last
// 2 cells (the toroidal texture holds another place beyond them).
vec4 CoverMap(sampler2D map, vec4 m, vec2 xy, out float inside)
{
    vec2 c = xy / m.x;
    vec2 rel = c - m.zw;
    float edge = min(min(rel.x, rel.y), min(m.y - rel.x, m.y - rel.y));
    inside = clamp((edge - 0.5) * 0.5, 0.0, 1.0);
    return textureLod(map, c / m.y, 0.0);
}

// Fallen snow (yd), painted snow's kept share, moisture above rest, the water surface (world z).
vec4 CoverGround(vec2 xy)
{
    vec4 g = vec4(cover.outside.xyz, kCoverNoWater);
    float k;

    if (cover.tuning.w > 0.5)
    {
        vec4 f = CoverMap(sCoverFar, cover.farMap, xy, k);
        g.xyz = mix(g.xyz, f.xyz, k);
    }

    if (cover.outside.w > 0.5)
    {
        vec4 n = CoverMap(sCoverNear, cover.nearMap, xy, k);
        g.xyz = mix(g.xyz, n.xyz, k);
        g.w = k >= 1.0 ? n.w : kCoverNoWater;
    }

    return g;
}

// --- entries -----------------------------------------------------------------------------------------
vec4 CoverEntry(int e, int row) { return cover.entries[(e - 1) * 5 + row]; }

// Row 3 holds integers (sxc_cover_entry::texture, flags) and the z offset: the integers by their bits.
// Row 4: stiffness, rim, relax seconds (the trails).
int CoverEntryTexture(vec4 e3) { return floatBitsToInt(e3.x); }
uint CoverEntryFlags(vec4 e3) { return floatBitsToUint(e3.y); }

// A layer's weight after its water flags: "prevent underwater" thins out below the surface, "prevent on
// land" only exists there.
float CoverWater(uint flags, float under)
{
    float d = under / kShoreFade;
    float f = 1.0;
    if ((flags & kCoverPreventUnderwater) != 0u) f *= 1.0 - clamp(d, 0.0, 1.0);
    if ((flags & kCoverPreventOnLand) != 0u) f *= clamp(d, 0.0, 1.0);
    return f;
}

// The layers' weights as the colour blends them: lerped in turn, or (big alpha) layer 0 taking the rest.
vec4 CoverLayerWeights(vec4 alpha, bool bigAlpha)
{
    vec4 w = vec4(1.0, 0.0, 0.0, 0.0);

    if (bigAlpha)
    {
        w = vec4(1.0 - clamp(dot(alpha.xyz, vec3(1.0)), 0.0, 1.0), alpha.xyz);
    }
    else
    {
        for (int k = 1; k < LAYERS; k++)
        {
            w *= 1.0 - alpha[k - 1];
            w[k] = alpha[k - 1];
        }
    }

    for (int k = LAYERS; k < 4; k++) w[k] = 0.0;
    return w;
}

// --- one spot ----------------------------------------------------------------------------------------
// What lies at a spot, the look's shares and the heights, from the same rules.
struct CoverSpot
{
    vec4  g;             // the maps' values (CoverGround)
    float under;         // yd below the water surface
    float edge, drift;   // the noises
    float covered;       // the painted layers' weight (after their water, moisture and melt)
    float painted;       // the painted cover's share (shaped, slope-limited)
    float paintedHeight; // yd
    float below;         // yd the painted cover's base lies below the terrain (a negative z offset: mud)
    float rowWet;      // the painted rows' own wet look
    float snow;          // fallen snow's share (hides the ground from the thin depth on)
    float snowHeight;    // yd
    float snowWet;       // the snow looks' own wet look
    int   entry;         // the strongest painted entry
    // The trails (step 5d), from the covers that make the height, weighted by their part of it.
    float stiffness;     // 0 pressed to the ground .. 1 rigid
    float rimShare;      // the rim, share of the depth
    float relax;         // s: a trail fills back in over this long
};

// A painted layer's weight here: its paint, its water flags, its moisture need (mud: full 0.1 above its
// threshold) and, for painted snow, what's left of it (patchy as it melts).
float CoverLayerWeight(vec4 e0, vec4 e2, vec4 e3, float paint, vec4 g, float under, float drift, float edge)
{
    if (e0.x <= 0.0 || paint <= 0.0)
    {
        return 0.0;
    }

    uint flags = CoverEntryFlags(e3);
    float pw = paint * CoverWater(flags, under);

    if (e2.z <= 1.0)
    {
        pw *= clamp((e2.w + g.z - e2.z) * 10.0, 0.0, 1.0);
    }

    if ((flags & kCoverSnow) != 0u)
    {
        float noise = 0.6 * drift + 0.4 * edge;
        pw *= clamp(g.y * 1.6 - 0.6 * (1.0 - noise), 0.0, 1.0);
    }

    return pw;
}

// xy: world; groundZ: the terrain's height there; up: how much it faces up; w: the layers' weights.
CoverSpot CoverEvaluate(vec2 xy, float groundZ, float up, vec4 w)
{
    CoverSpot s;
    s.g = CoverGround(xy);

    // The water line: the near map's (each spot's own or nearest water), else the chunk's liquid surface
    // (p13.y: the lowest top of its liquids; kCoverNoWater without), so nothing settles on far sea beds.
    if (s.g.w <= kCoverNoWater + 1.0)
    {
        s.g.w = regs.p[13].y;
    }

    s.under = s.g.w - groundZ;
    s.edge = CoverEdgeNoise(xy);
    s.drift = CoverDrift(xy);
    s.covered = 0.0;
    s.painted = 0.0;
    s.paintedHeight = 0.0;
    s.below = 0.0;
    s.rowWet = 0.0;
    s.snow = 0.0;
    s.snowHeight = 0.0;
    s.snowWet = 0.0;
    s.entry = 0;
    s.stiffness = 0.0;
    s.rimShare = 0.3;
    s.relax = 30.0;
    int entries = int(cover.tuning2.w + 0.5);

    // The painted covers: each layer's entry by its weight here.
    ivec4 painted = ivec4(regs.p[11] + 0.5);
    float depth = 0.0, zOffset = 0.0, maxSlope = 0.0, slopeFade = 0.0, breakup = 0.0, driftAmount = 0.0, strongest = 0.0;
    vec3 paintedTrail = vec3(0.0), snowTrail = vec3(0.0, 0.3, 30.0);   // stiffness, rim, relax

    for (int l = 0; l < LAYERS; l++)
    {
        int e = painted[l];

        if (e <= 0 || e > entries || w[l] <= 0.0)
        {
            continue;
        }

        vec4 e0 = CoverEntry(e, 0), e2 = CoverEntry(e, 2), e3 = CoverEntry(e, 3);
        float pw = CoverLayerWeight(e0, e2, e3, w[l], s.g, s.under, s.drift, s.edge);

        if (pw <= 0.0)
        {
            continue;
        }

        s.covered += pw;
        depth += pw * e0.x;
        zOffset += pw * e3.z;
        maxSlope += pw * e0.y;
        slopeFade += pw * e0.z;
        driftAmount += pw * e0.w;
        breakup += pw * e2.x;
        s.rowWet += pw * e2.y;
        paintedTrail += pw * CoverEntry(e, 4).xyz;

        if (pw > strongest)
        {
            strongest = pw;
            s.entry = e;
        }
    }

    if (s.covered > 0.0)
    {
        float inv = 1.0 / s.covered;
        float b = clamp(breakup * inv * cover.tuning2.x, 0.0, 1.0);
        s.painted = CoverShape(min(s.covered, 1.0), b, s.edge) * CoverSlope(up, maxSlope * inv, slopeFade * inv);
        s.painted *= cover.tuning.z > 0.0 ? 1.0 : 0.0;
        s.rowWet *= inv;
        paintedTrail *= inv;
        // The painted depth varied by the drifts, its z offset (below the terrain: only what rises above
        // it shows), tapering out with its share.
        float driftF = max(1.0 + driftAmount * inv * cover.tuning2.y * (s.drift * 2.0 - 1.0), 0.0);
        s.paintedHeight = max((depth * inv * driftF * cover.tuning.z + zOffset * inv) * s.painted, 0.0);
        // The part below the terrain is still cover: the trails press into it (CoverTrailed).
        s.below = max(-zOffset * inv, 0.0) * s.painted;
    }

    // Fallen snow on top: its look from the layers' snow looks by their weights (built in where none),
    // its depth varied by the drifts, thinned on slopes, none below water; it hides what's below from
    // the thin depth on, patchy before.
    if (cover.tuning2.z > 0.5 && s.g.x > 0.0005)
    {
        ivec4 looks = ivec4(regs.p[12] + 0.5);
        float lw = 0.0, sMax = 0.0, sFade = 0.0, sBreak = 0.0, sDrift = 0.0, sWet = 0.0;
        vec3 sTrail = vec3(0.0);

        for (int l = 0; l < LAYERS; l++)
        {
            int e = looks[l];

            if (e <= 0 || e > entries || w[l] <= 0.0)
            {
                continue;
            }

            vec4 e0 = CoverEntry(e, 0), e2 = CoverEntry(e, 2);
            lw += w[l];
            sMax += w[l] * e0.y;
            sFade += w[l] * e0.z;
            sDrift += w[l] * e0.w;
            sBreak += w[l] * e2.x;
            sWet += w[l] * e2.y;
            sTrail += w[l] * CoverEntry(e, 4).xyz;
        }

        float maxS = 50.0, fadeS = 15.0, breakS = 0.5, driftS = 0.35;

        if (lw > 0.0)
        {
            float inv = 1.0 / lw;
            maxS = sMax * inv;
            fadeS = sFade * inv;
            breakS = sBreak * inv;
            driftS = sDrift * inv;
            s.snowWet = sWet * inv;
            snowTrail = sTrail * inv;
        }

        float driftF = max(1.0 + driftS * cover.tuning2.y * (s.drift * 2.0 - 1.0), 0.0);
        float dry = 1.0 - clamp(s.under / kShoreFade, 0.0, 1.0);
        float depth = s.g.x * cover.tuning.z * driftF * CoverSlope(up, maxS, fadeS) * dry;
        s.snow = CoverShape(clamp(depth / cover.tuning.x, 0.0, 1.0), clamp(breakS * cover.tuning2.x, 0.0, 1.0), s.edge);
        // Raised as far as it shows: snow too thin to show doesn't lift (or tilt) the ground under it.
        s.snowHeight = depth * s.snow;
    }

    // The trails' properties: by each cover's part of the depth, the painted cover's below the terrain
    // too (a row without them: soft, 0.3 rim, 30 s). Mud wholly below the terrain took the defaults, its
    // own rim and stiffness only once it rose above: weak rims, then a jump to the row's (2026-10-23).
    float hp = s.paintedHeight + s.below, hs = s.snowHeight;

    if (hp + hs > 0.0)
    {
        vec3 t = (paintedTrail * hp + snowTrail * hs) / (hp + hs);
        s.stiffness = clamp(t.x, 0.0, 1.0);
        s.rimShare = max(t.y, 0.0);
        s.relax = t.z > 0.5 ? t.z : 30.0;
    }

    return s;
}

// --- trails (step 5d) --------------------------------------------------------------------------------
// One trail layer at a world point, now: the ceiling the cover was pressed down to, risen since at
// `rise` yd/s (to `top` at most), and the rim's strength fading over `rimLife` s; bilinear between
// texels, each texel's own state first (their times differ).
void CoverTrailSample(sampler2D map, vec4 m, vec2 xy, float top, float rise, float rimLife, out float ceiling, out float rim)
{
    ceiling = top;
    rim = 0.0;
    int n = int(m.y + 0.5);

    if (n < 1)
    {
        return;
    }

    vec2 c = xy / m.x - 0.5;
    vec2 base = floor(c);
    vec2 f = c - base;
    ivec2 b = ivec2(base);
    ivec2 first = ivec2(m.zw);
    // The texel is the cell mod n, taken of a positive number: the cell plus whole windows enough for
    // anywhere on a map (17067 yd from its middle). A negative operand's % is left to the driver, and one
    // took it as unsigned: the fine layer (768, which doesn't divide 2^32) showed 256 cells off toward
    // -x, -y wherever the world's coordinates are negative (2026-10-22).
    int lift = n * (int(17100.0 / (m.x * float(n))) + 2);
    float cs[4], rs[4];

    for (int k = 0; k < 4; k++)
    {
        ivec2 q = b + ivec2(k & 1, k >> 1);
        ivec2 rel = q - first;
        cs[k] = top;
        rs[k] = 0.0;

        if (rel.x < 0 || rel.y < 0 || rel.x >= n || rel.y >= n)
        {
            continue;
        }

        ivec2 slot = ivec2(uvec2(q + lift) % uint(n));
        vec4 t = texelFetch(map, slot, 0);

        if (t.x < 0.5e6)
        {
            cs[k] = min(t.x + max(cover.trail.x - t.y, 0.0) * rise, top);
        }

        rs[k] = t.z * clamp(1.0 - max(cover.trail.x - t.w, 0.0) / rimLife, 0.0, 1.0);
    }

    ceiling = mix(mix(cs[0], cs[1], f.x), mix(cs[2], cs[3], f.x), f.y);
    rim = mix(mix(rs[0], rs[1], f.x), mix(rs[2], rs[3], f.x), f.y);
}

// The covers' height after the trails (yd above groundZ; below 0 in a print in mud) from what they'd be
// untouched (h, the spot s): pressed down to where feet went (the fine layer: crisp, filling back in over
// the cover's relax time; the coarse one: half as deep, softer, kept for the persist time), never below
// the cover's base (the terrain, or deeper where the cover sits below it; bare ground doesn't deform),
// held up by the cover's stiffness, softened at the edges the deeper the cover (slurred trails in deep
// snow), a rim pushed up beside.
//
// A cover whose base lies below the terrain (mud: a negative z offset, only its top shows) is carved as
// if the unit stood on that base: the trails' heights (where the bones went, the unit on the terrain)
// shifted down by it, so feet sink through the whole cover, not just the part above the terrain
// (Seyri, 2026-10-22: since the bones carve, mud showed no prints).
float CoverTrailed(vec2 xy, float groundZ, float h, CoverSpot s)
{
    float below = s.below;

    if (cover.trail.w < 0.5 || h + below <= 0.0)
    {
        return h;
    }

    float top = groundZ + h;
    float span = h + below;            // the whole cover, its base to its top
    float topB = top + below;          // its top where the bones' heights are taken
    float relax = max(s.relax, 0.5);
    float persist = max(cover.trail.y, relax);
    float slur = clamp(span * cover.trail.z, 0.0, 0.6);
    const vec2 taps[5] = vec2[5](vec2(0.0), vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
    int count = slur > 0.02 ? 5 : 1;
    float ceiling = 0.0, rim = 0.0;

    for (int k = 0; k < count; k++)
    {
        vec2 p = xy + taps[k] * slur;
        float cf, rf, cc, rc;
        CoverTrailSample(sTrailFine, cover.trailFine, p, topB, span / relax, relax, cf, rf);
        CoverTrailSample(sTrailCoarse, cover.trailCoarse, p, topB, span / persist, persist, cc, rc);
        ceiling += min(cf, topB - (topB - cc) * 0.5);
        rim += max(rf, rc * 0.5);
    }

    ceiling = ceiling / float(count) - below;
    rim /= float(count);
    float stiffness = clamp(s.stiffness, 0.0, 1.0);
    float pressed = max(groundZ - below, min(top, ceiling));
    pressed = mix(pressed, top, stiffness);
    return pressed - groundZ + rim * s.rimShare * cover.trail2.x * span * (1.0 - stiffness);
}

// How much cover lies here (yd): its height above the terrain (the painted cover, fallen snow on it) and
// what lies below it (the trails can press that far down; Terrain.tesc subdivides where either is).
float CoverHeight(vec2 xy, float groundZ, float up, vec4 w)
{
    if (cover.enable.x < 0.5)
    {
        return 0.0;
    }

    CoverSpot s = CoverEvaluate(xy, groundZ, up, w);
    return s.paintedHeight + s.snowHeight + (cover.trail.w > 0.5 ? s.below : 0.0);
}

#if !COVER_DEPTH_ONLY
// --- the look (Terrain.frag) -------------------------------------------------------------------------

// The cover textures: one array indexed per pixel (COVER_BINDLESS: a Vulkan 1.2 device with non-uniform
// indexing), else 16 slots the client gives to the textures in view.
#if COVER_BINDLESS
layout(set = 2, binding = 7) uniform sampler2D sCoverTex[1024];
#else
layout(set = 2, binding = 7) uniform sampler2D sCoverTex1;
layout(set = 2, binding = 8) uniform sampler2D sCoverTex2;
layout(set = 2, binding = 9) uniform sampler2D sCoverTex3;
layout(set = 2, binding = 10) uniform sampler2D sCoverTex4;
layout(set = 2, binding = 11) uniform sampler2D sCoverTex5;
layout(set = 2, binding = 12) uniform sampler2D sCoverTex6;
layout(set = 2, binding = 13) uniform sampler2D sCoverTex7;
layout(set = 2, binding = 14) uniform sampler2D sCoverTex8;
layout(set = 2, binding = 15) uniform sampler2D sCoverTex9;
layout(set = 2, binding = 16) uniform sampler2D sCoverTex10;
layout(set = 2, binding = 17) uniform sampler2D sCoverTex11;
layout(set = 2, binding = 18) uniform sampler2D sCoverTex12;
layout(set = 2, binding = 19) uniform sampler2D sCoverTex13;
layout(set = 2, binding = 20) uniform sampler2D sCoverTex14;
layout(set = 2, binding = 21) uniform sampler2D sCoverTex15;
layout(set = 2, binding = 22) uniform sampler2D sCoverTex16;
#endif

layout(location = 11) in vec4 coverWorld;   // world position (raised onto the covers), how much the terrain faces up
layout(location = 12) in vec4 coverView;    // view position
layout(location = 13) in vec4 coverNormal;  // view normal (not normalised)
layout(location = 14) in vec4 coverTint;    // the factor the vertex colour put into color0; w the terrain's own height

// The surface grid (step 5e): the covers' surface where the terrain is drawn, for the client to read
// back (units, the camera, effects at units' feet). Per cell: the surface's z, the cell's tag ((i &
// 4095) * 4096 + (j & 4095): the client takes only a cell's own), the bare ground's z, 1. Only cells in
// the window around the player (far terrain would wrap onto near cells).
layout(set = 2, binding = 5, rgba32f) uniform writeonly image2D sCoverSurface;

// The terrain's distance per pixel of the target (step 5e: the client's projected textures, Decal.frag,
// project onto the raised covers through it): the nearest terrain pixel's view depth (its clip w, the
// same under any projection's near and far and any viewport depth range) with its lowest bit taken for
// "the cover is raised here", by an atomic minimum (a positive float's bits order like it; overdraw keeps
// the nearest). Cleared to all ones each frame.
layout(set = 2, binding = 6, r32ui) uniform coherent uimage2D sCoverDepth;

void CoverWriteDepth(bool raised)
{
    ivec2 px = ivec2(gl_FragCoord.xy);

    if (all(lessThan(px, imageSize(sCoverDepth))) && gl_FragCoord.w > 0.0)
    {
        imageAtomicMin(sCoverDepth, px, (floatBitsToUint(1.0 / gl_FragCoord.w) & ~1u) | (raised ? 1u : 0u));
    }
}

// The slope (dz/dx, dz/dy in world yd) of a value over the surface at this pixel, from the screen
// derivatives of the world position and of the value; zero where the surface is seen edge-on.
vec2 CoverSlopeOf(vec3 dpx, vec3 dpy, float dvx, float dvy)
{
    float det = dpx.x * dpy.y - dpx.y * dpy.x;

    if (abs(det) < 1e-8)
    {
        return vec2(0.0);
    }

    vec2 g = vec2(dvx * dpy.y - dvy * dpx.y, dpx.x * dvy - dpy.x * dvx) / det;
    return clamp(g, vec2(-3.0), vec2(3.0));
}

void CoverWriteSurface(vec3 world, float groundZ)
{
    vec4 m = cover.surface;
    int n = int(m.y + 0.5);

    if (n < 1)
    {
        return;
    }

    // The values at the cell's middle, along the slopes here: every pixel in a cell writes nearly the
    // same (the pixel's own would differ across a 0.25 yd cell on a slope by up to a quarter yard, and
    // the last one to land changes from frame to frame: decals on the surface shook).
    vec3 dpx = dFdx(world), dpy = dFdy(world);
    vec2 surfaceSlope = CoverSlopeOf(dpx, dpy, dpx.z, dpy.z);
    vec2 groundSlope = CoverSlopeOf(dpx, dpy, dFdx(groundZ), dFdy(groundZ));
    ivec2 q = ivec2(floor(world.xy / m.x));
    ivec2 rel = q - ivec2(m.zw);

    if (rel.x < 0 || rel.y < 0 || rel.x >= n || rel.y >= n)
    {
        return;
    }

    vec2 toMiddle = (vec2(q) + 0.5) * m.x - world.xy;
    float surfaceZ = world.z + dot(surfaceSlope, toMiddle);
    float middleGroundZ = groundZ + dot(groundSlope, toMiddle);

    // Positive operands only (CoverTrailSample: a driver took % of a negative int as unsigned).
    int lift = n * (int(17100.0 / (m.x * float(n))) + 2);
    ivec2 slot = ivec2(uvec2(q + lift) % uint(n));
    float tag = float((q.x & 4095) * 4096 + (q.y & 4095));
    imageStore(sCoverSurface, slot, vec4(surfaceZ, tag, middleGroundZ, 1.0));
}

float CoverTexInfo(int t) { return cover.texInfo[(t - 1) >> 2][(t - 1) & 3]; }

// A texture's slot (0: none, drawn plain) and whether its alpha is a gloss mask.
int CoverTexSlot(int t) { return int(CoverTexInfo(t) * 0.25 + 0.01); }
bool CoverTexGloss(int t) { return mod(CoverTexInfo(t), 4.0) >= 2.0; }

// Explicit gradients: the cover textures are read in branches that differ from pixel to pixel.
vec4 CoverTexture(int slot, vec2 uv, vec2 dx, vec2 dy)
{
#if COVER_BINDLESS
    return textureGrad(sCoverTex[nonuniformEXT(slot - 1)], uv, dx, dy);
#else
    switch (slot)
    {
        case 1: return textureGrad(sCoverTex1, uv, dx, dy);
        case 2: return textureGrad(sCoverTex2, uv, dx, dy);
        case 3: return textureGrad(sCoverTex3, uv, dx, dy);
        case 4: return textureGrad(sCoverTex4, uv, dx, dy);
        case 5: return textureGrad(sCoverTex5, uv, dx, dy);
        case 6: return textureGrad(sCoverTex6, uv, dx, dy);
        case 7: return textureGrad(sCoverTex7, uv, dx, dy);
        case 8: return textureGrad(sCoverTex8, uv, dx, dy);
        case 9: return textureGrad(sCoverTex9, uv, dx, dy);
        case 10: return textureGrad(sCoverTex10, uv, dx, dy);
        case 11: return textureGrad(sCoverTex11, uv, dx, dy);
        case 12: return textureGrad(sCoverTex12, uv, dx, dy);
        case 13: return textureGrad(sCoverTex13, uv, dx, dy);
        case 14: return textureGrad(sCoverTex14, uv, dx, dy);
        case 15: return textureGrad(sCoverTex15, uv, dx, dy);
        case 16: return textureGrad(sCoverTex16, uv, dx, dy);
    }

    return vec4(1.0);
#endif
}

// An entry's colour (rgb, as a layer's albedo) and gloss (a): its texture or the plain colour, its tint,
// the vertex colour taken out again unless it uses it.
vec4 CoverLook(int e, vec2 uv, vec2 dx, vec2 dy)
{
    vec4 e1 = CoverEntry(e, 1), e3 = CoverEntry(e, 3);
    int t = CoverEntryTexture(e3);
    uint flags = CoverEntryFlags(e3);
    vec4 look = vec4(vec3(cover.enable.w), 0.0);
    int slot = t >= 1 && t <= 1024 ? CoverTexSlot(t) : 0;

    if (slot >= 1)
    {
        vec4 tex = CoverTexture(slot, uv, dx, dy);
        look = vec4(tex.rgb, CoverTexGloss(t) ? tex.a : 0.0);
    }

    look.rgb = mix(look.rgb, e1.rgb, clamp(e1.w, 0.0, 1.0));

    if ((flags & kCoverUseVertexColor) == 0u)
    {
        look.rgb *= min(vec3(0.5) / max(coverTint.rgb, vec3(0.02)), vec3(4.0));
    }

    return look;
}

struct CoverResult
{
    vec4  c;        // the colour (rgb) and gloss (a) that show
    float wet;      // 0..1
    float painted;  // the painted cover's share
    float snow;     // fallen snow's share
    float under;    // yd below the water surface
    int   entry;    // debug: the strongest painted entry
};

// SXC: the share of the covers the ground pokes through here (Terrain.frag sets it: stones rising above
// a trail's pressed cover); 0 = none.
float coverPoke = 0.0;

// c: the stock layers (rgb lit per layer when the chunk has unlit layers, a their gloss); w: the layers'
// weights; twice: twice the diffuse light (what unlit-layer chunks light each layer by).
CoverResult Covers(vec4 c, vec4 w, vec3 twice, bool litLayers)
{
    CoverResult r;
    r.c = c;
    r.wet = 0.0;
    r.painted = 0.0;
    r.snow = 0.0;
    r.under = -1.0e6;
    r.entry = 0;

    if (cover.enable.x < 0.5)
    {
        return r;
    }

    vec2 xy = coverWorld.xy;
    // Tiled like the terrain's layers (8 repeats per chunk, the same orientation), from the chunk's corner.
    vec2 uv = -(xy.yx - regs.v[23].yx) * cover.tuning.y;
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    CoverSpot s = CoverEvaluate(xy, coverTint.w, coverWorld.w, w);
    int entries = int(cover.tuning2.w + 0.5);
    r.under = s.under;
    r.painted = s.painted * (1.0 - coverPoke);
    r.snow = s.snow * (1.0 - coverPoke);
    r.entry = s.entry;

    // The painted look: each layer's entry by its weight here.
    vec4 paintedLook = vec4(0.0);

    if (s.covered > 0.0)
    {
        ivec4 painted = ivec4(regs.p[11] + 0.5);

        for (int l = 0; l < LAYERS; l++)
        {
            int e = painted[l];

            if (e <= 0 || e > entries || w[l] <= 0.0)
            {
                continue;
            }

            float pw = CoverLayerWeight(CoverEntry(e, 0), CoverEntry(e, 2), CoverEntry(e, 3), w[l], s.g, s.under, s.drift, s.edge);

            if (pw > 0.0)
            {
                paintedLook += pw * CoverLook(e, uv, dx, dy);
            }
        }

        paintedLook /= s.covered;
    }

    // Fallen snow's look: the layers' snow looks by their weights (layers sharing one count once).
    vec4 snowLook = vec4(vec3(cover.enable.w), 0.0);

    if (s.snow > 0.0)
    {
        ivec4 looks = ivec4(regs.p[12] + 0.5);
        float lw = 0.0;
        vec4 sLook = vec4(0.0);

        for (int l = 0; l < LAYERS; l++)
        {
            int e = looks[l];

            if (e <= 0 || e > entries || w[l] <= 0.0)
            {
                continue;
            }

            bool seen = false;

            for (int k = 0; k < l; k++)
            {
                seen = seen || (looks[k] == e && w[k] > 0.0);
            }

            if (seen)
            {
                continue;
            }

            float wl = 0.0;

            for (int k = l; k < LAYERS; k++)
            {
                wl += looks[k] == e ? w[k] : 0.0;
            }

            lw += wl;
            sLook += wl * CoverLook(e, uv, dx, dy);
        }

        if (lw > 0.0)
        {
            snowLook = sLook / lw;
        }
    }

    // Covers on chunks with unlit layers carry their light like the layers do.
    if (!litLayers)
    {
        paintedLook.rgb *= twice;
        snowLook.rgb *= twice;
    }

    // The stack: the ground, the painted cover over it, fallen snow over both.
    r.c = mix(r.c, paintedLook, r.painted);
    r.c = mix(r.c, snowLook, r.snow);

    // Wetness: the covers' own wet look where they show, plus the ground's moisture above its rest
    // (snow keeps its own: melt water soaks in below it).
    float rowWet = mix(s.rowWet, s.snowWet, r.snow);
    float shown = max(r.painted, r.snow);
    r.wet = clamp(rowWet * shown + s.g.z * (1.0 - r.snow), 0.0, 1.0);
    r.c.rgb *= 1.0 - cover.enable.z * r.wet;
    return r;
}

// The wet shine (WXL's cover: a stronger, tighter highlight): per pixel, the sun's Blinn-Phong with the
// exponent sharpened by the wetness, by the gloss made stronger by it. Nothing while the stock highlight
// is off.
float CoverWetShine(float wet, float gloss, float specularOn)
{
    if (wet <= 0.0 || specularOn < 0.5)
    {
        return 0.0;
    }

    vec3 n = normalize(coverNormal.xyz);
    vec3 v = normalize(-coverView.xyz);
    vec3 h = normalize(regs.v[24].xyz + v);
    float e = max(regs.v[27].w, 1.0) * (1.0 + 3.0 * wet);
    float mask = clamp(gloss * (1.0 + 2.0 * wet), 0.0, 1.0) - gloss;
    return pow(max(dot(n, h), 0.0), e) * max(mask, 0.0);
}

// Debug views (the look's debug): 1 what lies where (red bare, green painted, white fallen snow, blue
// water line), 2 wetness, 3 the painted entries (a colour each), 4 the maps' reach (green near, blue far,
// grey beyond), 5 the height (black flat .. white 0.5 yd; red where the chunk isn't raised), 6 the trails
// as the layers hold them here (red the fine layer, green the coarse one, by how far below the ground's
// height + 1 yd the carving went; blue the coarse one, magenta both; white / black lines the layers'
// window edges).
vec3 CoverDebug(CoverResult r, int view)
{
    if (view == 6)
    {
        vec2 xy = coverWorld.xy;
        float top = coverTint.w + 1.0;
        float cf, rf, cc, rc;
        CoverTrailSample(sTrailFine, cover.trailFine, xy, top, 0.0, 1.0e6, cf, rf);
        CoverTrailSample(sTrailCoarse, cover.trailCoarse, xy, top, 0.0, 1.0e6, cc, rc);
        vec3 colour = vec3(0.35) + vec3(0.65, -0.3, -0.35) * clamp(top - cf, 0.0, 1.0) + vec3(-0.35, -0.3, 0.65) * clamp(top - cc, 0.0, 1.0);
        vec4 layers[2] = vec4[2](cover.trailFine, cover.trailCoarse);
        vec3 lines[2] = vec3[2](vec3(1.0), vec3(0.0));

        for (int k = 0; k < 2; k++)
        {
            vec4 m = layers[k];

            if (m.y > 0.5)
            {
                vec2 lo = m.zw * m.x, hi = (m.zw + m.y) * m.x;
                vec2 d = min(abs(xy - lo), abs(xy - hi));
                bool inside = all(greaterThanEqual(xy, lo - 0.3)) && all(lessThanEqual(xy, hi + 0.3));
                colour = inside && min(d.x, d.y) < 0.3 ? lines[k] : colour;
            }
        }

        return colour;
    }

    if (view == 1)
    {
        vec3 colour = mix(vec3(0.35, 0.05, 0.05), vec3(0.1, 0.9, 0.1), r.painted);
        colour = mix(colour, vec3(1.0), r.snow);
        return r.under > 0.0 ? mix(colour, vec3(0.1, 0.3, 1.0), 0.5) : colour;
    }

    if (view == 2)
    {
        return vec3(0.1, 0.1, 0.1) + vec3(0.0, 0.3, 0.9) * r.wet;
    }

    if (view == 3)
    {
        if (r.entry == 0) return vec3(0.15);
        float hue = fract(float(r.entry) * 0.618034);
        vec3 k = clamp(abs(fract(vec3(hue) + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
        return mix(vec3(0.15), k, max(r.painted, 0.35));
    }

    if (view == 5)
    {
        float h = clamp((coverWorld.z - coverTint.w) / 0.5, 0.0, 1.0);
        return regs.p[13].x > 0.5 ? vec3(h) : vec3(0.4 + 0.6 * h, 0.05, 0.05);
    }

    float k;
    CoverMap(sCoverNear, cover.nearMap, coverWorld.xy, k);
    float near = cover.outside.w > 0.5 ? k : 0.0;
    CoverMap(sCoverFar, cover.farMap, coverWorld.xy, k);
    float far = cover.tuning.w > 0.5 ? k : 0.0;
    return mix(mix(vec3(0.3), vec3(0.1, 0.2, 1.0), far), vec3(0.1, 0.9, 0.1), near);
}
#endif
