// The Vulkan backend's shaders: the engine's CGxShader objects stay the base's (the hash tables, the
// .bls code loaded from the MPQs, the reference counts); a shader becomes valid when this backend
// can draw its bytes -- with a program of its own for the interface's "UI" vertex shader and "UI" /
// "Desaturate" pixel shaders, or with the translated module the manifest lists for the MD5 of its
// code (Tools/VulkanShaders). Every other shader stays invalid, so the engine takes its
// fixed-function paths for it, which the backend's fixed-function programs render; an unknown
// shader is named once in the log and never retried.
//
// The engine asks CGxShader::Valid() before it sets a shader (IShaderCreate on first ask), and the
// interface and the shader effects set shaders without asking; a shader not yet created is created
// at the draw, as CGxDeviceD3d::IBindPixelShader does.
#include <cstdint>
#include <cstring>
#include <new>
#include <cstdlib>        // SXC:
#include <unordered_map>  // SXC:
#include <unordered_set>  // SXC:
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Manifest.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Md5.hpp"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Storm/H/storm.h"
#include "SXC/Log.h"  // SXC:
#include "SXC/Settings/Settings.h"  // SXC: [Terrain] OurShader
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Cover.hpp"  // SXC: our terrain shader's covers
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Lights.hpp" // SXC: and its point lights
#include "SXC/Lights/Lights.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"  // SXC:

namespace {
    namespace manifest  = gxvk::vk::manifest;
    namespace pipelines = gxvk::vk::pipelines;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Info;
    using gxvk::log::Warn;

    // The backend's program for a shader; CGxShader::m_apiSpecific points at one of these for every
    // valid shader: a static one for the shaders the backend owns, one of a table indexed by manifest
    // record for the translated ones.
    struct Program {
        pipelines::VertexProgram   vertex;
        pipelines::FragmentProgram fragment;
        const manifest::Record*    record;
    };

    Program g_uiVertex   = { pipelines::VertexProgram::Ui, pipelines::FragmentProgram::FixedFunction, nullptr };
    Program g_uiPixel    = { pipelines::VertexProgram::FixedFunction, pipelines::FragmentProgram::Ui, nullptr };
    Program g_desaturate = { pipelines::VertexProgram::FixedFunction, pipelines::FragmentProgram::Desaturate, nullptr };

    Program* g_translated      = nullptr;   ///< one per manifest record, made at first need
    uint32_t g_translatedCount = 0;

    struct Stats {
        uint64_t creates;
        uint64_t own;          ///< valid through a program of the backend's own
        uint64_t translated;   ///< valid through a translated module
        uint64_t unknown;      ///< invalid: bytes the manifest does not list
        uint64_t noCode;       ///< invalid: the engine loaded no code
        uint64_t flushes;      ///< constant flushes with a dirty range
        uint64_t pairsCompanion;      ///< draws of a translated vertex shader with no pixel shader, through the record's companion
        uint64_t pairsFixedPixel;     ///< ... through the fixed-function pixel stage of the translated interface
        uint64_t pairsFixedVertex;    ///< draws of a translated pixel shader with no vertex shader, through the fixed-function vertex stage
        uint64_t pairsOwnReplaced;    ///< draws mixing an own program with a translated one, the own one replaced by the interface's fixed stage
    };
    Stats g_stats;

    // The hash key is the name ShaderCreate was given, with ":n" for the n-th of several
    // permutations: the "UI" vertex shader is created with two ("UI:0", "UI:1" -- the second is
    // the stereo one, which this device never enables).
    Program* OwnProgramOf(const CGxShader* shader) {
        const char* name = shader->m_key.m_str;

        if (!name) {
            return nullptr;
        }

        if (shader->m_target == GxSh_Vertex) {
            if (!SStrCmpI(name, "UI", STORM_MAX_STR) || !SStrCmpI(name, "UI:0", STORM_MAX_STR) || !SStrCmpI(name, "UI:1", STORM_MAX_STR)) {
                return &g_uiVertex;
            }
        } else if (shader->m_target == GxSh_Pixel) {
            if (!SStrCmpI(name, "UI", STORM_MAX_STR)) {
                return &g_uiPixel;
            }

            if (!SStrCmpI(name, "Desaturate", STORM_MAX_STR)) {
                return &g_desaturate;
            }
        }

        return nullptr;
    }

