// The graphics pipelines a draw binds, cached by what the draw needs; the samplers, the per-frame
// descriptor sets and the uniform slices a draw binds; and the record of one draw.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::manifest { struct Record; }

// Two families of programs, combined per draw:
//
//   The backend's own (Shaders/*.vert, *.frag), in their own interface -- position, colour,
//   texcoord 0 and texcoord 1 at locations 0..3, the transform and parameters pushed:
//     vertex    FixedFunction  the position through the matrix the draw pushes (World * View * ProjNative),
//                              the texcoords through the stages' texture transforms
//               Ui             the engine's "UI" vertex shader: the position against c0..c3 pushed
//     fragment  FixedFunction  D3D9's texture stages 0 and 1 as the engine configures them, then the alpha test
//               Ui             the engine's "UI" pixel shader: texture * vertex colour, alpha test
//               Desaturate     the engine's "Desaturate" pixel shader: the texture's luminance * colour
//
//   The translated (Tools/VulkanShaders: the client's own DX9 shaders), in the translated interface --
//   vertex inputs and varyings at fixed locations per semantic, the constant registers in a uniform
//   slice at set 1, one combined image sampler per D3D sampler slot at set 0 binding <slot>:
//     vertex    Translated             a manifest vertex record's module
//               FixedFunctionSemantic  the fixed-function vertex stage in that interface, for a
//                                      translated pixel module drawn with no vertex shader
//     fragment  Translated             a manifest pixel record's module (a generated companion included)
//               FixedFunctionTranslated the fixed-function pixel stage in that interface, for a
//                                      translated vertex module drawn with no pixel shader
//
//   SXC: our terrain shader (Shaders/Terrain.vert, Terrain.frag), in the translated interface, in place
//   of the client's terrain records when [Terrain] OurShader is on: Terrain for both stages, the
//   record's options as specialization constants (Draw::terrainVertex / terrainFragment). And Decal for
//   both (Shaders/Decal.vert, Decal.frag): a projected texture's box, shaded onto the raised covers.
//
// A pipeline is built for each distinct combination of vertex layout, programs (and records),
// topology, blend mode, depth and culling state, colour write mask, fill mode, sampler-slot set and
// render pass, on first use, and kept for the device's life. The viewport, the scissor, the depth
// bias and the blend constants are dynamic. Everything is Vulkan 1.0 core.
namespace gxvk::vk::pipelines
{
    /// Streams one draw may bind: one per distinct buffer of the engine's attribute table.
    constexpr uint32_t kMaxStreams = 8;

    /// The engine's vertex attributes (EGxVertexAttrib): position, blend weight, blend indices,
    /// normal, colour 0, colour 1, texcoord 0..7.
    constexpr uint32_t kAttribCount = 14;

    /// D3D's texture stages / sampler slots.
    constexpr uint32_t kTextureStages = 16;

    /// The constant register files of the translated interface: pixel c0..c15, vertex c0..c63 or
    /// c0..c255 by the vertex record's `vconst`.
    constexpr uint32_t kPixelRows       = 16;
    constexpr uint32_t kVertexRowsSmall = 64;
    constexpr uint32_t kVertexRowsLarge = 256;

    enum class VertexProgram : uint32_t
    {
        FixedFunction = 0,
        Ui            = 1,
        FixedFunctionSemantic = 2,
        Translated    = 3,
        Terrain       = 4,   // SXC:
        Decal         = 5,   // SXC: projected textures on the raised covers (step 5e)
        Count
    };

    enum class FragmentProgram : uint32_t
    {
        FixedFunction = 0,
        Ui            = 1,
        Desaturate    = 2,
        FixedFunctionTranslated = 3,
        Translated    = 4,
        Terrain       = 5,   // SXC:
        Decal         = 6,   // SXC: projected textures on the raised covers (step 5e)
        Count
    };

