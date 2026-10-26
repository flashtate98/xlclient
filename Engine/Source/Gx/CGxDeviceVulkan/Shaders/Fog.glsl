// SXC: the volumetric fog's shared block and medium (Vulkan/Fog.cpp, Fog.comp, Fog.frag).
//
// Two ways to draw it: per pixel (Fog.frag marches each pixel's own ray to its depth: thin layers on
// cliffs and around the feet stay sharp) or from the froxel volume (Fog.comp fills 160 x 90 columns of
// slices by the distance from the camera, Fog.frag reads its pixel's texel: cheap, soft, too coarse
// across the screen for layers thinner than a few yards).
//
// Positions are relative to `origin` (the camera), in the world's frame: +x north, +y west, +z up.
// Matrices are the engine's row-vector ones read as column-major (so M * v is the row-vector v * M).

layout(std140, set = 0, binding = 0) uniform FogParams
{
    mat4 invViewProj;  // clip (D3D: y up, z 0..1) -> camera-relative position
    vec4 origin;       // xyz the camera's world position, w seconds
    vec4 viewport;     // x, y, width, height in target pixels
    vec4 depth;        // the viewport's minDepth, maxDepth; the volume's near, far (yd)
    vec4 colour;       // rgb the fog's colour (lit), a the step length (yd)
    vec4 field;        // the near field: cell size (yd), cells per side, first cell i, first cell j
    vec4 noise;        // amount 0..1, tile (yd), how far the patches have drifted: x, y (yd, summed frame by frame)
    vec4 misc;         // density scale, debug view (0 off, 1 magenta, 2 near green / far blue), field present (1/0), haze density (per yd at the ground)
    vec4 haze;         // haze: base height (world z), thickness (yd); the field's colours present (1/0), x their brightness
    vec4 march;        // x 1 = march per pixel (0 = the froxel volume), y the step's growth with distance, z the most steps,
                       // w 1 = the pool layer's w says how much the near field shows per cell
    vec4 far;          // the far map: cell size (yd), cells per side, first cell i, first cell j
    vec4 maps;         // x the far map present (1/0), y its colours present (1/0), z the wake present (1/0), w the hand-over band (yd)
    vec4 wake;         // the wake map: world x, y of its corner, 1 / its extent (yd)
    vec4 lights;       // our lights in the fog: x strength (0 off), y forward glow (Henyey-Greenstein g), z the light volume's reach (yd), w the volume filled (1/0)
} fp;

layout(set = 0, binding = 1) uniform sampler2D fieldLight; // per cell: density (1/yd), layer floor (world z), thickness (yd; < 0 not ready), patchiness
layout(set = 0, binding = 2) uniform sampler2D fieldPool;  // per cell: density (1/yd), pool top (world z), soft top (yd), unused
layout(set = 0, binding = 6) uniform sampler2D fieldTint;  // per cell: the fog's colour (rgb), unused
layout(set = 0, binding = 7) uniform sampler2D farLayer;   // per far cell: density (1/yd), floor (world z), thickness (yd), unused
layout(set = 0, binding = 8) uniform sampler2D farTint;    // per far cell: colour (rgb), unused
layout(set = 0, binding = 9) uniform sampler2D wakeMap;    // per texel: push (< 0 at a rim), its top (world z), displacement x, y (yd)
layout(set = 0, binding = 11) uniform sampler3D lightVolume; // FogLights.comp: our lights' light per froxel (rgb), unused

const int kSlices = 128;
const int kLightSlices = 64; // the light volume's slices, near to fp.lights.z

// The light volume's slice edge e (0 .. kLightSlices) as a distance, and back.
float LightSliceEdge(float e)
{
    return fp.depth.z * pow(fp.lights.z / fp.depth.z, e / float(kLightSlices));
}

float LightSliceOf(float d)
{
    return log(max(d, fp.depth.z) / fp.depth.z) / log(fp.lights.z / fp.depth.z) * float(kLightSlices);
}

// Our lights' light scattered toward the camera at a distance along the ray through screen uv (per unit
// of the fog's scattering): added to the fog's own colour where it scatters.
vec3 FogLightIn(vec2 uv, float d)
{
    if (fp.lights.w < 0.5 || d >= fp.lights.z)
    {
        return vec3(0.0);
    }

    return textureLod(lightVolume, vec3(uv, LightSliceOf(d) / float(kLightSlices)), 0.0).rgb;
}

// The distance (yd) of slice edge e (0 .. kSlices).
float SliceEdge(float e)
{
    return fp.depth.z * pow(fp.depth.w / fp.depth.z, e / float(kSlices));
}

// The slice coordinate (0 .. kSlices) of a distance.
float SliceOf(float d)
{
    return log(max(d, fp.depth.z) / fp.depth.z) / log(fp.depth.w / fp.depth.z) * float(kSlices);
}

// A camera-relative position from clip x, y (D3D: y up) and depth z 0..1.
vec3 Unproject(vec2 ndc, float z)
{
    vec4 p = fp.invViewProj * vec4(ndc, z, 1.0);
    return p.xyz / p.w;
}

