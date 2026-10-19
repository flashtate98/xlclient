// The fixed-function vertex program: the position through the device's World * View * ProjNative,
// the vertex colour forwarded, texture coordinate sets 0 and 1 through their stages' texture
// transforms, as the engine's fixed-function pipeline would (no lighting, no texgen). SXC: plus
// D3D9's linear vertex fog factor, which Ff.frag applies.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;       // D3DCOLOR, fetched as r, g, b, a; opaque white when the vertex has none
layout(location = 2) in vec2 inTexCoord0;   // texcoord set 0; (0, 0) when the vertex has none
layout(location = 3) in vec2 inTexCoord1;   // texcoord set 1; (0, 0) when the vertex has none

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vTexCoord0;
layout(location = 2) out vec2 vTexCoord1;
layout(location = 3) out float vFog;        // SXC: 1 = no fog, 0 = all fog

void main()
{
    vec4 clip   = pc.xform * vec4(inPos, 1.0);
    vFog        = FogFactor(clip.w);        // SXC: clip w = view-space depth, D3D's fog distance
    gl_Position = ToVulkanClip(clip);
    vColor      = inColor;
    vTexCoord0  = TexTransform(0u, inTexCoord0);
    vTexCoord1  = TexTransform(1u, inTexCoord1);
}
