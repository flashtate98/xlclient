// SXC: our terrain vertex shader (Vulkan/Pipelines: VertexProgram::Terrain), in place of the client's
// 128 "Terrain" vertex records. One source, its options as specialization constants; the device sets
// them from the record the engine binds ("Terrain:n", n = 64 point lights + 16 (layers - 1) + 8
// specular + 4 vertex colour + 2 env + shadow; CMapRenderChunk::GetVertexShader). The math is
// TerrainVertex.glsl's (5a: the stock look, unchanged).
//
// Every varying is written, so any of our terrain pixel modules pairs with it. Chunks that carry covers
// are drawn through the tessellation instead (TerrainPatch.vert, Terrain.tesc, Terrain.tese, step 5c),
// and once more with this shader, lowered (kSunk), under the raised surface: where two chunks' covers
// don't meet exactly at their border, the ground shows through the hairline instead of the sky. It sinks
// under the deepest base of the chunk's covers too (p13.z: mud lies below the terrain, and prints in it
// go down to its base; above that, the underlay hid them).
#version 450
#extension GL_GOOGLE_include_directive : require

layout(constant_id = 6) const bool kSunk = false; // the underlay of a tessellated chunk: a little lower

#include "TerrainVertex.glsl"

layout(location = 0) in vec4 inPosition;   // position0
layout(location = 1) in vec4 inNormal;     // normal0
layout(location = 2) in vec4 inColor;      // color0 (white from the constant stream when the vertex has none)

void main()
{
    vec4 position = inPosition;

    if (kSunk)
    {
        position.z -= 0.03 + max(regs.p[13].z, 0.0);
    }

    vec3 up = inNormal.xyz * inversesqrt(max(dot(inNormal.xyz, inNormal.xyz), 1e-12));
    TerrainVertex(position, inNormal, inColor, inPosition.z, up.z);
}