    // The translated program of the manifest record with the md5 of the shader's code, of the
    // shader's kind.
    Program* TranslatedProgramOf(CGxShader* shader) {
        if (!manifest::Loaded() || shader->m_code.Count() == 0) {
            return nullptr;
        }

        char md5[33];
        gxvk::md5::Hex(shader->m_code.m_data, shader->m_code.Count(), md5);
        const manifest::Record* record = manifest::ByMd5(md5);

        if (!record) {
            return nullptr;
        }

        const bool vertex = shader->m_target == GxSh_Vertex;

        if ((record->kind == manifest::Kind::Vertex) != vertex) {
            return nullptr;
        }

        if (!g_translated) {
            g_translatedCount = manifest::Count();
            g_translated = new (std::nothrow) Program[g_translatedCount];

            if (!g_translated) {
                g_translatedCount = 0;
                return nullptr;
            }

            for (uint32_t i = 0; i < g_translatedCount; i++) {
                const manifest::Record* r = manifest::At(i);
                g_translated[i].record = r;
                g_translated[i].vertex = r && r->kind == manifest::Kind::Vertex ? pipelines::VertexProgram::Translated : pipelines::VertexProgram::FixedFunction;
                g_translated[i].fragment = r && r->kind == manifest::Kind::Pixel ? pipelines::FragmentProgram::Translated : pipelines::FragmentProgram::FixedFunction;
            }
        }

        const uint16_t index = manifest::IndexOf(record);
        return index < g_translatedCount ? &g_translated[index] : nullptr;
    }

    // SXC: every shader object the engine created through this device and has not destroyed. The
    // backend reads the bound shader at every draw (D3D9 only when the state changes), so a value in
    // GxRs_VertexShader / GxRs_PixelShader that is not a live shader would fault here: seen 2026-10-18
    // in Halls of Origination with two floats (620.3, 1.0) in the vertex shader slot. Such a value is
    // drawn as the fixed function and reported once instead.
    std::unordered_set<const CGxShader*> g_liveShaders;

    // SXC: our terrain shader (Shaders/Terrain.vert, Terrain.frag) in place of the client's terrain
    // records, when [Terrain] OurShader is on. The options come from the engine's own shader names:
    //   vertex  "Terrain:n"   n = 64 point lights + 16 (layers - 1) + 8 specular + 4 vertex colour + 2 env + shadow
    //   pixel   "Terrain1:n"  n = 16 unlit + 8 big alpha + 2 (layers - 1) + cube   (no shadow map)
    //           "Terrain3:n"  n = 48 unlit + 24 big alpha + 8 (mode - 1) + 2 (layers - 1) + cube   (modes 1-3)
    // (CMapRenderChunk::GetVertexShader, CMap::GetTerrainPixelShader); the cube records (a reflective
    // layer 0) since 2026-10-27, with kFragmentCube. Not covered, so stock: Terrain2 and the _pcf families
    // (this device takes the shadow cache's R32F path), Terrain0.
    SXC::Settings::Bool& OurTerrain() {
        static SXC::Settings::Bool s_setting("Terrain", "OurShader", true, "Our terrain shader (Vulkan)",
                                             "Draw the terrain with our own shader instead of the client's 128 + 128 translated ones. It looks the same; covers, wetness and our lights build on it.");
        return s_setting;
    }

    struct TerrainDecode {
        bool     decoded = false;
        bool     covered = false;
        uint32_t options = 0;
    };
    std::unordered_map<const CGxShader*, TerrainDecode> g_terrainDecodes;
    uint64_t g_terrainDraws = 0, g_terrainStock = 0;

    // "Family:n" -> family, n (n = 0 when the name has no permutation).
    bool SplitName(const char* name, char* family, size_t size, uint32_t& n) {
        if (!name) {
            return false;
        }

        const char* colon = strchr(name, ':');
        const size_t length = colon ? static_cast<size_t>(colon - name) : strlen(name);

        if (length + 1 > size) {
            return false;
        }

        memcpy(family, name, length);
        family[length] = '\0';
        n = colon ? static_cast<uint32_t>(strtoul(colon + 1, nullptr, 10)) : 0u;
        return true;
    }

