// The present pass's fragment stage: the scene's texel at this pixel, through the gamma ramp when
// the ramp applies (D3D9's SetGammaRamp, which the engine uses in fullscreen).
#version 450

layout(location = 0) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D scene;

// The ramp as 256 entries of (r, g, b, 0) in 0..1, and params.x = 1 when it applies. SXC: params.y the
// full-screen depth view (0 off, 1 the depth buffer as it is, 2 the water's copy as the view's w),
// params.z the w that shows white (yd).
layout(set = 0, binding = 1) uniform Gamma
{
    vec4 lut[256];
    vec4 params;
} gamma;

// SXC: the water's scene copy (r: the view's w, g: the raw depth); the scene itself when there's none.
layout(set = 0, binding = 2) uniform sampler2D depthCopy;

void main()
{
    int depthView = int(gamma.params.y + 0.5);

    // SXC: the depth views (the Water panel). Magenta: not a number. The raw depth on a log scale, as
    // D24's values crowd at 1 (black 0, white the far plane); the copy's w in grey up to params.z, blue
    // where nothing was drawn.
    if (depthView != 0)
    {
        vec2 copy = textureLod(depthCopy, vTexCoord, 0.0).rg;
        vec3 c;

        if (depthView == 1)
        {
            float d = copy.g;
            c = isnan(d) ? vec3(1.0, 0.0, 1.0) : vec3(clamp(-log2(max(1.0 - d, 1e-7)) / 23.0, 0.0, 1.0));
        }
        else
        {
            float w = copy.r;
            c = isnan(w) ? vec3(1.0, 0.0, 1.0)
              : w >= 1e5 ? vec3(0.1, 0.2, 0.5)
              : vec3(sqrt(clamp(w / max(gamma.params.z, 1.0), 0.0, 1.0)));
        }

        outColor = vec4(c, 1.0);
        return;
    }

    vec4 c = texture(scene, vTexCoord);
    if (gamma.params.x > 0.5)
    {
        ivec3 i = ivec3(clamp(c.rgb * 255.0 + 0.5, 0.0, 255.0));
        c.rgb   = vec3(gamma.lut[i.r].r, gamma.lut[i.g].g, gamma.lut[i.b].b);
    }
    outColor = vec4(c.rgb, 1.0);
}
