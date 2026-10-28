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
// every 4 yd: it carries the long waves (cascade 0, half of cascade 1 near the camera); the short ones stay
// in the shading. How far: the wave height and the Look panel's "Surface movement" (WaterScene.waveLook.w).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"
#include "TerrainRegs.glsl"
#include "WaterScene.glsl"

layout(location = 0) in vec3 inPos;     // position0
layout(location = 1) in vec4 inColor;   // color0: a WMO interior's liquid colour (white when the vertex has none)
layout(location = 2) in vec2 inDepth;   // texcoord1: the depth byte through the depth table (y)

layout(location = 0) out vec4 vView;    // the point in view space, and its clip w (the fog's depth)
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec2 vTint;    // where the tint gradient is read (Water.frag's s0)

layout(set = 2, binding = 4) uniform sampler2DArray waveDisp;   // per cascade: dx, dy, height (yd)

void main()
{
    vec4 rel = regs.v[46] * inPos.x + regs.v[47] * inPos.y + regs.v[48] * inPos.z + regs.v[49];
    vec3 world = rel.xyz + regs.v[54].xyz;

    if (scene.waveSizes.w > 0.5 && scene.waveLook.w > 0.0)
    {
        float distance = length(world - regs.p[5].xyz);
        float near1 = clamp(1.0 - distance / scene.waveLook.x, 0.0, 1.0);
        vec3 d = textureLod(waveDisp, vec3(world.xy / scene.waveSizes.x, 0.0), 0.0).xyz
               + textureLod(waveDisp, vec3(world.xy / scene.waveSizes.y, 1.0), 0.0).xyz * (0.5 * near1);
        // Eased out toward the shore by the vertex's own depth (the MH2O depth byte through the depth table:
        // 0 at the waterline, 1 a few yards deep): moved at the beach, the water's edge rose above the sand
        // and showed its steps (Seyri, 2026-10-29). The coast stays put and joins, open water moves fully.
        // WMO liquid carries no depth here, so it stays still. The liquid map's shore field does this
        // properly later.
        float deep = smoothstep(0.05, 0.6, inDepth.y);
        rel.xyz += d * (scene.waveLook.w * deep);
    }

    vec4 view = regs.v[50] * rel.x + regs.v[51] * rel.y + regs.v[52] * rel.z + regs.v[53] * rel.w;
    vec4 clip = regs.v[0] * view.x + regs.v[1] * view.y + regs.v[2] * view.z + regs.v[3] * view.w;
    vView = vec4(view.xyz, clip.w);
    vColor = inColor;
    vTint = (regs.v[13] * inDepth.x + regs.v[14] * inDepth.y + regs.v[16]).xy;
    gl_Position = ToVulkanClip(clip);
}
