// SXC: the ground marks on the grass (sxc.cover's set_marks: burnt, frozen, ...; sxc.grass's set_marks: what
// each does to plants), in the instanced grass variant of the DetailDoodad vertex modules (the translator
// inlines this file after GrassDensity.glsl; Tools/VulkanShaders). A plant reads the marks at its root: per
// channel, up to half its value it takes on the channel's tint, from half to full it shrinks toward the
// channel's size (burnt grass blackens, then burns away); the wind moves it by the channel's wind factor
// over the whole range (frozen plants stand stiff, and give way less to characters). A value below 0
// (the life channel aside) is growing back: fresh, no tint, sized by how much is still missing.
//
// The map: the covers' set (set 2), binding 23 (Vulkan/Cover.hpp kMarksBinding): RGBA32F, toroidal like
// the cover maps, a channel each (SXC_MARK_*: burn, frost, life -1..1, soot).
// The rows the client uploads (SXC/Grass):
//   c150  1 / cell (yd), the map's size in cells (0: none), its first cell i, j
//   c151  on (x)
//   c152 + 2k, c153 + 2k  channel k's look (k 0 burn, 1 frost, 2 life above 0, 3 soot, 4 life below 0:
//         blight): tint (rgb, strength); size at full, wind at full
// Per plant (sxcPlantDensity.w): 1 when the marks pass it by (GroundEffectDoodadWind's "no marks").

layout(set = 2, binding = 23) uniform sampler2D sxcMarks;

const int kSxcGrassMarks = 150;

// A plant's marks at its root: the tint (rgb, how much), its size factor and wind factor.
void SxcGrassMarks(vec2 root, out vec4 tint, out float size, out float wind)
{
    tint = vec4(0.0);
    size = 1.0;
    wind = 1.0;
    vec4 m = regs.v[kSxcGrassMarks];

    if (regs.v[kSxcGrassMarks + 1].x < 0.5 || m.y < 1.0 || sxcPlantDensity.w > 0.5)
    {
        return;
    }

    // Only inside the map's window (beyond it the toroidal texture holds another place).
    vec2 c = root * m.x;
    vec2 rel = c - m.zw;

    if (any(lessThan(rel, vec2(0.5))) || any(greaterThan(rel, vec2(m.y - 0.5))))
    {
        return;
    }

    vec4 v = textureLod(sxcMarks, c / m.y, 0.0);
    float values[5] = float[5](v.x, v.y, max(v.z, 0.0), v.w, max(-v.z, 0.0));
    vec3 colour = vec3(0.0);
    float amount = 0.0;

    for (int k = 0; k < 5; k++)
    {
        vec4 look = regs.v[kSxcGrassMarks + 2 + 2 * k];
        vec4 shape = regs.v[kSxcGrassMarks + 3 + 2 * k];

        // Below 0 (not the life channel): growing back, fresh: the plant's size only, -1 the smallest.
        if (k != 2 && k != 4 && values[k] < 0.0)
        {
            size *= mix(1.0, shape.x, clamp(-values[k], 0.0, 1.0));
            continue;
        }

        float value = clamp(values[k], 0.0, 1.0);

        if (value <= 0.0)
        {
            continue;
        }
        float t = clamp(value * 2.0, 0.0, 1.0) * look.w;
        colour += look.rgb * t;
        amount += t;
        size *= mix(1.0, shape.x, clamp(value * 2.0 - 1.0, 0.0, 1.0));
        wind *= mix(1.0, shape.y, value);
    }

    tint = amount > 0.0 ? vec4(colour / amount, min(amount, 1.0)) : vec4(0.0);
}
