// SXC: our water's vertex shader (Vulkan/Pipelines: VertexProgram::Water; SXC/Water draws it), in place of
// the stock water materials 1 and 3 when [Liquid] OurShader is on. In the translated interface, reading
// the registers the liquid code sets (Liquid::SetShaderConstants, as the stock vsLiquidWater reads them):
// c0..c3 the native projection, c13..c16 texture transform 1 (the depth coordinate's: SXC/Water sets its
// scale); and SXC/Water's own past them: c46..c49 the mesh's world transform, c50..c53 the view, c54 what
// the world transform leaves out of the world position (the camera's, when camera-relative). Pixel c5 is
// the camera's world position (Liquid::SetShaderTransforms).
//
// Step 3b: the surface moves with the FFT waves (WaterWaves.glsl's displacement, set 2 binding 4) at the
// vertex's world position, so neighbouring patches agree at their edges (WaterMove.glsl).
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

#include "WaterMove.glsl"

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

    // The surface's movement (WaterMove.glsl, shared with the swimmers' query).
    float ease;
    rel.xyz += SurfaceMove(world, regs.v[55].x > 0.5, nearCell, depthCoord.y, vLook[4], regs.p[5].xyz, regs.p[6].x, ease);
    vEase = ease;

    vec4 view = regs.v[50] * rel.x + regs.v[51] * rel.y + regs.v[52] * rel.z + regs.v[53] * rel.w;
    vec4 clip = regs.v[0] * view.x + regs.v[1] * view.y + regs.v[2] * view.z + regs.v[3] * view.w;
    vView = vec4(view.xyz, clip.w);
    vColor = inColor;
    vTint = (regs.v[13] * depthCoord.x + regs.v[14] * depthCoord.y + regs.v[16]).xy;
    gl_Position = ToVulkanClip(clip);
}
