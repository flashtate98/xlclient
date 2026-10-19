// The push constants every pipeline of the Vulkan backend shares: 128 bytes, pushed whole by each draw.
//
//   xform   the vertex transform. For the fixed-function vertex program the engine's row-major
//           World * View * ProjNative, read as a column-major mat4 (so xform * v is the row-vector
//           product v * M). For the UI vertex program the four vertex constant registers c0..c3
//           (the engine's ViewProjNativeTranspose), one per column.
//   params  x : the alpha test reference as the engine's 8-bit value; 0 turns the test off
//           y : the texture stages with a texture bound, bit s for stage s (stages 0 and 1)
//           z : the colour ops, stage 0 (GxRs_ColorOp0) + 8 * stage 1 (GxRs_ColorOp1)
//           w : the alpha ops, stage 0 (GxRs_AlphaOp0) + 8 * stage 1 (GxRs_AlphaOp1)
//   tex     the fixed-function texture transforms of stages 0 and 1 (D3D9's D3DTS_TEXTUREn with
//           COUNT2, as the engine sets them from the Tex0 / Tex1 matrix stacks): for stage s the
//           2x2 part in tex[s] as (a0, b0, a1, b1) and the translation in tex[2] (stage 0 in xy,
//           stage 1 in zw), so u' = u * a0 + v * b0 + d0 and v' = u * a1 + v * b1 + d1. Identity
//           when the stage has no transform.
//   fog     SXC: D3D9's linear vertex fog for the fixed-function programs: x start, y end, z the fog
//           colour's RGB as one integer (0xRRGGBB, exact in a float), w 1 when fog is on.
layout(push_constant) uniform Push
{
    mat4 xform;
    vec4 params;
    vec4 tex[3];
    vec4 fog;
} pc;

// SXC: D3D9's linear fog factor for a vertex at view-space depth `depth` (the clip w of the
// perspective projection): 1 = no fog, 0 = all fog. 1 when fog is off.
float FogFactor(float depth)
{
    if (pc.fog.w < 0.5) return 1.0;
    float range = pc.fog.y - pc.fog.x;
    return range > 0.0 ? clamp((pc.fog.y - depth) / range, 0.0, 1.0) : (depth < pc.fog.x ? 1.0 : 0.0);
}

// SXC: blends a colour toward the fog colour by a factor from FogFactor().
vec3 ApplyFog(vec3 colour, float factor)
{
    uint c = uint(pc.fog.z + 0.5);
    vec3 fogColour = vec3(float((c >> 16u) & 255u), float((c >> 8u) & 255u), float(c & 255u)) / 255.0;
    return mix(fogColour, colour, factor);
}

// The D3D clip space the engine's matrices produce has y up; Vulkan's has y down.
vec4 ToVulkanClip(vec4 clip)
{
    clip.y = -clip.y;
    return clip;
}

// A texture coordinate through the fixed-function transform of stage `s` (0 or 1).
vec2 TexTransform(uint s, vec2 uv)
{
    vec4 m = pc.tex[s];
    vec2 d = s == 0u ? pc.tex[2].xy : pc.tex[2].zw;
    return vec2(uv.x * m.x + uv.y * m.y + d.x, uv.x * m.z + uv.y * m.w + d.y);
}
