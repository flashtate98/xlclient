// The pipeline of a key: the blend and topology tables, the vertex input of each program family,
// the pairing checks, the build, the pipeline table.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines/Internal.hpp"

#include <cstring>

namespace
{
    using namespace gxvk::vk::pipelines::detail;

    /**
     * The colour blend state of one EGxBlend value (CGxDeviceD3d's s_srcBlend / s_dstBlend table):
     *
     *   0 Opaque        off                     7 InvSrcAlphaAdd    INVSRCALPHA / ONE
     *   1 AlphaKey      off (the test, not a blend)  8 InvSrcAlphaOpaque INVSRCALPHA / ZERO
     *   2 Alpha         SRCALPHA / INVSRCALPHA  9 SrcAlphaOpaque    SRCALPHA / ZERO
     *   3 Add           SRCALPHA / ONE         10 NoAlphaAdd        ONE / ONE
     *   4 Mod           DSTCOLOR / ZERO        11 ConstantAlpha     CONSTANT_COLOR / ONE_MINUS_CONSTANT_COLOR
     *   5 Mod2x         DSTCOLOR / SRCCOLOR    12 BlendAdd          ONE / INVSRCALPHA (WarcraftXL's, modern M2 blend 7)
     *   6 ModAdd        DSTCOLOR / ONE
     *
     * The same factors for alpha as for colour, op ADD on both: D3D9 with SEPARATEALPHABLENDENABLE
     * off, which the engine never turns on. 11 reads the blend constants, which the draw sets
     * (D3D9's BLENDFACTOR). The write mask is the key's colour mask.
     */
    bool BlendState(uint32_t blend, uint32_t colourMask, VkPipelineColorBlendAttachmentState& a)
    {
        a.blendEnable  = VK_TRUE;
        a.colorBlendOp = VK_BLEND_OP_ADD;
        a.alphaBlendOp = VK_BLEND_OP_ADD;

        VkBlendFactor src = VK_BLEND_FACTOR_ONE;
        VkBlendFactor dst = VK_BLEND_FACTOR_ZERO;
        switch (blend)
        {
            case 0:
            case 1:  a.blendEnable = VK_FALSE; break;
            case 2:  src = VK_BLEND_FACTOR_SRC_ALPHA;           dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
            case 3:  src = VK_BLEND_FACTOR_SRC_ALPHA;           dst = VK_BLEND_FACTOR_ONE;                 break;
            case 4:  src = VK_BLEND_FACTOR_DST_COLOR;           dst = VK_BLEND_FACTOR_ZERO;                break;
            case 5:  src = VK_BLEND_FACTOR_DST_COLOR;           dst = VK_BLEND_FACTOR_SRC_COLOR;           break;
            case 6:  src = VK_BLEND_FACTOR_DST_COLOR;           dst = VK_BLEND_FACTOR_ONE;                 break;
            case 7:  src = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; dst = VK_BLEND_FACTOR_ONE;                 break;
            case 8:  src = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; dst = VK_BLEND_FACTOR_ZERO;                break;
            case 9:  src = VK_BLEND_FACTOR_SRC_ALPHA;           dst = VK_BLEND_FACTOR_ZERO;                break;
            case 10: src = VK_BLEND_FACTOR_ONE;                 dst = VK_BLEND_FACTOR_ONE;                 break;
            case 11: src = VK_BLEND_FACTOR_CONSTANT_COLOR;      dst = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; break;
            case 12: src = VK_BLEND_FACTOR_ONE;                 dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
            default: return false;
        }

        a.srcColorBlendFactor = src;
        a.dstColorBlendFactor = dst;
        a.srcAlphaBlendFactor = src;
        a.dstAlphaBlendFactor = dst;
        // The engine's GxRs_ColorWrite bits are D3D9's D3DCOLORWRITEENABLE_RED (1), GREEN (2),
        // BLUE (4), ALPHA (8): the same bits as VkColorComponentFlagBits.
        a.colorWriteMask      = colourMask & 0xFu;
        return true;
    }

    /// The compare op of the engine's GxRs_DepthFunc value (CGxDeviceD3d::s_cmpFunc).
    bool DepthOp(uint32_t func, VkCompareOp& out)
    {
        switch (func)
        {
            case 0: out = VK_COMPARE_OP_LESS_OR_EQUAL;    return true;
            case 1: out = VK_COMPARE_OP_EQUAL;            return true;
            case 2: out = VK_COMPARE_OP_GREATER_OR_EQUAL; return true;
            case 3: out = VK_COMPARE_OP_LESS;             return true;
            default: return false;
        }
    }

