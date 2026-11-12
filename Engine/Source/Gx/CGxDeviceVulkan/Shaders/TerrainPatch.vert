// SXC: our terrain's vertex shader for chunks raised onto their covers (step 5c): the vertex as it is,
// for the tessellation (Terrain.tesc, Terrain.tese), which does the vertex math at the raised points.
#version 450

layout(location = 0) in vec4 inPosition;   // position0 (world)
layout(location = 1) in vec4 inNormal;     // normal0 (world)
layout(location = 2) in vec4 inColor;      // color0 (white from the constant stream when the vertex has none)

layout(location = 0) out vec3 patchPosition;
layout(location = 1) out vec3 patchNormal;
layout(location = 2) out vec4 patchColor;

void main()
{
    patchPosition = inPosition.xyz;
    patchNormal = inNormal.xyz;
    patchColor = inColor;
}
