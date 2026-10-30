// SXC: our water's vertex shader (Vulkan/Pipelines: VertexProgram::Water; SXC/Water draws it), in place of
// the stock water materials 1 and 3 when [Liquid] OurShader is on. In the translated interface, reading
// the registers the liquid code sets (Liquid::SetShaderConstants, as the stock vsLiquidWater reads them):
// c0..c3 the native projection, c13..c16 texture transform 1 (the depth coordinate's: SXC/Water sets its
// scale); and SXC/Water's own past them: c46..c49 the mesh's world transform, c50..c53 the view, c54 what
// the world transform leaves out of the world position (the camera's, when camera-relative). Pixel c5 is
// the camera's world position (Liquid::SetShaderTransforms).
//
// Step 3b: the surface moves with the FFT waves (WaterWaves.glsl's displacement, set 2 binding 4) at the
// vertex's world position, so neighbouring patches agree at their edges. The stock mesh has a vertex about
// every 4 yd: it carries the long waves (cascade 0, smoothed, the sideways part small, capped, fading with
// distance); the short ones stay in the shading. How far: the wave height and this water's look (its
// movement and wave scale).
//
// The look (SXC/Water/Surface.h, five vec4s, out to Water.frag): (clarity clear, clarity murky, own colour,
// refraction), (reflectivity, reflection, Snell's window, glint), (shallow colour, Hue weight), (deep colour,
// Replace weight), (glint sharpness, wave scale, foam, movement). Terrain water (p14.z 1) blends it from the
// look grid between the four nearest chunk centres, so places fade into each other over a chunk; other
// water takes the draw's own, p9..p13.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"
#include "TerrainRegs.glsl"
#include "WaterScene.glsl"
#include "WaterShore.glsl"

layout(location = 0) in vec3 inPos;     // position0
layout(location = 1) in vec4 inColor;   // color0: a WMO interior's liquid colour (white when the vertex has none)
layout(location = 2) in vec2 inDepth;   // texcoord1: the depth byte through the depth table (y)

layout(location = 0) out vec4 vView;    // the point in view space, and its clip w (the fog's depth)
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec2 vTint;    // where the tint gradient is read (Water.frag's s0)
layout(location = 3) out float vEase;   // the movement's easing toward the shore (the debug view)
layout(location = 4) out vec4 vLook[5]; // this water's look here (above)
layout(location = 9) out float vNear;   // the near mesh: 1 where this vertex's cell is its own

layout(set = 2, binding = 4) uniform sampler2DArray waveDisp;   // per cascade: dx, dy, height (yd)

const float kHalfWorld = 32.0 * 533.33333;
const float kChunk = 533.33333 / 16.0;

uint LookSlot(ivec2 c)
{
    int b = c.y * 64 + c.x;
    uint word = scene.lookIndex[b >> 4][(b >> 2) & 3];
    return (word >> uint((b & 3) * 8)) & 0xFFu;
}

void LookAt(vec3 world)
{
    if (regs.p[14].z < 0.5 || scene.lookGrid.z < 0.5)
    {
        for (int m = 0; m < 5; m++)
        {
            vLook[m] = regs.p[9 + m];
        }

        return;
    }

    // Chunk centres sit at whole + 0.5 chunks; past the grid's edge its last chunks hold.
    vec2 g = vec2(kHalfWorld - world.y, kHalfWorld - world.x) / kChunk - scene.lookGrid.xy - 0.5;
    g = clamp(g, vec2(0.0), vec2(63.0));
    ivec2 i0 = min(ivec2(g), ivec2(62));
    vec2 f = g - vec2(i0);
    uint a = LookSlot(i0) * 5u, b = LookSlot(i0 + ivec2(1, 0)) * 5u;
    uint c = LookSlot(i0 + ivec2(0, 1)) * 5u, d = LookSlot(i0 + ivec2(1, 1)) * 5u;

    for (uint m = 0u; m < 5u; m++)
    {
        vLook[m] = mix(mix(scene.lookPalette[a + m], scene.lookPalette[b + m], f.x),
                       mix(scene.lookPalette[c + m], scene.lookPalette[d + m], f.x), f.y);
    }
}

