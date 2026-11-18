#version 450
// SXC: our WMO vertex shader (Vulkan/Pipelines: VertexProgram::Wmo; kb: our-shaders), in place of the
// translated stock and Cataclysm WMO vertex records for a unified WMO batch. One shader for every material
// shader id, chosen by specialization constants; the engine's registers read here and in Wmo.frag only
// (the contract on the kb page):
//   v2..v5    the projection             v10, v11, v12  ambient, direct, the sun's direction (view space)
//   v6..v9    Cataclysm's two texture    v28, v29       the material's diffuse, emissive
//             matrices (MapObjCata::     v30            fog: scale, offset, power
//             ApplyMatrices)             v31..v33       world x view
//   v224..v235 the sun shadow's cascades (CShadowCache)
// What the stock and Cataclysm vertex shaders computed, unchanged: the lit colour (MapObjUDiffuse_T1),
// the fog, the shadow coordinates; the texture coordinates of retail's vertex shaders per id (T1, Comp,
// T1_Refl, Comp_Refl, T1_Env_T2).

layout(constant_id = 0) const int  kShader     = 0;     // the material's shader id
layout(constant_id = 1) const bool kLit        = false; // the stock lighting mode isn't 0
layout(constant_id = 2) const int  kShadowMode = 0;     // 0 none, 1 one cascade, 2 and 3 all four

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor0;   // MOCV (white where the group has none)
layout(location = 3) in vec4 inColor1;   // the blend set: its alpha mixes the two-layer shaders' textures
layout(location = 4) in vec2 inTex0;
layout(location = 5) in vec2 inTex1;     // the second UV set (zero where the group has none)

layout(location = 0) out vec4 outColor;   // the lit colour, the vertex alpha
layout(location = 1) out float outBlend;  // the blend set's alpha
layout(location = 2) out vec2 outTex0;
layout(location = 3) out vec2 outTex1;    // the second layer (Comp), or the environment (T1_Refl)
layout(location = 4) out vec2 outTex2;    // the environment (Comp_Refl) or the third texture (T1_Env_T2)
layout(location = 5) out vec3 outView;    // the view-space position
layout(location = 6) out vec3 outNormal;  // the view-space normal
layout(location = 7) out vec4 outShadow0; // the cascades' coordinates (w: the far one's, CShadowCache)
layout(location = 8) out vec4 outShadow1;
layout(location = 9) out vec4 outShadow2;
layout(location = 10) out float outFog;   // 1 clear, 0 all fog

layout(std140, set = 1, binding = 0) uniform Regs {
    vec4 p[16];
    vec4 misc;
    vec4 v[256];
} regs;

// D3D9's pow: 0 at 0.
float DxPow(float x, float y) { return x == 0.0 ? 0.0 : exp2(y * log2(abs(x))); }

// Cataclysm's effects read texture matrices c6..c9 (the stock ones read the coordinates as they are).
bool CataMatrices() { return kShader == 7 || kShader == 9 || kShader == 11 || kShader == 12 || kShader == 13; }

vec2 Matrix(int row, vec2 uv)
{
    vec3 t = vec3(uv, 1.0);
    return vec2(dot(regs.v[row].xyw, t), dot(regs.v[row + 1].xyw, t));
}

void main()
{
    vec4 position = vec4(inPosition, 1.0);
    vec4 view = vec4(dot(regs.v[31], position), dot(regs.v[32], position), dot(regs.v[33], position), 1.0);
    vec4 clip = vec4(dot(regs.v[2], view), dot(regs.v[3], view), dot(regs.v[4], view), dot(regs.v[5], view));
    // D3D clip space has +y up, Vulkan +y down; z is already in [0, 1].
    gl_Position = vec4(clip.x, -clip.y, clip.z, clip.w);

    vec3 n = vec3(dot(regs.v[31].xyz, inNormal), dot(regs.v[32].xyz, inNormal), dot(regs.v[33].xyz, inNormal));
    n = dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 0.0, 1.0);

    // The stock lit colour: the sun on the ambient, times the material's diffuse, plus the vertex colour and
    // the emissive; unlit, the vertex colour alone.
    vec4 colour = inColor0;

    if (kLit)
    {
        vec3 light = clamp(clamp(dot(-regs.v[12].xyz, n), 0.0, 1.0) * regs.v[11].xyz + regs.v[10].xyz, 0.0, 1.0);
        colour.rgb = clamp(regs.v[28].xyz * light + inColor0.rgb + regs.v[29].xyz, 0.0, 1.0);
        colour.a = clamp(inColor0.a, 0.0, 1.0);
    }

    outColor = colour;
    outBlend = inColor1.a;

    // The texture coordinates of retail's vertex shader for the id.
    vec3 eye = normalize(view.xyz);
    vec2 reflection = reflect(eye, n).xy;
    vec2 uv0 = CataMatrices() ? Matrix(6, inTex0) : inTex0;
    vec2 uv1 = CataMatrices() ? Matrix(8, inTex1) : inTex1;
    outTex0 = uv0;

    if (kShader == 3 || kShader == 5)
    {
        outTex1 = reflection;                                     // T1_Refl
        outTex2 = vec2(0.0);
    }
    else if (kShader == 11 || kShader == 12)
    {
        outTex1 = normalize(reflect(eye, n) + vec3(0.0, 0.0, 1.0)).xy * 0.5 + 0.5;   // T1_Env_T2: a sphere map
        outTex2 = uv1;
    }
    else
    {
        outTex1 = uv1;                                            // Comp
        outTex2 = reflection;                                     // Comp_Refl
    }

    outView = view.xyz;
    outNormal = n;

    // The fog factor as the stock computes it.
    outFog = min(DxPow(max(view.z * regs.v[30].x + regs.v[30].y, 0.0), regs.v[30].z), 1.0);

    // The cascades: three, each with a fourth row in w for the far one (CShadowCache, mode 2 on).
    outShadow0 = vec4(0.0);
    outShadow1 = vec4(0.0);
    outShadow2 = vec4(0.0);

    if (kShadowMode >= 1)
    {
        outShadow0.xyz = vec3(dot(view, regs.v[224]), dot(view, regs.v[225]), dot(view, regs.v[226]));
    }

    if (kShadowMode >= 2)
    {
        outShadow1.xyz = vec3(dot(view, regs.v[227]), dot(view, regs.v[228]), dot(view, regs.v[229]));
        outShadow2.xyz = vec3(dot(view, regs.v[230]), dot(view, regs.v[231]), dot(view, regs.v[232]));
        outShadow0.w = dot(view, regs.v[233]);
        outShadow1.w = dot(view, regs.v[234]);
        outShadow2.w = dot(view, regs.v[235]);
    }
}