    /**
     * The cull mode of the engine's GxRs_Culling value under the master enable
     * (CGxDeviceD3d::s_cullMode: NONE, CW, CCW). The draw's clip-space y is negated for Vulkan and
     * both APIs put window y downwards, so a triangle's window-space winding is the same under
     * both: with the pipeline's front face CLOCKWISE, D3D's "cull clockwise" is cull FRONT and its
     * "cull counter-clockwise" is cull BACK.
     */
    bool CullMode(uint32_t cull, VkCullModeFlags& out)
    {
        switch (cull)
        {
            case 0: out = VK_CULL_MODE_NONE;      return true;
            case 1: out = VK_CULL_MODE_FRONT_BIT; return true;
            case 2: out = VK_CULL_MODE_BACK_BIT;  return true;
            default: return false;
        }
    }

    /// An EGxPrim as a Vulkan topology (CGxDeviceD3d::s_primitiveConversion).
    bool Topology(uint32_t prim, VkPrimitiveTopology& out)
    {
        switch (prim)
        {
            case 0: out = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;     return true;
            case 1: out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;      return true;
            case 2: out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;     return true;
            case 3: out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  return true;
            case 4: out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
            case 5: out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   return true;
            default: return false;
        }
    }

    /**
     * The engine attribute (EGxVertexAttrib) that carries a translated-interface semantic
     * (manifest::Semantic): position0 -> 0, blendweight0 -> 1, blendindices0 -> 2, normal0 -> 3,
     * color0 -> 4, color1 -> 5, texcoordN -> 6 + N. False for fog0 / psize0, which no vertex carries.
     */
    bool AttribOfSemantic(uint32_t semantic, uint32_t& attrib)
    {
        switch (semantic)
        {
            case manifest::kPosition0:     attrib = 0; return true;
            case manifest::kBlendWeight0:  attrib = 1; return true;
            case manifest::kBlendIndices0: attrib = 2; return true;
            case manifest::kNormal0:       attrib = 3; return true;
            case manifest::kColor0:        attrib = 4; return true;
            case manifest::kColor1:        attrib = 5; return true;
            default:
                if (semantic >= manifest::kTexcoord0 && semantic < manifest::kTexcoord0 + 8)
                {
                    attrib = 6 + (semantic - manifest::kTexcoord0);
                    return true;
                }
                return false;
        }
    }

    /**
     * The attribute location a translated module declares for a semantic (the translator's
     * VIN_LOCATION): position0 0, normal0 1, color0 2, color1 3, texcoord0 4, texcoord1 5,
     * blendweight0 6, blendindices0 7; texcoord2..7 at 8..13 (no module names them).
     */
    bool LocationOfSemantic(uint32_t semantic, uint32_t& location)
    {
        switch (semantic)
        {
            case manifest::kPosition0:     location = 0; return true;
            case manifest::kNormal0:       location = 1; return true;
            case manifest::kColor0:        location = 2; return true;
            case manifest::kColor1:        location = 3; return true;
            case manifest::kTexcoord0:     location = 4; return true;
            case manifest::kTexcoord0 + 1: location = 5; return true;
            case manifest::kBlendWeight0:  location = 6; return true;
            case manifest::kBlendIndices0: location = 7; return true;
            default:
                if (semantic > manifest::kTexcoord0 + 1 && semantic < manifest::kBlendWeight0)
                {
                    location = 8 + (semantic - (manifest::kTexcoord0 + 2));
                    return true;
                }
                return false;
        }
    }

    struct VertexInput
    {
        VkVertexInputAttributeDescription attrs[kAttribCount + 8];   // SXC: the grass variant's seven extra
        uint32_t                          count     = 0;
        bool                              needConst = false;   ///< the constant stream is read
    };

    /// One attribute from the key's table, or from the constant stream when the vertex lacks it.
    void AddAttribute(const Key& k, VertexInput& in, uint32_t location, uint32_t attrib, VkFormat constFormat, uint32_t constOffset)
    {
        VkVertexInputAttributeDescription& a = in.attrs[in.count++];
        a.location = location;
        if (k.attribMask & (1u << attrib))
        {
            a.binding = k.attribs[attrib].binding;
            a.format  = k.attribs[attrib].format;
            a.offset  = k.attribs[attrib].offset;
        }
        else
        {
            a.binding    = k.streamCount;
            a.format     = constFormat;
            a.offset     = constOffset;
            in.needConst = true;
        }
    }