    const TerrainDecode& DecodeTerrain(const CGxShader* shader) {
        TerrainDecode& d = g_terrainDecodes[shader];

        if (d.decoded) {
            return d;
        }

        d.decoded = true;
        char family[32];
        uint32_t n = 0;

        if (!SplitName(shader->m_key.m_str, family, sizeof(family), n)) {
            return d;
        }

        namespace terrain = pipelines::terrain;

        if (shader->m_target == GxSh_Vertex && !strcmp(family, "Terrain") && n < 128) {
            d.options = ((n >> 4) & 3u) | ((n & 1u) ? terrain::kVertexShadow : 0u) | ((n & 2u) ? terrain::kVertexEnv : 0u) |
                        ((n & 4u) ? terrain::kVertexColor : 0u) | ((n & 8u) ? terrain::kVertexSpecular : 0u) | ((n & 64u) ? terrain::kVertexPointLights : 0u);
            d.covered = true;
        } else if (shader->m_target == GxSh_Pixel && !strcmp(family, "Terrain1") && n < 32) {
            d.options = ((n >> 1) & 3u) | ((n & 8u) ? terrain::kFragmentBigAlpha : 0u) | ((n & 16u) ? terrain::kFragmentUnlit : 0u) |
                        ((n & 1u) ? terrain::kFragmentCube : 0u);
            d.covered = true;
        } else if (shader->m_target == GxSh_Pixel && !strcmp(family, "Terrain3") && n < 96) {
            const uint32_t mode = (n % 24u) / 8u + 1u;
            d.options = ((n >> 1) & 3u) | ((mode == 1 ? 1u : 2u) << 2) | (((n / 24u) & 1u) ? terrain::kFragmentBigAlpha : 0u) |
                        ((n / 48u) ? terrain::kFragmentUnlit : 0u) | (mode == 3 ? terrain::kFragmentNineTaps : 0u) | ((n & 1u) ? terrain::kFragmentCube : 0u);
            d.covered = true;
        }

        return d;
    }

    bool IsTerrainName(const CGxShader* shader) {
        const char* name = shader ? shader->m_key.m_str : nullptr;
        return name && !SStrCmpI(name, "Terrain", 7);
    }

    // The shader the engine set for a stage, created if it never was, or null for the fixed function.
    Program* Bound(CGxDeviceVulkan* device, void* value, const char* stage) {
        CGxShader* shader = static_cast<CGxShader*>(value);

        if (!shader) {
            return nullptr;
        }

        // SXC: see g_liveShaders.
        if (g_liveShaders.find(shader) == g_liveShaders.end()) {
            char key[64];
            SStrPrintf(key, sizeof(key), "gxvk-shader|%s", stage);
            SXC::LogOnce(key, "vulkan: the %s shader render state holds 0x%016llX, which is not a live shader (drawn without it; reported once per stage)",
                         stage, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(value)));
            return nullptr;
        }

        if (!shader->m_loaded) {
            device->IShaderCreate(shader);
        }

        return shader->m_valid ? static_cast<Program*>(shader->m_apiSpecific) : nullptr;
    }

}

// Mirrors CGxDeviceD3d::ShaderCreate (0x006AA130): the base's object, created on the device at once
// for a single permutation; several permutations are created when the engine first asks Valid().
void CGxDeviceVulkan::ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations) {
    CGxDevice::ShaderCreate(shaders, target, path, name, permutations);

    // SXC: see g_liveShaders.
    for (uint32_t i = 0; i < permutations; i++) {
        if (shaders[i]) {
            g_liveShaders.insert(shaders[i]);
        }
    }

    if (permutations == 1 && !shaders[0]->m_loaded) {
        this->IShaderCreate(shaders[0]);
    }
}

