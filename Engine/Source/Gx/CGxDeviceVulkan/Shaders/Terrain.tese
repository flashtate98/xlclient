// SXC: a raised terrain chunk's points (step 5c): each point of the subdivided triangle raised by the
// covers' height there (TerrainCover.glsl: the painted cover from the chunk's alpha map and the layers'
// entries, fallen snow on it, the same rules as the look), its normal from the height around it, then
// the stock vertex math at the raised point (TerrainVertex.glsl): the light, the shadow cascades and
// the fog follow the covers' shape.
//
// Compiled once per layer count (LAYERS 1..4: the alpha map's binding is the layer count); the vertex
// options as specialization constants (0..5, TerrainVertex.glsl), big alpha (7) from the pixel record.
#version 450
#extension GL_GOOGLE_include_directive : require

#ifndef LAYERS
#define LAYERS 1
#endif
#define COVER_DEPTH_ONLY 1

// cw: Vulkan's tessellation domain has its origin upper-left (OpenGL's lower-left), which mirrors the
// generated triangles; "cw" here gives them the patch's own winding, so the terrain's culling keeps the
// top side (with ccw they showed only from below, 2026-10-21).
layout(triangles, fractional_odd_spacing, cw) in;

layout(constant_id = 7) const bool kBigAlpha = false; // layer 0 takes what the others leave (else lerped in turn)

#include "TerrainVertex.glsl"
#include "TerrainCover.glsl"

layout(set = 0, binding = LAYERS) uniform sampler2D sAlpha;

layout(location = 0) in vec3 tessPosition[];
layout(location = 1) in vec3 tessNormal[];
layout(location = 2) in vec4 tessColor[];

// yd between the points the height's slope is taken over (the drifts' smallest lumps are ~2.5 yd).
const float kSlopeStep = 0.25;
// yd of cover from which a point takes its own light rather than its triangle's stock light.
const float kOwnLightHeight = 0.05;

// The covers' height here, pressed down by the trails (step 5d).
float HeightAt(vec2 xy, float groundZ, float up)
{
    if (cover.enable.x < 0.5)
    {
        return 0.0;
    }

    vec4 alpha = textureLod(sAlpha, TerrainCoord(LAYERS, xy), 0.0);
    CoverSpot s = CoverEvaluate(xy, groundZ, up, CoverLayerWeights(alpha, kBigAlpha));
    return CoverTrailed(xy, groundZ, s.paintedHeight + s.snowHeight, s);
}

void main()
{
    vec3 b = gl_TessCoord;
    vec3 position = tessPosition[0] * b.x + tessPosition[1] * b.y + tessPosition[2] * b.z;
    vec3 normal = tessNormal[0] * b.x + tessNormal[1] * b.y + tessNormal[2] * b.z;
    vec4 colour = tessColor[0] * b.x + tessColor[1] * b.y + tessColor[2] * b.z;
    vec3 n = normal * inversesqrt(max(dot(normal, normal), 1e-12));

    // The height here and a step along x and y (the terrain's own slope carries the ground's height there).
    float h = HeightAt(position.xy, position.z, n.z);

    // The light's normal: the terrain's own, as interpolated (the stock light where the cover is flat or
    // absent: on bare rock nothing changes), tilted by the cover's slope where it has one. The surface
    // z = terrain + height has the normal n / n.z - (dh/dx, dh/dy, 0), scaled by n.z: n - n.z grad.
    vec3 surface = normal;

    // Where the cover has no height the point is the terrain itself: its normal stays (a bare point next
    // to cover kept a slope toward it before, and those single vertices shaded darker or lighter). On the
    // cover, its slope over a step to either side (centred: no lean toward +x or +y).
    if (cover.enable.x > 0.5 && n.z > 0.0 && h != 0.0)
    {
        vec2 slope = -n.xy / max(n.z, 0.05);   // the terrain's dz/dx, dz/dy
        vec2 dx = vec2(kSlopeStep, 0.0), dy = vec2(0.0, kSlopeStep);
        float hx1 = HeightAt(position.xy + dx, position.z + slope.x * kSlopeStep, n.z);
        float hx0 = HeightAt(position.xy - dx, position.z - slope.x * kSlopeStep, n.z);
        float hy1 = HeightAt(position.xy + dy, position.z + slope.y * kSlopeStep, n.z);
        float hy0 = HeightAt(position.xy - dy, position.z - slope.y * kSlopeStep, n.z);
        vec2 grad = vec2(hx1 - hx0, hy1 - hy0) / (2.0 * kSlopeStep);

        if (grad.x != 0.0 || grad.y != 0.0)
        {
            surface = normal - n.z * vec3(grad, 0.0) * length(normal);
        }
    }

    // The stock light and fog of the triangle: its corners' (as the flat terrain lights them), blended
    // across it (Gouraud). Lighting each new point itself differs where the light clamps (a steep face
    // turned from the sun stayed dark further along the edge: darkened vertices at sand's foot, seen
    // 2026-10-21), so bare ground takes the corners' light; the cover's own light (its drifts, its slope)
    // takes over as it gains height. The fog stays the corners' too: the cover moves a point by inches.
    vec4 c0 = vec4(0.0), c1 = vec4(0.0);
    float fog = 0.0;

    for (int k = 0; k < 3; k++)
    {
        vec4 cornerPos = TerrainViewPosition(vec4(tessPosition[k], 1.0));
        vec4 k0, k1;
        TerrainLight(cornerPos, TerrainViewNormal(vec4(tessNormal[k], 0.0)), tessColor[k], k0, k1);
        c0 += b[k] * k0;
        c1 += b[k] * k1;
        fog += b[k] * TerrainFog(cornerPos);
    }

    float stock = 1.0 - clamp(abs(h) / kOwnLightHeight, 0.0, 1.0);   // a print in mud sinks below the terrain
    TerrainVertexLit(vec4(position.x, position.y, position.z + h, 1.0), vec4(surface, 0.0), colour, position.z, n.z, stock, c0, c1, true, fog);
}
