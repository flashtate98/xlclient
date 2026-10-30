// SXC: the shore field (SXC/Water/Shore.h; Vulkan/Water.cpp binding 5), shared by Water.vert and Water.frag:
// around the player, per cell of a sixteenth of a terrain chunk, the signed distance to the shore (yd, positive
// over water), the depth (surface minus ground, yd) and the direction to the shore (world x, y). Stored
// toroidally (a cell at global (gx, gy) at (gx & 255, gy & 255)), so it is read with a repeating sampler at the
// global cell coordinates; `weight` is 0 outside the field's window and fades in over its last 16 cells.
// Terrain liquid only: WMO water would read the terrain's shores, so it passes terrain = false (p14.w 0).
layout(set = 2, binding = 5) uniform sampler2D shoreMap;

const float kShoreHalfWorld = 32.0 * 533.33333;

vec4 ShoreAt(vec2 world, out float weight)
{
    weight = 0.0;

    if (scene.shoreGrid.z < 0.5 || regs.p[14].w < 0.5)
    {
        return vec4(64.0, 50.0, 0.0, 0.0);
    }

    vec2 g = vec2(kShoreHalfWorld - world.y, kShoreHalfWorld - world.x) / scene.shoreGrid.w;
    vec2 inside = g - scene.shoreGrid.xy;
    vec2 edge = min(inside, vec2(256.0) - inside);
    weight = clamp((min(edge.x, edge.y) - 2.0) / 14.0, 0.0, 1.0);
    return textureLod(shoreMap, g / 256.0, 0.0);
}
