// SXC: the engine's constant registers as our terrain's vertex-side stages read them (Regs: the
// translated interface's set 1; SVertexShaderConstants, MapRenderChunk.h), and the layers' coordinates.
// Included by TerrainVertex.glsl (Terrain.vert, Terrain.tese) and Terrain.tesc.

layout(std140, set = 1, binding = 0) uniform Regs {
    vec4 p[16];
    vec4 misc;
    vec4 v[64];
} regs;

// Coordinate k (k < the layer count: a layer's, k == it: the alpha map's) of a world position, in the
// stock records' order: v13.. the offsets, v18.. the scales, v23 the chunk's position.
vec2 TerrainCoord(int k, vec2 world)
{
    vec2 r = -regs.v[23].yx + world.yx;
    return r * regs.v[18 + k].yx + regs.v[13 + k].yx;
}
