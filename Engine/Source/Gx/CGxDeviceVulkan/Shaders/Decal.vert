// SXC: the client's projected textures on the raised covers (step 5e; Decal.frag): the decal's box,
// its back faces only (the device culls the front ones), so each pixel the box covers is shaded once
// with the camera outside or inside it. In the translated interface: the vertex registers carry the
// box's World x View in rows 0..3 and the native projection in rows 4..7 (row vectors), set by the
// device (CGxDeviceVulkanDraw.cpp).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"
#include "TerrainRegs.glsl"

layout(location = 0) in vec3 inPos;

layout(location = 0) out vec4 vView;   // the box's point in view space, and its clip w: the ray Decal.frag follows to the terrain

void main()
{
    vec4 view = regs.v[0] * inPos.x + regs.v[1] * inPos.y + regs.v[2] * inPos.z + regs.v[3];
    vec4 clip = regs.v[4] * view.x + regs.v[5] * view.y + regs.v[6] * view.z + regs.v[7];
    vView = vec4(view.xyz, clip.w);
    gl_Position = ToVulkanClip(clip);
}