    /**
     * The vertex input of a key's vertex program.
     *
     *   FixedFunction, Ui        position at 0, colour at 1, texcoord 0 at 2, texcoord 1 at 3 (the
     *                            backend's interface)
     *   FixedFunctionSemantic    position0 at 0, color0 at 2, texcoord0 at 4, texcoord1 at 5
     *   Translated               the record's declared inputs, each at its semantic's location, each
     *                            required of the vertex
     *
     * A colour the vertex lacks reads opaque white, a texcoord zero, from the constant stream.
     */
    bool BuildVertexInput(const Key& k, VertexInput& in, const char*& why)
    {
        in.count     = 0;
        in.needConst = false;
        if (!(k.attribMask & (1u << kAttribPosition)))
        {
            why = "the vertex has no position";
            return false;
        }
        switch (static_cast<VertexProgram>(k.vertex))
        {
            case VertexProgram::FixedFunction:
            case VertexProgram::Ui:
                AddAttribute(k, in, 0, kAttribPosition, VK_FORMAT_R32G32B32_SFLOAT, 0);
                AddAttribute(k, in, 1, kAttribColor0, VK_FORMAT_B8G8R8A8_UNORM, kConstantColourOffset);
                AddAttribute(k, in, 2, kAttribTexCoord0, VK_FORMAT_R32G32_SFLOAT, kConstantTexcoordOffset);
                AddAttribute(k, in, 3, kAttribTexCoord1, VK_FORMAT_R32G32_SFLOAT, kConstantTexcoordOffset);
                return true;

            case VertexProgram::FixedFunctionSemantic:
                AddAttribute(k, in, 0, kAttribPosition, VK_FORMAT_R32G32B32_SFLOAT, 0);
                AddAttribute(k, in, 2, kAttribColor0, VK_FORMAT_B8G8R8A8_UNORM, kConstantColourOffset);
                AddAttribute(k, in, 4, kAttribTexCoord0, VK_FORMAT_R32G32_SFLOAT, kConstantTexcoordOffset);
                AddAttribute(k, in, 5, kAttribTexCoord1, VK_FORMAT_R32G32_SFLOAT, kConstantTexcoordOffset);
                return true;

            // SXC: our terrain shader reads position0, normal0 and color0 (white when the vertex has none).
            case VertexProgram::Terrain:
                if (!(k.attribMask & (1u << kAttribNormal)))
                {
                    why = "the terrain vertex has no normal";
                    return false;
                }
                AddAttribute(k, in, 0, kAttribPosition, VK_FORMAT_R32G32B32_SFLOAT, 0);
                AddAttribute(k, in, 1, kAttribNormal, VK_FORMAT_R32G32B32_SFLOAT, 0);
                AddAttribute(k, in, 2, kAttribColor0, VK_FORMAT_B8G8R8A8_UNORM, kConstantColourOffset);
                return true;

            // SXC: a projected texture's box (step 5e) reads its position alone.
            case VertexProgram::Decal:
                AddAttribute(k, in, 0, kAttribPosition, VK_FORMAT_R32G32B32_SFLOAT, 0);
                return true;

            case VertexProgram::Translated:
            {
                const manifest::Record* r = manifest::At(k.vertexRecord);
                if (!r)
                {
                    why = "the vertex record is gone";
                    return false;
                }
                for (uint32_t sem = 0; sem < manifest::kSemanticCount; ++sem)
                {
                    if (!(r->inputMask & (1u << sem))) continue;
                    uint32_t attrib = 0, location = 0;
                    if (!AttribOfSemantic(sem, attrib) || !LocationOfSemantic(sem, location))
                    {
                        why = "the vertex module reads a semantic no vertex carries";
                        return false;
                    }
                    if (!(k.attribMask & (1u << attrib)))
                    {
                        why = "the vertex module reads a semantic this vertex layout does not carry";
                        return false;
                    }
                    VkVertexInputAttributeDescription& a = in.attrs[in.count++];
                    a.location = location;
                    a.binding  = k.attribs[attrib].binding;
                    a.format   = k.attribs[attrib].format;
                    a.offset   = k.attribs[attrib].offset;
                }
                // SXC: the instanced grass variant reads a plant's rows (step 7).
                if (r->grass)
                {
                    if (!k.plantStream)
                    {
                        why = "the grass variant needs the plant rows' stream";
                        return false;
                    }
                    for (uint32_t row = 0; row < 3; ++row)
                    {
                        VkVertexInputAttributeDescription& a = in.attrs[in.count++];
                        a.location = 13 + row;
                        a.binding  = k.plantStream - 1;
                        a.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
                        a.offset   = 16 * row;
                    }
                    // Step 7b: the plant's motion values (GrassMotion.glsl's sxcPlantMotion).
                    VkVertexInputAttributeDescription& m = in.attrs[in.count++];
                    m.location = 5;
                    m.binding  = k.plantStream - 1;
                    m.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
                    m.offset   = 64;
                    // Step 7d: the plant's density (copies, radius, spread as halves), its spot in its
                    // terrain cell, and the copy number (GrassDensity.glsl).
                    if (!k.copyStream)
                    {
                        why = "the grass variant needs the copy stream";
                        return false;
                    }
                    VkVertexInputAttributeDescription& density = in.attrs[in.count++];
                    density.location = 6;
                    density.binding  = k.plantStream - 1;
                    density.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
                    density.offset   = 80;
                    VkVertexInputAttributeDescription& cell = in.attrs[in.count++];
                    cell.location = 7;
                    cell.binding  = k.plantStream - 1;
                    cell.format   = VK_FORMAT_R32G32_SFLOAT;
                    cell.offset   = 88;
                    VkVertexInputAttributeDescription& copy = in.attrs[in.count++];
                    copy.location = 8;
                    copy.binding  = k.copyStream - 1;
                    copy.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
                    copy.offset   = 0;
                }
                return true;
            }

            default:
                why = "an unknown vertex program";
                return false;
        }
    }