// --- value noise ------------------------------------------------------------------------------------
float Hash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float Noise(vec3 x)
{
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(Hash(i + vec3(0, 0, 0)), Hash(i + vec3(1, 0, 0)), f.x),
                   mix(Hash(i + vec3(0, 1, 0)), Hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(Hash(i + vec3(0, 0, 1)), Hash(i + vec3(1, 0, 1)), f.x),
                   mix(Hash(i + vec3(0, 1, 1)), Hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// 0..1, mean ~0.5: two octaves, the second smaller. `world` may be moved by the wake (the patches
// dragged along behind a unit).
float Patches(vec3 world)
{
    vec3 drift = vec3(fp.noise.z, fp.noise.w, 0.0);
    vec3 p = (world - drift) / fp.noise.y * 8.0;
    return 0.65 * Noise(p * vec3(1.0, 1.0, 2.0)) + 0.35 * Noise(p * vec3(2.3, 2.3, 4.0) + vec3(5.2, 1.3, 0.0) - drift.yxz * 0.05);
}

// --- the layers' profiles, averaged exactly over a step ---------------------------------------------
// A step's density is the layer's exact average over the step's height range above its floor (the
// floor taken at both ends), as WXL's march did: one point per step hit a thin layer or missed it,
// rings around the camera along the terrain's contours.

// The light layer's factor exp(-max(h, 0) / T) (h = height above its floor: full below it, falling off
// above) averaged over h from ha to hb. Its antiderivative: h below the floor, T (1 - exp(-h / T)) above.
float LayerIntegral(float h, float T) { return h < 0.0 ? h : T * (1.0 - exp(-h / T)); }

float LayerAverage(float ha, float hb, float T)
{
    if (abs(hb - ha) < 1e-3)
    {
        return exp(-max(0.5 * (ha + hb), 0.0) / T);
    }

    return (LayerIntegral(hb, T) - LayerIntegral(ha, T)) / (hb - ha);
}

// The pool's factor clamp(u, 0, 1) (u = (top - z) / soft + 0.5) averaged over u from ua to ub. Its
// antiderivative: 0, u^2 / 2, u - 1/2.
float RampIntegral(float u) { u = max(u, 0.0); return u < 1.0 ? 0.5 * u * u : u - 0.5; }

float RampAverage(float ua, float ub)
{
    if (abs(ub - ua) < 1e-3)
    {
        return clamp(0.5 * (ua + ub), 0.0, 1.0);
    }

    return (RampIntegral(ub) - RampIntegral(ua)) / (ub - ua);
}

// --- the near field at a point ------------------------------------------------------------------------

struct Field
{
    vec4 light;  // density, floor, thickness, patchiness (density x reach)
    vec4 pool;   // density, top, soft top (density x reach)
    vec3 tint;
    vec4 far;    // the far map: density (x its share: where the near field doesn't reach), floor, thickness
    vec3 farTint;
};

// How much of a toroidal window a world position sees: 1 inside, fading to 0 over its last `fade`
// cells (beyond it the texels hold other places). `map`: cell size, cells per side, first i, first j.
float WindowReach(vec2 world, vec4 map, float fade)
{
    vec2 cell = world / map.x - map.zw; // 0 .. cells inside the window
    vec2 edge = min(cell, vec2(map.y) - cell);
    return clamp(min(edge.x, edge.y) / fade, 0.0, 1.0);
}

Field SampleField(vec2 xy)
{
    Field f;
    f.light = vec4(0.0, 0.0, 1.0, 0.0);
    f.pool = vec4(0.0, -1.0e6, 1.0, 0.0);
    f.tint = fp.colour.rgb;
    f.far = vec4(0.0, 0.0, 1.0, 0.0);
    f.farTint = fp.colour.rgb;
    float reach = 0.0;

    if (fp.misc.z > 0.5)
    {
        // The near field fades into the far map over the hand-over band at its edge (WXL's 64 yd: over
        // 16 yd the seam between the two showed plainly).
        reach = WindowReach(xy, fp.field, max(fp.maps.w / fp.field.x, 1.0));

        if (reach > 0.0)
        {
            // The textures hold the field toroidally (cell i at i mod size): repeat addressing finds any cell.
            vec2 uv = xy / (fp.field.x * fp.field.y);
            f.light = textureLod(fieldLight, uv, 0.0);
            f.pool = textureLod(fieldPool, uv, 0.0);
            // A cell not ready yet (thickness < 0): the far map shows there instead; one that just
            // started fades in over it (the mod's per-cell share).
            reach *= clamp(f.light.z * 4.0, 0.0, 1.0);

            if (fp.march.w > 0.5)
            {
                reach *= clamp(f.pool.w, 0.0, 1.0);
            }
            f.light.x *= reach;
            f.pool.x *= reach;

            if (fp.haze.z > 0.5)
            {
                f.tint = textureLod(fieldTint, uv, 0.0).rgb * fp.haze.w;
            }
        }
    }

    // The far map, where the near field fades out.
    if (fp.maps.x > 0.5 && reach < 1.0)
    {
        float farReach = WindowReach(xy, fp.far, 2.0);

        if (farReach > 0.0)
        {
            vec2 uv = xy / (fp.far.x * fp.far.y);
            f.far = textureLod(farLayer, uv, 0.0);
            f.far.x *= farReach * (1.0 - reach);

            if (fp.maps.y > 0.5)
            {
                f.farTint = textureLod(farTint, uv, 0.0).rgb * fp.haze.w;
            }
        }
    }

    return f;
}

// The wake at a point (height z): how much of the fog stays (the push reaches up to its top, fading out
// over half a yard above), and how far the patches are dragged there.
float WakeKeep(vec3 p, out vec2 displace)
{
    displace = vec2(0.0);

    if (fp.maps.z < 0.5)
    {
        return 1.0;
    }

    vec4 w = textureLod(wakeMap, (p.xy - fp.wake.xy) * fp.wake.z, 0.0);
    float below = clamp((w.y - p.z) / 0.5 + 0.5, 0.0, 1.0);
    displace = w.zw * below;
    return max(1.0 - w.x * below, 0.0);
}

// The medium over the step from a to b (their field samples fa, fb): rgb the light it scatters per yd,
// a its extinction per yd, both the step's average. `farOnly`: only the far map (the long step past the
// march's reach).
vec4 StepMedium(vec3 a, vec3 b, Field fa, Field fb, bool farOnly)
{
    vec3 middle = 0.5 * (a + b);
    vec2 displace;
    float keep = farOnly ? 1.0 : WakeKeep(middle, displace);

    // The light layer: its density at both ends, the height above its floor going linearly from one
    // end's to the other's, averaged exactly; broken up by the noise at the step's middle. An end in a
    // cell not ready yet takes the other's floor and thickness.
    float light = 0.0;
    float da = fa.light.x, db = fb.light.x;

    if (!farOnly && (da > 0.0 || db > 0.0))
    {
        bool ra = fa.light.z > 0.0, rb = fb.light.z > 0.0;
        float floorA = ra || !rb ? fa.light.y : fb.light.y, floorB = rb || !ra ? fb.light.y : fa.light.y;
        float thickA = ra ? fa.light.z : fb.light.z, thickB = rb ? fb.light.z : fa.light.z;
        float thickness = max(0.5 * (thickA + thickB), 0.25);
        float profile = LayerAverage(a.z - floorA, b.z - floorB, thickness);
        float patchy = 0.5 * (fa.light.w + fb.light.w) * fp.noise.x;
        float n = Patches(middle - vec3(displace, 0.0));
        light = 0.5 * (da + db) * profile * clamp(1.0 - patchy + 2.0 * patchy * n, 0.0, 2.0);
    }

    // Pools: below their flat top, a soft band under it.
    float pool = 0.0;
    float pa = fa.pool.x, pb = fb.pool.x;

    if (!farOnly && (pa > 0.0 || pb > 0.0))
    {
        float ua = (fa.pool.y - a.z) / max(fa.pool.z, 0.05) + 0.5;
        float ub = (fb.pool.y - b.z) / max(fb.pool.z, 0.05) + 0.5;
        pool = 0.5 * (pa + pb) * RampAverage(ua, ub);
    }

    // The far map: each zone's typical layer on the chunks' low ground, the same profile. An end
    // without far fog takes the other's floor.
    float far = 0.0;
    float fda = fa.far.x, fdb = fb.far.x;

    if (fda > 0.0 || fdb > 0.0)
    {
        float floorA = fda > 0.0 ? fa.far.y : fb.far.y, floorB = fdb > 0.0 ? fb.far.y : fa.far.y;
        float thickness = max(max(fa.far.z, fb.far.z), 0.25);
        far = 0.5 * (fda + fdb) * LayerAverage(a.z - floorA, b.z - floorB, thickness);
    }

    // The haze: everywhere, thinning above its base (the same profile), in the look's colour.
    float haze = 0.0;

    if (!farOnly && fp.misc.w > 0.0)
    {
        haze = fp.misc.w * LayerAverage(a.z - fp.haze.x, b.z - fp.haze.x, max(fp.haze.y, 1.0));
    }

    // Units push the ground fog aside (not the haze).
    float field = (light + pool) * keep;
    far *= keep;
    float sigma = (field + far + haze) * fp.misc.x;
    vec3 tint = 0.5 * (fa.tint + fb.tint);
    vec3 farColour = fda + fdb > 0.0 ? (fda * fa.farTint + fdb * fb.farTint) / (fda + fdb) : fp.colour.rgb;
    vec3 hazeColour = fp.colour.rgb;

    // Debug view 2: where the fog comes from (WXL's view 4): near field green, far map blue, haze grey.
    if (fp.misc.y > 1.5)
    {
        tint = vec3(0.0, 1.0, 0.0);
        farColour = vec3(0.0, 0.3, 1.0);
        hazeColour = vec3(0.5);
    }

    float total = field + far + haze;
    vec3 colour = total > 0.0 ? (field * tint + far * farColour + haze * hazeColour) / total : hazeColour;
    return vec4(colour * sigma, sigma);
}