// Mirrors CGxDeviceD3d::IShaderCreateVertex / IShaderCreatePixel (0x006AA0D0 / 0x006AA070), with
// "CreateVertexShader succeeded" replaced by "this backend has a program for it": one of its own
// by name, else the translated module of the code's md5. Loaded either way, so the engine does not
// ask again.
void CGxDeviceVulkan::IShaderCreate(CGxShader* shader) {
    shader->m_valid = 0;

    if (!this->m_context) {
        return;
    }

    if (shader->m_target != GxSh_Vertex && shader->m_target != GxSh_Pixel) {
        return;
    }

    shader->m_loaded = 1;
    g_stats.creates++;

    Program* program = OwnProgramOf(shader);

    if (program) {
        g_stats.own++;
    } else {
        program = TranslatedProgramOf(shader);

        if (program) {
            g_stats.translated++;
        } else if (shader->m_code.Count() == 0) {
            g_stats.noCode++;
        } else {
            g_stats.unknown++;
            char md5[33];
            gxvk::md5::Hex(shader->m_code.m_data, shader->m_code.Count(), md5);
            Write(Warn, "vulkan: shaders: no translated module for %s shader \"%s\" (md5 %s, %u bytes); the engine's fixed-function path draws it",
                  shader->m_target == GxSh_Vertex ? "vertex" : "pixel", shader->m_key.m_str ? shader->m_key.m_str : "?", md5, shader->m_code.Count());
        }
    }

    shader->m_apiSpecific = program;

    if (program) {
        shader->m_valid = 1;
    }
}

// Mirrors CGxDeviceD3d::ShaderDestroy (0x006AA190): the API object goes with the last reference;
// here it is a table entry, so only the fields are reset.
void CGxDeviceVulkan::ShaderDestroy(CGxShader*& shader) {
    if (shader->m_refCount == 1) {
        shader->m_apiSpecific = nullptr;
        shader->m_loaded = 0;
        g_liveShaders.erase(shader);  // SXC: the base deletes it with this last reference
        g_terrainDecodes.erase(shader);  // SXC: a new object may get this address
    }

    CGxDevice::ShaderDestroy(shader);
}

// Mirrors CGxDeviceD3d::IShaderReload: the code reloaded from the MPQs, the device object made again.
void CGxDeviceVulkan::IShaderReload(CGxShader* shader, const char* path, const char* name) {
    if (shader->m_target != GxSh_Vertex && shader->m_target != GxSh_Pixel) {
        return;
    }

    if (shader->m_refCount == 1) {
        shader->m_apiSpecific = nullptr;
        shader->m_loaded = 0;
    }

    this->IShaderLoad(&shader, static_cast<EGxShTarget>(shader->m_target), path, name, 1);
    this->IShaderCreate(shader);
}

// The base keeps the shadow constants; IShaderConstantsFlush moves their dirty ranges into the
// device's register files at each draw.
void CGxDeviceVulkan::ShaderConstantsSet(EGxShTarget target, uint32_t index, const float* constants, uint32_t count) {
    CGxDevice::ShaderConstantsSet(target, index, constants, count);
}

// The folder the translated shaders are read from: GX_VULKAN_SHADERS in the environment, else
// VulkanShaders beside the executable.
void CGxDeviceVulkan::IShaderFolder(char* folder, size_t size) {
    DWORD length = GetEnvironmentVariableA("GX_VULKAN_SHADERS", folder, static_cast<DWORD>(size));

    if (length > 0 && length < size) {
        return;
    }

    char exe[MAX_PATH];
    length = GetModuleFileNameA(nullptr, exe, sizeof(exe));

    if (length == 0 || length >= sizeof(exe)) {
        SStrCopy(folder, "VulkanShaders", static_cast<int32_t>(size));
        return;
    }

    char* slash = strrchr(exe, '\\');

    if (slash) {
        *slash = '\0';
    }

    SStrPrintf(folder, static_cast<int32_t>(size), "%s\\VulkanShaders", exe);
}

