#version 450
// SXC: one face of a light's cube shadow map (Vulkan/Shadows.cpp): the WMO, terrain and doodad
// triangles around the light, depth only. The face looks along axis k (x 0, y 1, z 2) with sign s; its
// picture plane is spanned by the next two axes (k + 1, k + 2), and Lights.glsl's LightShadow() reads it
// the same way. The texture coordinate is for the alpha-keyed parts (LightShadowAlpha.frag).

layout(location = 0) in vec3 inPos;   // world, relative to the light
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec2 outUv;

layout(push_constant) uniform ShadowFace {
    vec4 face;   // axis, sign, the depth's A and B (depth = A + B / distance along the axis)
} pc;

void main()
{
    int k = int(pc.face.x + 0.5);
    float m = pc.face.y * inPos[k];
    vec2 uv = k == 0 ? inPos.yz : (k == 1 ? inPos.zx : inPos.xy);
    gl_Position = vec4(uv, pc.face.z * m + pc.face.w, m);
    outUv = inUv;
}