    /**
     * Whether the fragment program's varying reads are written by the vertex program: a Vulkan
     * fragment input must have a matching vertex output. The backend's own family is paired only
     * with itself; FixedFunctionSemantic writes every varying of the translated interface.
     */
    bool PairingOk(const Key& k, const char*& why)
    {
        const auto v = static_cast<VertexProgram>(k.vertex);
        const auto f = static_cast<FragmentProgram>(k.fragment);
        if (TranslatedFamily(v) != TranslatedFamily(f))
        {
            why = "the vertex and fragment programs are of different interfaces";
            return false;
        }
        if (!TranslatedFamily(v)) return true;

        // SXC: our terrain shaders pair only with each other (the vertex one writes every varying).
        if ((v == VertexProgram::Terrain) != (f == FragmentProgram::Terrain))
        {
            why = "our terrain shader is paired with another program";
            return false;
        }
        if (v == VertexProgram::Terrain) return true;
        // SXC: and the decal ones (step 5e).
        if ((v == VertexProgram::Decal) != (f == FragmentProgram::Decal))
        {
            why = "the decal shader is paired with another program";
            return false;
        }
        if (v == VertexProgram::Decal) return true;

        uint32_t outputs = 0xFFFFFFFFu;
        if (v == VertexProgram::Translated)
        {
            const manifest::Record* r = manifest::At(k.vertexRecord);
            if (!r)
            {
                why = "the vertex record is gone";
                return false;
            }
            outputs = r->outputMask;
        }
        uint32_t reads = 0;
        if (f == FragmentProgram::Translated)
        {
            const manifest::Record* r = manifest::At(k.fragmentRecord);
            if (!r)
            {
                why = "the pixel record is gone";
                return false;
            }
            reads = r->readsMask;
        }
        else
            reads = (1u << manifest::kColor0) | (1u << manifest::kTexcoord0) | (1u << (manifest::kTexcoord0 + 1));
        if ((reads & ~outputs) != 0)
        {
            why = "the pixel module reads a varying the vertex module does not write";
            return false;
        }
        return true;
    }

    /// The name of a key's record for a log line, or the program's.
    const char* ProgramName(uint32_t program, uint16_t record, bool vertex)
    {
        if (const manifest::Record* r = manifest::At(record)) return manifest::FirstName(*r);
        static const char* kVertex[]   = { "FixedFunction", "Ui", "FixedFunctionSemantic", "Translated", "Terrain", "Decal" };
        static const char* kFragment[] = { "FixedFunction", "Ui", "Desaturate", "FixedFunctionTranslated", "Translated", "Terrain", "Decal" };
        if (vertex) return program < kVertexPrograms ? kVertex[program] : "?";
        return program < kFragmentPrograms ? kFragment[program] : "?";
    }