// Mirrors CGxDeviceD3d::IShaderConstantsFlush (0x006A9FE0): the shadow constants' dirty ranges
// become the device's registers (index 1 of s_shadowConstants is the vertex file, 0 the pixel one,
// as ShaderConstantsSet files them), and the ranges are reset.
void CGxDeviceVulkan::IShaderConstantsFlush() {
    ShadowConstants& vs = CGxDevice::s_shadowConstants[1];

    if (vs.dirtyMin <= vs.dirtyMax && vs.dirtyMax < 256) {
        memcpy(&this->m_vertexRegs[vs.dirtyMin * 4], &vs.constants[vs.dirtyMin * 4], (vs.dirtyMax - vs.dirtyMin + 1) * 4 * sizeof(float));
        g_stats.flushes++;
    }

    vs.dirtyMin = 255;
    vs.dirtyMax = 0;

    ShadowConstants& ps = CGxDevice::s_shadowConstants[0];

    if (ps.dirtyMin <= ps.dirtyMax && ps.dirtyMax < 256) {
        memcpy(&this->m_pixelRegs[ps.dirtyMin * 4], &ps.constants[ps.dirtyMin * 4], (ps.dirtyMax - ps.dirtyMin + 1) * 4 * sizeof(float));
        g_stats.flushes++;
    }

    ps.dirtyMin = 255;
    ps.dirtyMax = 0;
}

