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
    LookAt(world);

    // How freely the surface moves here: eased out toward the shore. By the shore field where it reaches
    // (still at the waterline and in water under a yard or so deep, free some 8 yd out); beyond it by the
    // vertex's own depth (the MH2O depth byte through the depth table: 0 at the waterline, 1 a few yards
    // deep). Moved at the beach, the water's edge rose above the sand and showed its steps (Seyri,
    // 2026-10-29). WMO liquid carries neither, so it stays still.
    float shoreWeight;
    vec4 shore = ShoreAt(world.xy, shoreWeight);
    float deep = mix(smoothstep(0.05, 0.6, inDepth.y), smoothstep(0.5, 8.0, shore.x) * smoothstep(0.3, 2.0, shore.y), shoreWeight);
    vEase = deep;

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
        rel.xyz += d * (vLook[4].w * deep * vLook[4].y);   // this water's movement and wave scale
    }

    vec4 view = regs.v[50] * rel.x + regs.v[51] * rel.y + regs.v[52] * rel.z + regs.v[53] * rel.w;
    vec4 clip = regs.v[0] * view.x + regs.v[1] * view.y + regs.v[2] * view.z + regs.v[3] * view.w;
    vView = vec4(view.xyz, clip.w);
    vColor = inColor;
    vTint = (regs.v[13] * inDepth.x + regs.v[14] * inDepth.y + regs.v[16]).xy;
    gl_Position = ToVulkanClip(clip);
}