    void Refuse(const Key& k, const char* why)
    {
        ++s.stats.refusedKeys;
        Write(Warn, "vulkan: pipelines: a draw with vertex %s / pixel %s on attribute mask 0x%04X is refused: %s (draws with this key are skipped)",
              ProgramName(k.vertex, k.vertexRecord, true), ProgramName(k.fragment, k.fragmentRecord, false), k.attribMask, why);
    }

    /**
     * Builds the pipeline of a key.
     *
     * The vertex input has one binding per stream at the key's stride, plus the constant stream
     * at binding streamCount with stride 0 when a program reads an element the vertex lacks.
     * Viewport, scissor, depth bias and blend constants are dynamic; the depth test and write,
     * the culling, the colour mask and the fill mode are the key's; one sample.
     */
    bool Build(const Key& k, VkPipeline& out)
    {
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineColorBlendAttachmentState blend = {};
        VkCompareOp     depthOp = VK_COMPARE_OP_ALWAYS;
        VkCullModeFlags cull    = VK_CULL_MODE_NONE;
        if (!Topology(k.topology, topology) || !BlendState(k.blend, k.colourMask, blend) || !DepthOp(k.depthFunc, depthOp) || !CullMode(k.cull, cull))
        {
            Refuse(k, "the topology, the blend mode, the depth function or the cull mode is outside the tables");
            return false;
        }
        const char* why = "";
        if (!PairingOk(k, why))
        {
            Refuse(k, why);
            return false;
        }
        VertexInput in;
        if (!BuildVertexInput(k, in, why))
        {
            Refuse(k, why);
            return false;
        }
        const VkShaderModule vert = VertexModule(k);
        const VkShaderModule frag = FragmentModule(k);
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
        {
            Refuse(k, vert == VK_NULL_HANDLE ? "the vertex module could not be loaded" : "the pixel module could not be loaded");
            return false;
        }
        if (k.layoutIndex >= s.layoutCount)
        {
            Refuse(k, "the sampler-slot layout is gone");
            return false;
        }

        // SXC: a raised terrain chunk (step 5c) runs through the tessellation: triangles as patches of 3.
        const bool tessellated = TerrainTessellated(k);
        const VkShaderModule tesc = TessControlModule(k), tese = TessEvalModule(k);
        if (tessellated && (tesc == VK_NULL_HANDLE || tese == VK_NULL_HANDLE || topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST))
        {
            Refuse(k, "a raised terrain chunk without the tessellation modules, or not a triangle list");
            return false;
        }
        if (tessellated) topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;

        TerrainSpecialization vertexConstants, fragmentConstants;  // SXC: our terrain shader's options
        VkPipelineShaderStageCreateInfo stages[4] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[0].pSpecializationInfo = VertexSpecialization(k, vertexConstants);
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";
        stages[1].pSpecializationInfo = FragmentSpecialization(k, fragmentConstants);
        if (tessellated)
        {
            stages[2].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[2].stage  = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            stages[2].module = tesc;
            stages[2].pName  = "main";
            stages[2].pSpecializationInfo = stages[0].pSpecializationInfo;  // big alpha (it reads the alpha map)
            stages[3].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[3].stage  = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            stages[3].module = tese;
            stages[3].pName  = "main";
            stages[3].pSpecializationInfo = stages[0].pSpecializationInfo;  // the vertex math runs there
        }
        VkPipelineTessellationStateCreateInfo tessellation = {};
        tessellation.sType              = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
        tessellation.patchControlPoints = 3;

        VkVertexInputBindingDescription bindings[kMaxStreams + 1] = {};
        uint32_t bindingCount = 0;
        for (uint32_t i = 0; i < k.streamCount; ++i)
        {
            bindings[bindingCount].binding   = i;
            bindings[bindingCount].stride    = k.stride[i];
            bindings[bindingCount].inputRate = (k.instanceStreams & (1u << i)) ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;  // SXC: grass
            ++bindingCount;
        }
        if (in.needConst)
        {
            bindings[bindingCount].binding   = k.streamCount;
            bindings[bindingCount].stride    = 0;
            bindings[bindingCount].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            ++bindingCount;
        }

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount   = bindingCount;
        vertexInput.pVertexBindingDescriptions      = bindings;
        vertexInput.vertexAttributeDescriptionCount = in.count;
        vertexInput.pVertexAttributeDescriptions    = in.attrs;

        VkPipelineInputAssemblyStateCreateInfo assembly = {};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = topology;

        VkPipelineViewportStateCreateInfo viewport = {};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;

        // The bias is always enabled and set per draw: a constant factor of 0 is no bias.
        VkPipelineRasterizationStateCreateInfo raster = {};
        raster.sType           = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode     = (k.wireframe && s.wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        raster.cullMode        = cull;
        raster.frontFace       = VK_FRONT_FACE_CLOCKWISE;
        raster.depthBiasEnable = VK_TRUE;
        raster.lineWidth       = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // A depth write without a test is D3D's ZFUNC ALWAYS with ZWRITEENABLE: Vulkan writes only
        // when the test is enabled, so the test is on with compare ALWAYS.
        VkPipelineDepthStencilStateCreateInfo depth = {};
        depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable  = (k.depthTest || k.depthWrite) ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = k.depthWrite ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp   = k.depthTest ? depthOp : VK_COMPARE_OP_ALWAYS;
        depth.minDepthBounds   = 0.0f;
        depth.maxDepthBounds   = 1.0f;

        VkPipelineColorBlendStateCreateInfo blending = {};
        blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blending.attachmentCount = 1;
        blending.pAttachments    = &blend;

        const VkDynamicState dynamics[4] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
        VkPipelineDynamicStateCreateInfo dynamic = {};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 4;
        dynamic.pDynamicStates    = dynamics;

        VkGraphicsPipelineCreateInfo info = {};
        info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount          = tessellated ? 4 : 2;
        info.pStages             = stages;
        info.pVertexInputState   = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pTessellationState  = tessellated ? &tessellation : nullptr;
        info.pViewportState      = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pDepthStencilState  = &depth;
        info.pColorBlendState    = &blending;
        info.pDynamicState       = &dynamic;
        info.layout              = s.layouts[k.layoutIndex].layout;
        info.renderPass          = k.pass;
        info.subpass             = 0;

        const VkResult r = s.dapi->CreateGraphicsPipelines(s.device, s.cache, 1, &info, nullptr, &out);
        if (r != VK_SUCCESS)
        {
            out = VK_NULL_HANDLE;
            Write(Error, "vulkan: pipelines: vkCreateGraphicsPipelines failed: %s (vertex %s, pixel %s, topology %u, blend %u, %u stream(s))",
                  ResultName(r), ProgramName(k.vertex, k.vertexRecord, true), ProgramName(k.fragment, k.fragmentRecord, false),
                  k.topology, k.blend, k.streamCount);
            ++s.stats.refusedKeys;
            return false;
        }
        ++s.stats.pipelineBuilds;
        return true;
    }
}

namespace gxvk::vk::pipelines::detail
{
    /// The entry of a key: found, or built and remembered -- a refused key too, with a null
    /// pipeline, so its draws are skipped without a second attempt. Null when the table is full.
    const Entry* Lookup(const Key& k)
    {
        for (uint32_t i = 0; i < s.entryCount; ++i)
            if (std::memcmp(&s.entries[i].key, &k, sizeof k) == 0) return &s.entries[i];
        if (s.entryCount == kMaxPipelines)
        {
            if (!s.warnedFull)
            {
                s.warnedFull = true;
                Write(Warn, "vulkan: pipelines: %u pipelines live, the table is full; further distinct draws are skipped (logged once)", kMaxPipelines);
            }
            return nullptr;
        }
        VkPipeline pipeline = VK_NULL_HANDLE;
        Build(k, pipeline);
        Entry& e = s.entries[s.entryCount++];
        // Bytewise, padding included: the lookup above compares the whole struct.
        std::memcpy(&e.key, &k, sizeof k);
        e.pipeline = pipeline;
        if (pipeline != VK_NULL_HANDLE) ++s.stats.pipelines;
        return &e;
    }

    /// Destroys every pipeline (the pass they were built against is gone). The caller has made the
    /// device idle.
    void DropPipelines()
    {
        for (uint32_t i = 0; i < s.entryCount; ++i)
            if (s.entries[i].pipeline != VK_NULL_HANDLE) s.dapi->DestroyPipeline(s.device, s.entries[i].pipeline, nullptr);
        s.entryCount      = 0;
        s.stats.pipelines = 0;
        s.bound           = Bound();
    }
}