// The programs of a draw from GxRs_VertexShader and GxRs_PixelShader, and the pairing D3D would
// make of them:
//
//   both translated           the two records (the pipeline checks the pixel module's reads against
//                             the vertex module's outputs)
//   translated vertex, no     the record's generated companion when it has one (the weather), else
//   pixel shader              the fixed-function pixel stage of the translated interface -- D3D runs
//                             its fixed-function stage 0 after any vertex shader
//   translated pixel, no      the fixed-function vertex stage of the translated interface, with the
//   vertex shader             device's matrices
//   an own program beside a   the own one is replaced by the interface's fixed stage (the UI vertex
//   translated one            shader's registers transposed into the matrix the stage reads)
//   neither translated        the backend's own programs (the fixed function, the UI)
//
// The UI vertex program reads its registers c0..c3 from the vertex register file; a fixed-function
// vertex program the device's World * View * ProjNative.
//
// @return uint32 samplerMask : the texture stages the draw must resolve (bit s = GxRs_Texture0 + s)
uint32_t CGxDeviceVulkan::IShaderPrograms(pipelines::Draw& draw) {
    const Program* vs = Bound(this, this->m_appRenderStates[GxRs_VertexShader].m_value.m_data.p, "vertex");  // SXC: stage names
    const Program* ps = Bound(this, this->m_appRenderStates[GxRs_PixelShader].m_value.m_data.p, "pixel");

    const bool vsTranslated = vs && vs->record;
    const bool psTranslated = ps && ps->record;

    draw.vertex = pipelines::VertexProgram::FixedFunction;
    draw.fragment = pipelines::FragmentProgram::FixedFunction;
    draw.vertexRecord = nullptr;
    draw.fragmentRecord = nullptr;
    draw.vertexRows = pipelines::kVertexRowsSmall;
    draw.lightsSurface = SXC::Lights::DrawSurface();  // SXC: inside / outside / any; a portrait's models get none of the world's lights

    if (!vsTranslated && !psTranslated) {
        if (vs) {
            draw.vertex = vs->vertex;
        }

        if (ps) {
            draw.fragment = ps->fragment;
        }

        if (draw.vertex == pipelines::VertexProgram::Ui) {
            memcpy(draw.xform, &this->m_vertexRegs[0], 16 * sizeof(float));
        } else {
            this->IModelViewProj(draw.xform);
        }

        // The fixed-function pixel stages read texture stages 0 and 1; the UI and Desaturate
        // shaders stage 0 alone.
        return draw.fragment == pipelines::FragmentProgram::FixedFunction ? 3u : 1u;
    }

    // SXC: our terrain shader for a terrain pair it covers (both stages: it pairs only with itself). It
    // reads the covers' set 2 and the lights' set 3, so it needs Vulkan/Cover and Vulkan/Lights.
    if (vsTranslated && psTranslated && OurTerrain().Get() && gxvk::vk::cover::Ready() && gxvk::vk::lights::Ready()) {
        const CGxShader* vsShader = static_cast<const CGxShader*>(this->m_appRenderStates[GxRs_VertexShader].m_value.m_data.p);
        const CGxShader* psShader = static_cast<const CGxShader*>(this->m_appRenderStates[GxRs_PixelShader].m_value.m_data.p);

        if (IsTerrainName(vsShader) && IsTerrainName(psShader)) {
            const TerrainDecode& v = DecodeTerrain(vsShader);
            const TerrainDecode& p = DecodeTerrain(psShader);

            if (v.covered && p.covered) {
                draw.vertex = pipelines::VertexProgram::Terrain;
                draw.fragment = pipelines::FragmentProgram::Terrain;
                draw.vertexRecord = vs->record;
                draw.vertexRows = pipelines::kVertexRowsSmall;
                draw.terrainVertex = v.options;
                draw.terrainFragment = p.options;

                // Our point lights replace the client's 3 per chunk while a mod has them on (step 6a).
                if (SXC::Lights::StockTerrainOff()) {
                    draw.terrainVertex &= ~pipelines::terrain::kVertexPointLights;
                }

                // A chunk that can carry depth (the cover seam's c13.x) is raised through the
                // tessellation where the device has it, its flat self drawn a little lower first so
                // a hairline where two chunks' covers don't meet shows ground (step 5c).
                const float raise = reinterpret_cast<const float*>(this->m_pixelRegs)[13 * 4];

                if (raise > 0.5f && gxvk::vk::cover::Tessellation() && !(v.options & pipelines::terrain::kVertexEnv)) {
                    draw.terrainVertex |= pipelines::terrain::kVertexTess | ((p.options & pipelines::terrain::kFragmentBigAlpha) ? pipelines::terrain::kVertexBigAlpha : 0u);
                    draw.terrainUnderlay = true;
                }
                // The covers' set for this frame, made now: its textures resolve like the draw's own
                // stages, before the draw takes the pass (an inline upload may end it).
                gxvk::vk::cover::FrameSet(gxvk::vk::frame::Slot(), gxvk::vk::frame::Serial());
                gxvk::vk::lights::FrameSet(gxvk::vk::frame::Slot(), gxvk::vk::frame::Serial());

                if (g_terrainDraws++ == 0) {
                    SXC::LogOnce("gxvk-terrain-on", "vulkan: our terrain shader draws the terrain (first: %s / %s)", vsShader->m_key.m_str, psShader->m_key.m_str);
                }

                return pipelines::terrain::SamplerMask(p.options);
            }

            g_terrainStock++;
            char key[96];
            SStrPrintf(key, sizeof(key), "gxvk-terrain|%s|%s", vsShader->m_key.m_str, psShader->m_key.m_str);
            SXC::LogOnce(key, "vulkan: our terrain shader doesn't cover %s / %s yet; the stock shaders draw it", vsShader->m_key.m_str, psShader->m_key.m_str);
        }
    }

    if (vsTranslated) {
        draw.vertex = pipelines::VertexProgram::Translated;
        draw.vertexRecord = vs->record;
        draw.vertexRows = vs->record->vconst == pipelines::kVertexRowsLarge ? pipelines::kVertexRowsLarge : pipelines::kVertexRowsSmall;
    } else {
        draw.vertex = pipelines::VertexProgram::FixedFunctionSemantic;

        if (vs && vs->vertex == pipelines::VertexProgram::Ui) {
            // The UI's c0..c3 are the rows of the transposed view-projection: transposed back, they
            // are the matrix the fixed stage multiplies by.
            const float* c = &this->m_vertexRegs[0];

            for (uint32_t row = 0; row < 4; row++) {
                for (uint32_t col = 0; col < 4; col++) {
                    draw.xform[row * 4 + col] = c[col * 4 + row];
                }
            }

            g_stats.pairsOwnReplaced++;
        } else {
            this->IModelViewProj(draw.xform);
        }

        g_stats.pairsFixedVertex++;
    }

    if (psTranslated) {
        draw.fragment = pipelines::FragmentProgram::Translated;
        draw.fragmentRecord = ps->record;
    } else {
        const manifest::Record* companion = vsTranslated ? manifest::Companion(*vs->record) : nullptr;

        if (companion) {
            draw.fragment = pipelines::FragmentProgram::Translated;
            draw.fragmentRecord = companion;
            g_stats.pairsCompanion++;
        } else {
            draw.fragment = pipelines::FragmentProgram::FixedFunctionTranslated;
            g_stats.pairsFixedPixel++;

            if (ps) {
                g_stats.pairsOwnReplaced++;
            }
        }
    }

    return draw.fragmentRecord ? draw.fragmentRecord->samplerMask : 3u;
}