void main()
{
    vec4 rel = regs.v[46] * inPos.x + regs.v[47] * inPos.y + regs.v[48] * inPos.z + regs.v[49];
    vec3 world = rel.xyz + regs.v[54].xyz;
    vec2 depthCoord = inDepth;
    vec4 nearCell = vec4(0.0);
    vNear = 1.0;

    // The near mesh (v55 = (1, its LiquidType)): a vertex per cell of the shore field's window, inPos.xy the
    // cell in the window; its height the cell's surface (land near the water takes the water's), so the
    // waterline is wherever the surface meets the ground. Cells it doesn't cover, or of another water, mark
    // their triangles dropped (vNear < 1 reaches every pixel of them).
    if (regs.v[55].x > 0.5)
    {
        ivec2 g = ivec2(scene.shoreGrid.xy) + ivec2(inPos.xy + 0.5);
        vec4 c = ShoreCell(g);
        nearCell = c;
        float cell = scene.shoreGrid.w;
        world = vec3(kShoreHalfWorld - (float(g.y) + 0.5) * cell, kShoreHalfWorld - (float(g.x) + 0.5) * cell, c.x);
        vNear = c.w > 0.5 && abs(c.z - regs.v[55].y) < 0.5 ? 1.0 : 0.0;
        depthCoord = vec2(0.0, max(c.w - 1.0, 0.0));   // the stock mesh's depth coordinate here (Shore.cpp)
        rel = vec4(world - regs.v[54].xyz, 1.0);
    }

    LookAt(world);

    // How freely the surface moves here: eased out toward the shore. By the shore field where it reaches
    // (still at the waterline and in water under a yard or so deep, free some 8 yd out); beyond it by the
    // vertex's own depth (the MH2O depth byte through the depth table: 0 at the waterline, 1 a few yards
    // deep). Moved at the beach, the water's edge rose above the sand and showed its steps (Seyri,
    // 2026-10-29). WMO liquid carries neither, so it stays still.
    float shoreWeight;
    vec4 shore = ShoreAt(world.xy, shoreWeight);
    float deep = mix(smoothstep(0.05, 0.6, depthCoord.y), smoothstep(0.5, 8.0, shore.x) * smoothstep(0.3, 2.0, shore.y), shoreWeight);
    float sideways = deep;

    // The near mesh reaches under the land, so its waves run on to the waterline and the ground cuts them
    // there (eased like the stock mesh, they went still before the sand: Seyri, 2026-10-30). A little calmer
    // in the shallows; sideways only out of them, so the mesh's edges stay over their cells.
    if (regs.v[55].x > 0.5)
    {
        float depth = nearCell.x - nearCell.y;
        deep = mix(0.6, 1.0, smoothstep(0.0, 4.0, depth));
        sideways = smoothstep(0.5, 3.0, depth);
    }

    vEase = deep;

    // The swash on the near mesh: the water near the waterline rises a little as each wave arrives and sinks
    // as it slides back, so the waterline itself runs up the beach and back (a flat beach far, a steep one
    // hardly: the ground decides where the raised surface meets it).
    if (regs.v[55].x > 0.5 && shoreWeight > 0.0)
    {
        float exposure = ShoreExposure(world.xy, shore);
        float runUp = scene.shoreLook.y * 1.5 * exposure * vLook[4].y * (1.0 - smoothstep(1.0, 6.0, abs(shore.x)));
        rel.z += runUp * ShoreSwash(ShoreArrival(world.xy, regs.p[6].x)) * shoreWeight;
    }

    if (scene.waveSizes.w > 0.5 && vLook[4].w > 0.0)
    {
        // Only what the mesh can hold, so the surface doesn't tear (Seyri, 2026-10-29: far-out waves at height
        // and movement 1 grew choppy, then split apart): cascade 0 alone, averaged over the vertex spacing
        // (shorter waves alias into spikes on a 4 yd mesh); the sideways part small (at full chop neighbouring
        // vertices passed each other and the triangles turned over); capped; and fading out with distance,
        // where the shading carries the waves alone.
        float distance = length(world - regs.p[5].xyz);
        float cascade0 = 1.0 / scene.waveSizes.x;
        const float kSpacing = 2.0;   // yd, half the stock mesh's
        vec3 d = (textureLod(waveDisp, vec3(world.xy * cascade0, 0.0), 0.0).xyz * 2.0
                + textureLod(waveDisp, vec3((world.xy + vec2(kSpacing, 0.0)) * cascade0, 0.0), 0.0).xyz
                + textureLod(waveDisp, vec3((world.xy - vec2(kSpacing, 0.0)) * cascade0, 0.0), 0.0).xyz
                + textureLod(waveDisp, vec3((world.xy + vec2(0.0, kSpacing)) * cascade0, 0.0), 0.0).xyz
                + textureLod(waveDisp, vec3((world.xy - vec2(0.0, kSpacing)) * cascade0, 0.0), 0.0).xyz) / 6.0;
        d.xy *= 0.3;
        d = clamp(d, vec3(-1.0), vec3(1.0));
        d *= 1.0 - smoothstep(150.0, 260.0, distance);
        // Eased out toward the shore (`deep`, above): the coast stays put and joins, open water moves fully.
        float scale = vLook[4].w * vLook[4].y;   // this water's movement and wave scale
        rel.xy += d.xy * (scale * sideways);
        rel.z += d.z * (scale * deep);
    }

    // Land the near mesh reaches under: the waves and the run-up may lift it over the ground near the
    // waterline, but not at the mesh's outer edge, which would stand out of the ground.
    if (regs.v[55].x > 0.5 && nearCell.x < nearCell.y)
    {
        float cap = nearCell.y - 0.05 - regs.v[54].z + 100.0 * (1.0 - smoothstep(2.0, 4.5, -shore.x));
        rel.z = min(rel.z, cap);
    }

    vec4 view = regs.v[50] * rel.x + regs.v[51] * rel.y + regs.v[52] * rel.z + regs.v[53] * rel.w;
    vec4 clip = regs.v[0] * view.x + regs.v[1] * view.y + regs.v[2] * view.z + regs.v[3] * view.w;
    vView = vec4(view.xyz, clip.w);
    vColor = inColor;
    vTint = (regs.v[13] * depthCoord.x + regs.v[14] * depthCoord.y + regs.v[16]).xy;
    gl_Position = ToVulkanClip(clip);
}