    // SXC: our terrain shader's options, packed (Shaders/Terrain.vert, Terrain.frag).
    namespace terrain
    {
        // Vertex: bits 0-1 layers - 1, 2 shadow, 3 env, 4 vertex colour, 5 specular, 6 point lights; the
        // covers (step 5c): 7 raised through the tessellation, 8 the lowered underlay of a raised chunk,
        // 9 big alpha (the raised points read the alpha map like the pixel record does).
        constexpr uint32_t kVertexShadow = 1u << 2, kVertexEnv = 1u << 3, kVertexColor = 1u << 4, kVertexSpecular = 1u << 5, kVertexPointLights = 1u << 6;
        constexpr uint32_t kVertexTess = 1u << 7, kVertexSunk = 1u << 8, kVertexBigAlpha = 1u << 9;
        // Fragment: bits 0-1 layers - 1, 2-3 shadow group (0 none, 1 one cascade, 2 all), 4 big alpha,
        // 5 unlit layers, 6 nine taps (shadow mode 3), 7 layer 0 a cube map (a reflective layer).
        constexpr uint32_t kFragmentBigAlpha = 1u << 4, kFragmentUnlit = 1u << 5, kFragmentNineTaps = 1u << 6, kFragmentCube = 1u << 7;
        inline uint32_t Layers(uint32_t options) { return (options & 3u) + 1u; }
        inline uint32_t ShadowGroup(uint32_t fragment) { return (fragment >> 2) & 3u; }
        /// The first slot of the layers' height textures (`_h`, one per layer; white where a layer has none).
        constexpr uint32_t kHeightSlot = 9;
        /// The samplers a fragment module of these options declares: the layers, the alpha map, the
        /// cascades, the layers' height textures.
        inline uint32_t SamplerMask(uint32_t fragment)
        {
            const uint32_t group = ShadowGroup(fragment);
            const uint32_t layers = Layers(fragment);
            return ((1u << (layers + 1u)) - 1u) | (group >= 1 ? 1u << 5 : 0u) | (group >= 2 ? 0xEu << 5 : 0u) | (((1u << layers) - 1u) << kHeightSlot);
        }
    }

    struct Stream
    {
        VkBuffer     buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;     ///< byte offset the stream starts at inside the buffer
        uint32_t     stride = 0;
    };

    /// One vertex attribute: where in which stream, and how it is encoded.
    struct Attribute
    {
        uint32_t binding = 0;        ///< the stream
        uint32_t offset  = 0;        ///< inside the vertex
        VkFormat format  = VK_FORMAT_UNDEFINED;
    };

    /// One texture stage: the image's view and id (texture::Image), its sampler key (CGxTexFlags
    /// bits 0-4 and 9-13) and whether the image is a cube map.
    struct Texture
    {
        VkImageView view       = VK_NULL_HANDLE;
        uint32_t    imageId    = 0;
        uint32_t    samplerKey = 0;
        bool        cube       = false;
    };

    /// SXC: one instanced draw of a run list (the instanced grass, step 7): indices of the bound index
    /// buffer, instances of the instance-rate streams; `copy` picks the copy stream's entry (the grass
    /// density, step 7d: 0 the plants themselves).
    struct Run
    {
        uint32_t firstIndex    = 0;
        uint32_t indexCount    = 0;
        int32_t  vertexOffset  = 0;
        uint32_t firstInstance = 0;
        uint32_t instanceCount = 0;
        uint32_t copy          = 0;
    };

    /// One draw, as the device reads it off the engine.
    struct Draw
    {
        Stream    streams[kMaxStreams];
        uint32_t  streamCount = 0;
        Attribute attribs[kAttribCount];   ///< by EGxVertexAttrib; set where attribMask has the bit
        uint32_t  attribMask  = 0;

        bool     indexed     = false;
        VkBuffer indexBuffer = VK_NULL_HANDLE;   ///< 16-bit indices, bound at offset 0
        uint32_t firstIndex  = 0;
        uint32_t count       = 0;                ///< indices when indexed, vertices otherwise

        uint32_t topology = 3;       ///< EGxPrim
        uint32_t blend    = 0;       ///< EGxBlend

        /// The render states that shape the pipeline, as D3D9 would see them after the master
        /// enables: the depth test (its compare op by the engine's GxRs_DepthFunc value: 0
        /// LESS_OR_EQUAL, 1 EQUAL, 2 GREATER_OR_EQUAL, 3 LESS), the depth write, the culling (0
        /// none, 1 the clockwise face, 2 the counter-clockwise face), the colour write mask (R 1, G
        /// 2, B 4, A 8), the fill mode.
        bool     depthTest  = false;
        bool     depthWrite = false;
        uint32_t depthFunc  = 0;
        uint32_t cull       = 0;
        uint32_t colourMask = 0xF;
        bool     wireframe  = false;

