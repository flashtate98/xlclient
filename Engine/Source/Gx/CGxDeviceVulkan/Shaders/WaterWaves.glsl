// SXC: our water's waves (Vulkan/Water.cpp, step 3 of the water shader): an FFT ocean after Tessendorf,
// ported from Crest (MIT; github.com/wave-harmonic/crest, Crest Water 4: FFTSpectrum.compute,
// FFTCompute.compute; "Inspired by github.com/speps/GX-EncinoWaves") and reshaped for three fixed cascades
// instead of Crest's LOD data. Each cascade is an N x N tile repeating over the world, holding its own band
// of wavelengths (no overlap with the others). Shared by WaterSpectrum.comp, WaterFft.comp, WaterDerive.comp.
//
// Copyright (c) 2019 Wave Harmonic and contributors (the Crest parts), MIT License:
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
// following conditions: The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
// KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

const int   kWaveN     = 256;   // texels per cascade side (Vulkan/Water.cpp's kWaveN)
const int   kWaveLog2N = 8;
const float kTau       = 6.28318530718;

// sizes:  the cascades' world sizes (yd), N
// bounds: the wave numbers between cascades 0 / 1 and 1 / 2 (rad / yd); gravity (yd / s^2); the loop period (s)
// wind:   direction x, y (unit), speed (yd / s), spread (0 all with the wind .. 1 every way)
// misc:   amplitude scale, chop (horizontal displacement), time (s), 0
layout(push_constant) uniform WavePush
{
    vec4 sizes;
    vec4 bounds;
    vec4 wind;
    vec4 misc;
} pw;

// The spectrum's starting amplitudes, its state at the time, the row pass's result, the displacement, the
// slopes and crest folding: an array layer per cascade.
layout(set = 0, binding = 0, rgba32f) uniform image2DArray waveH0;      // h0(k), h0(-k) conjugated
layout(set = 0, binding = 1, rgba32f) uniform image2DArray waveSpecA;   // height, x displacement (complex)
layout(set = 0, binding = 2, rgba32f) uniform image2DArray waveSpecB;   // y displacement (complex), 0
layout(set = 0, binding = 3, rgba32f) uniform image2DArray waveRowsA;
layout(set = 0, binding = 4, rgba32f) uniform image2DArray waveRowsB;
layout(set = 0, binding = 5, rgba16f) uniform image2DArray waveDisp;    // dx, dy, height (yd), 0
layout(set = 0, binding = 6, rgba16f) uniform image2DArray waveSlopes;  // dh/dx, dh/dy, Jacobian, height

// A texel's wave vector (standard FFT order: index m is wave number m, or m - N past the middle).
vec2 WaveVector(ivec2 texel, float size)
{
    ivec2 m = texel - ivec2(greaterThanEqual(texel, ivec2(kWaveN / 2))) * kWaveN;
    return kTau * vec2(m) / size;
}

// Deep-water dispersion, each frequency rounded to whole loops of the period (Crest): the waves repeat
// exactly after it, so the clock's wrap doesn't show.
float Dispersion(float k)
{
    float w = sqrt(abs(pw.bounds.z * k));

    if (pw.bounds.w > 0.0)
    {
        float loops = ceil(pw.bounds.w * w / kTau);
        w = kTau * loops / pw.bounds.w;
    }

    return w;
}

vec2 Cmul(vec2 a, vec2 b)
{
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}
