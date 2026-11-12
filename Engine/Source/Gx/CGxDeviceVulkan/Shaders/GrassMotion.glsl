// SXC: the grass in the wind and parting around characters (port plan step 7b; WXL's grass motion,
// features/GrassMotion), in the instanced grass variant of the DetailDoodad vertex modules (the
// translator inlines this file; Tools/VulkanShaders). A blade bends in world space before the stock
// transform: by how far up the blade a vertex is (its texture V between the plant's root and tip, a stiff
// base, squared), with the wind (a grid of samples around the player, bilinear), a flutter that grows in
// gusts, and pushed away from the characters near it.
//
// The rows the client uploads from c64 on (SXC/Grass, from what the mod sets through sxc.grass):
//   c64        time (s), anchor, 1 / (1 - anchor), amplitude (yd at wind strength 1)
//   c65        flutter (yd), flutter speed (rad/s), push strength (yd), on (1)
//   c66        1 / grid spacing (x, y), the grid's offset (x, y) in cells
//   c67        the player's position, the actor range squared
//   c68..c131  the 8 x 8 wind grid: wind x and y (x strength), gust, 0
//   c132..c147 the actors: position, push radius (0: none)
// Per plant (location 5): root V, 1 / (tip V - root V), wind scale, push scale (0: none).

layout(location = 5) in vec4 sxcPlantMotion;

// The ground marks' wind factor for this plant (GrassMarks.glsl sets it before the bend; frozen: stiff).
float sxcMarkWind = 1.0;

const int kSxcGrassHeader = 64;
const int kSxcGrassGrid = 68;
const int kSxcGrassActors = 132;
const int kSxcGrassActorCount = 16;

vec3 SxcGrassWind(vec2 xy)
{
    vec2 g = clamp(xy * regs.v[kSxcGrassHeader + 2].xy - regs.v[kSxcGrassHeader + 2].zw, vec2(0.0), vec2(6.999));
    vec2 f = fract(g);
    ivec2 c = ivec2(g - f);
    int i = kSxcGrassGrid + c.y * 8 + c.x;
    vec4 w = mix(mix(regs.v[i], regs.v[i + 1], f.x), mix(regs.v[i + 8], regs.v[i + 9], f.x), f.y);
    return w.xyz;
}

vec3 SxcGrassBend(vec3 p, float texV)
{
    vec4 h0 = regs.v[kSxcGrassHeader];
    vec4 h1 = regs.v[kSxcGrassHeader + 1];

    if (h1.w < 0.5)
    {
        return p;
    }

    float bend = clamp((texV - sxcPlantMotion.x) * sxcPlantMotion.y, 0.0, 1.0);
    bend = clamp((bend - h0.y) * h0.z, 0.0, 1.0);
    bend *= bend;

    if (bend <= 0.0)
    {
        return p;
    }

    // Wind, with a per-blade flutter (its phase from the position) that grows in gusts.
    vec3 w = SxcGrassWind(p.xy);
    float a = h0.x * h1.y + p.x * 0.61 + p.y * 0.37;
    a = fract(a * 0.15915494 + 0.5) * 6.2831853 - 3.1415927;
    float sway = (w.z + 0.3) * h1.x * sin(a) + h0.w;
    vec2 offset = w.xy * sway * sxcPlantMotion.z * sxcMarkWind;

    // Parting: each character near the player pushes blades away within its radius, near its height.
    vec4 h3 = regs.v[kSxcGrassHeader + 3];
    vec2 fromPlayer = p.xy - h3.xy;

    if (sxcPlantMotion.w > 0.0 && dot(fromPlayer, fromPlayer) < h3.w)
    {
        vec2 push = vec2(0.0);

        for (int k = 0; k < kSxcGrassActorCount; k++)
        {
            vec4 actor = regs.v[kSxcGrassActors + k];

            if (actor.w <= 0.0)
            {
                continue;
            }

            vec2 d = p.xy - actor.xy;
            float len = sqrt(dot(d, d) + 0.001);
            float fall = max(1.0 - len / actor.w, 0.0) * clamp(3.0 - abs(p.z - actor.z), 0.0, 1.0);
            push += d * (fall / len);
        }

        offset += push * h1.z * sxcPlantMotion.w * sxcMarkWind;   // frozen plants give way less
    }

    p.xy += offset * bend;
    return p;
}