        /// The dynamic states: D3D9's DEPTHBIAS, as Vulkan's constant depth bias factor (units of
        /// the depth buffer's smallest step); D3D9's BLENDFACTOR, for the ConstantAlpha blend.
        float depthBias        = 0.0f;
        float blendConstant[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        VertexProgram           vertex         = VertexProgram::FixedFunction;
        FragmentProgram         fragment       = FragmentProgram::FixedFunction;
        const manifest::Record* vertexRecord   = nullptr;   ///< with VertexProgram::Translated
        const manifest::Record* fragmentRecord = nullptr;   ///< with FragmentProgram::Translated
        uint32_t terrainVertex   = 0;   ///< SXC: with VertexProgram::Terrain, terrain::kVertex* options
        uint32_t terrainFragment = 0;   ///< SXC: with FragmentProgram::Terrain, terrain::kFragment* options
        bool     terrainUnderlay = false; ///< SXC: a raised terrain chunk: draw it flat and lowered first (kVertexSunk)
        uint32_t lightsSurface = 0;       ///< SXC: which of our lights a lit translated module takes (lights::Surface: any, outdoor, indoor, off)

        /// The push constants (Shaders/Push.glsl): the transform (the row-major matrix, or c0..c3),
        /// the parameters (alpha reference, textured stages, colour ops, alpha ops) and the texture
        /// transforms of stages 0 and 1 (identity by default): per stage the columns giving u and v
        /// from (x, y, z, 1), stage s at tex[8s] (u) and tex[8s + 4] (v).
        float xform[16] = {};
        float params[4] = {};
        float tex[16]   = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        /// SXC: D3D9's linear vertex fog for the fixed-function programs: GxRs_FogStart,
        /// GxRs_FogEnd, GxRs_FogColor's RGB as one integer (exact in a float), 1 when on.
        float fog[4]    = {};

        Texture  textures[kTextureStages];   ///< set where textureMask has the bit
        uint32_t textureMask = 0;

        /// The translated interface's constants: the vertex register file (vertexRows rows of four
        /// floats), the pixel register file (kPixelRows rows) and the misc row (alpha reference / 255,
        /// 0, colour op, alpha op). Read when either program is of the translated family.
        const float* vertexRegs = nullptr;
        uint32_t     vertexRows = kVertexRowsSmall;
        const float* pixelRegs  = nullptr;
        float        misc[4]    = {};

        VkViewport viewport = {};
        VkRect2D   scissor  = {};

        /// SXC: the instanced grass (step 7): the streams fed per instance (bit per stream), the stream
        /// whose first 48 bytes are a plant's rows (a grass vertex record reads them at locations
        /// 13..15; -1 none), and the runs recorded instead of the one draw (indexed; `count` ignored).
        /// Step 7d: the copy stream (stride 0, 16 bytes an entry: a run's `copy` binds its entry; -1 none).
        uint32_t   instanceStreams = 0;
        int32_t    plantStream     = -1;
        int32_t    copyStream      = -1;
        const Run* runs     = nullptr;
        uint32_t   runCount = 0;
    };

    /**
     * The shader modules, the descriptor set layouts, the pipeline cache, the per-slot descriptor
     * pools, the uniform ring and the constant stream. After memory::Init(), texture::Init() and
     * shaders::Init(). Idempotent.
     */
    bool Init();

    /// Everything, after the device is idle. Before shaders::Shutdown(), texture::Shutdown() and
    /// memory::Shutdown().
    void Shutdown();

    /// SXC: something else bound its own pipeline, sets and viewport on the main command buffer (an
    /// effect between the engine's draws): the next draw binds everything again.
    void ForgetBound();

    /**
     * Records one draw into an open render pass: the pipeline for the draw's key (built on first
     * use), the descriptor sets of its textures and constants, the vertex and index buffers, the
     * viewport and scissor, the push constants, the draw call. Binds only what changed since the
     * last draw of the same command buffer and frame.
     *
     * @param VkCommandBuffer cmd : the frame's main command buffer, its pass open
     * @param VkRenderPass pass : the pass it is open on (the frame's RenderPass())
     * @param uint32 passGeneration : frame::PassGeneration(); a change discards every pipeline
     * @param uint64 serial : frame::Serial()
     * @param uint32 slot : frame::Slot(), which per-slot pools the frame uses
     * @return bool drawn : false when the draw could not be issued (logged once per reason)
     */
    bool Record(VkCommandBuffer cmd, VkRenderPass pass, uint32_t passGeneration, uint64_t serial, uint32_t slot, const Draw& draw);

    struct Stats
    {
        uint64_t drawn          = 0;
        uint64_t skipped        = 0;   ///< draws refused for any reason below or a missing position
        uint64_t refusedKeys    = 0;   ///< draws whose pipeline could not be built (logged once per reason)
        uint64_t cubeMismatch   = 0;   ///< draws whose texture is not the kind the sampler slot wants
        uint64_t pipelineBuilds = 0;
        uint32_t pipelines      = 0;   ///< live
        uint32_t samplers       = 0;   ///< live
        uint32_t layouts        = 0;   ///< distinct sampler-slot sets
        uint64_t descriptorSets = 0;   ///< set-0 sets allocated over the session
        uint64_t descriptorHits = 0;   ///< draws that found their set written this frame
        uint64_t descriptorFull = 0;   ///< draws refused because the frame's pool was exhausted
        uint64_t regsSlices     = 0;   ///< uniform slices written
        uint64_t regsReused     = 0;   ///< draws that bound the previous slice again
        uint64_t regsBytes      = 0;
        uint64_t regsFull       = 0;   ///< draws refused because the frame's uniform ring was exhausted
    };
    void GetStats(Stats& out);
}
