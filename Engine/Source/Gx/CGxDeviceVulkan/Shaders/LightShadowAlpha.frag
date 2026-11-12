#version 450
// SXC: the alpha-keyed parts of a light's shadow map (leaves, fences, grilles: M2 batches with the
// alpha key or alpha blend mode): where their texture is see-through, they cast nothing.

layout(location = 0) in vec2 inUv;

layout(set = 0, binding = 0) uniform sampler2D tex;

void main()
{
    if (texture(tex, inUv).a < 0.5)
    {
        discard;
    }
}
