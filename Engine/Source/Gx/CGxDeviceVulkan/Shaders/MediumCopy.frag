// SXC: the underwater medium back onto the target (Vulkan/Medium.cpp): Medium.comp's result, pixel for pixel.
#version 450

layout(set = 0, binding = 4) uniform sampler2D mediumColour;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 oC0;

void main()
{
    oC0 = texelFetch(mediumColour, ivec2(gl_FragCoord.xy), 0);
}
