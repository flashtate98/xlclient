// SXC: a second view of the world (Vulkan/View.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/View.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"

namespace
{
    namespace frame     = gxvk::vk::frame;
    namespace pipelines = gxvk::vk::pipelines;
    namespace texture   = gxvk::vk::texture;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Warn;

    struct State
    {
        texture::Image  image;
        uint64_t        lastUsed  = 0;   ///< the last frame that drew the target
        uint64_t        drawn     = 0;   ///< the frame whose copies it holds
        uint64_t        serial    = 0;   ///< the frame Begin() was called in (a flush between: the copies are dropped)
        VkCommandBuffer cmd       = nullptr;
        bool            recording = false;
        float           clear[4]  = {};
        // The view's transforms (BeginInput), and per projection met (the frame's, the distant terrain's...) what
        // the copies need: the projection as it lies and transposed (how the constant registers carry it), the
        // view's projection the same ways, and what takes the one to the other in clip space (the pushed
        // transforms). Two kept: the frame's and the last other.
        float mirror[16] = {}, plane[4] = {}, skyMirror[16] = {};
        struct Projection
        {
            bool  valid = false;
            float proj[16] = {}, projT[16] = {};
            float view[16] = {}, viewT[16] = {}, toView[16] = {};
            float sky[16] = {}, skyT[16] = {}, toSky[16] = {};
        };
        Projection projections[2];
        bool  drawingSky = false;
        bool  grass = false;
        float grassReach = 200.0f;
        std::vector<pipelines::Run> runs;   // the grass's runs the view takes
        std::unordered_map<uint64_t, uint32_t> rows;   // a shader's projection row (its record, options, program)
        float scaleX = 1.0f, scaleY = 1.0f;
        float regs[pipelines::kVertexRowsLarge * 4] = {};
        gxvk::vk::view::Stats stats, counting;
        bool  loggedNoTarget = false;
    };
    State s;

    void Transpose(const float* m, float* out)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) out[c * 4 + r] = m[r * 4 + c];
    }

    void Multiply(const float* a, const float* b, float* out)
    {
        float r[16];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
        std::memcpy(out, r, sizeof r);
    }

    // A general 4x4 inverse (cofactors); false for a singular matrix.
    bool Invert(const float* m, float* out)
    {
        float inv[16];
        inv[0]  =  m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8]  =  m[4] * m[9]  * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9]  * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5]  =  m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9]  = -m[0] * m[9]  * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] =  m[0] * m[9]  * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2]  =  m[1] * m[6]  * m[15] - m[1] * m[7]  * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7]  - m[13] * m[3] * m[6];
        inv[6]  = -m[0] * m[6]  * m[15] + m[0] * m[7]  * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7]  + m[12] * m[3] * m[6];
        inv[10] =  m[0] * m[5]  * m[15] - m[0] * m[7]  * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7]  - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5]  * m[14] + m[0] * m[6]  * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6]  + m[12] * m[2] * m[5];
        inv[3]  = -m[1] * m[6]  * m[11] + m[1] * m[7]  * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9]  * m[2] * m[7]  + m[9]  * m[3] * m[6];
        inv[7]  =  m[0] * m[6]  * m[11] - m[0] * m[7]  * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8]  * m[2] * m[7]  - m[8]  * m[3] * m[6];
        inv[11] = -m[0] * m[5]  * m[11] + m[0] * m[7]  * m[9]  + m[4] * m[1] * m[11] - m[4] * m[3] * m[9]  - m[8]  * m[1] * m[7]  + m[8]  * m[3] * m[5];
        inv[15] =  m[0] * m[5]  * m[10] - m[0] * m[6]  * m[9]  - m[4] * m[1] * m[10] + m[4] * m[2] * m[9]  + m[8]  * m[1] * m[6]  - m[8]  * m[2] * m[5];
        const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (std::fabs(det) < 1e-20f) return false;
        for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
        return true;
    }

    uint32_t ScaledInt(int32_t v, float scale)
    {
        const float f = static_cast<float>(v) * scale;
        return f > 0.0f ? static_cast<uint32_t>(f + 0.5f) : 0u;
    }
}

namespace
{
    // The view's projection for a draw's projection `p`: the mirror before it, the plane as its near plane
    // (Lengyel's oblique near plane for a [0, 1] depth range: the z column becomes the plane, scaled so the far
    // plane still meets the frustum's far corner on the plane's side). False for a projection that can't be.
    bool Oblique(const float* p, const float* pi, float* out)
    {
        const float* c = s.plane;
        const float sx = c[0] > 0.0f ? 1.0f : (c[0] < 0.0f ? -1.0f : 0.0f);
        const float sy = c[1] > 0.0f ? 1.0f : (c[1] < 0.0f ? -1.0f : 0.0f);
        const float corner[4] = { sx, sy, 1.0f, 1.0f };
        float q[4];
        for (int j = 0; j < 4; ++j) q[j] = corner[0] * pi[j] + corner[1] * pi[4 + j] + corner[2] * pi[8 + j] + corner[3] * pi[12 + j];
        const float dot = c[0] * q[0] + c[1] * q[1] + c[2] * q[2] + c[3] * q[3];
        if (std::fabs(dot) < 1e-12f) return false;
        float o[16];
        std::memcpy(o, p, sizeof o);
        for (int i = 0; i < 4; ++i) o[i * 4 + 2] = c[i] / dot;
        Multiply(s.mirror, o, out);
        return true;
    }

    bool Prepare(State::Projection& e, const float* proj)
    {
        float inverse[16];
        if (!Invert(proj, inverse)) return false;
        std::memcpy(e.proj, proj, sizeof e.proj);
        Transpose(e.proj, e.projT);
        if (!Oblique(e.proj, inverse, e.view)) return false;
        Transpose(e.view, e.viewT);
        Multiply(inverse, e.view, e.toView);
        Multiply(s.skyMirror, e.proj, e.sky);
        Transpose(e.sky, e.skyT);
        Multiply(inverse, e.sky, e.toSky);
        e.valid = true;
        return true;
    }

    // The entry for the draw's projection (the frame's, or made for another), null when it can't be had.
    const State::Projection* For(const float* proj)
    {
        for (State::Projection& e : s.projections)
            if (e.valid && std::memcmp(e.proj, proj, sizeof e.proj) == 0) return &e;
        State::Projection& other = s.projections[1];
        other = State::Projection();
        return Prepare(other, proj) ? &other : nullptr;
    }
}

namespace gxvk::vk::view
{
    bool Begin(const BeginInput& in)
    {
        s.recording = false;
        const texture::Image* scene = frame::Scene();
        if (!scene || scene->extent.width == 0 || scene->extent.height == 0) return false;
        const uint32_t divisor = in.divisor ? in.divisor : 1u;
        const VkExtent2D want = { scene->extent.width / divisor > 0 ? scene->extent.width / divisor : 1u,
                                  scene->extent.height / divisor > 0 ? scene->extent.height / divisor : 1u };

        // Retired with this frame: its descriptors (the water's set, written at the frame's start) may name it still.
        if (s.image.handle != VK_NULL_HANDLE && (s.image.extent.width != want.width || s.image.extent.height != want.height || s.image.format != scene->format))
        {
            frame::RetireImage(s.image, frame::Serial());
            s.drawn = 0;
        }
        if (s.image.handle == VK_NULL_HANDLE)
        {
            if (!texture::CreateTarget(want.width, want.height, 1, false, scene->format, s.image))
            {
                if (!s.loggedNoTarget) Write(Warn, "vulkan: view: no %ux%u target", want.width, want.height);
                s.loggedNoTarget = true;
                return false;
            }
            Write(Debug, "vulkan: view: target %ux%u made", want.width, want.height);
        }

        std::memcpy(s.mirror, in.mirror, sizeof s.mirror);
        std::memcpy(s.plane, in.plane, sizeof s.plane);
        std::memcpy(s.skyMirror, in.sky, sizeof s.skyMirror);
        s.projections[0] = State::Projection();
        s.projections[1] = State::Projection();
        if (!Prepare(s.projections[0], in.proj)) return false;
        s.drawingSky = false;
        s.grass = in.grass;
        s.grassReach = in.grassReach;

        s.cmd = frame::BeginSecondary(s.image);
        if (!s.cmd) return false;
        std::memcpy(s.clear, in.clear, sizeof s.clear);
        s.scaleX    = static_cast<float>(want.width) / static_cast<float>(scene->extent.width);
        s.scaleY    = static_cast<float>(want.height) / static_cast<float>(scene->extent.height);
        s.counting  = Stats();
        s.counting.width  = want.width;
        s.counting.height = want.height;
        s.serial    = frame::Serial();
        s.recording = true;
        return true;
    }

    bool Recording() { return s.recording; }

    void Sky(bool on) { s.drawingSky = on; }

    void Copy(const pipelines::Draw& draw, const float proj[16])
    {
        if (!s.recording) return;
        if (frame::Serial() != s.serial)
        {
            s.recording = false;   // the frame was submitted in between (a blocking read): its buffer went with it
            return;
        }
        using VP = pipelines::VertexProgram;
        using FP = pipelines::FragmentProgram;

        // The interface, the projected textures (they read the screen's terrain depth) and the water itself
        // stay the screen's.
        if (draw.vertex == VP::Ui || draw.vertex == VP::Decal || draw.vertex == VP::Water || draw.fragment == FP::Ui || draw.fragment == FP::Desaturate
            || draw.fragment == FP::Decal || draw.fragment == FP::Water)
        {
            ++s.counting.left;
            return;
        }

        // The grass (instanced runs) only on request: in a reflection it barely shows, and it is most of the
        // triangles (Seyri, 2026-11-05: the mirror's pass cost as much as the scene's over Darkshore).
        if (draw.runs && !s.grass)
        {
            ++s.counting.left;
            return;
        }

        pipelines::Draw c = draw;

        // The grass's plants near the player only, without their density copies: a reflection shows little of
        // it, and its far plants and the copies near the player are most of its triangles.
        if (draw.runs)
        {
            s.runs.clear();
            for (uint32_t r = 0; r < draw.runCount; ++r)
                if (draw.runs[r].copy == 0 && draw.runs[r].distance <= s.grassReach) s.runs.push_back(draw.runs[r]);
            if (s.runs.empty())
            {
                ++s.counting.left;
                return;
            }
            c.runs     = s.runs.data();
            c.runCount = static_cast<uint32_t>(s.runs.size());
        }

        if (draw.vertex == VP::FixedFunction || draw.vertex == VP::FixedFunctionSemantic)
        {
            // World x View x Proj (the device's projection now), times Proj^-1 x the view's projection.
            const State::Projection* e = For(proj);
            if (!e)
            {
                ++s.counting.noProjection;
                return;
            }
            Multiply(draw.xform, s.drawingSky ? e->toSky : e->toView, c.xform);
        }
        else
        {
            if (!draw.vertexRegs)
            {
                ++s.counting.noProjection;
                return;
            }
            // The projection the registers carry: the device's now, or the frame's (the shader effects take it
            // once a frame, while the distant terrain sets its own).
            const uint32_t rows = draw.vertexRows == pipelines::kVertexRowsLarge ? pipelines::kVertexRowsLarge : pipelines::kVertexRowsSmall;
            std::memcpy(s.regs, draw.vertexRegs, rows * 16);
            const State::Projection* candidates[2] = { For(proj), &s.projections[0] };
            bool found = false;

            // Where this shader keeps its projection, learnt from its first draw: looked at first (the search
            // through every row, per draw, cost milliseconds over a town's models).
            const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(draw.vertexRecord)) << 8)
                                 ^ (static_cast<uint64_t>(draw.terrainVertex) << 4) ^ static_cast<uint64_t>(draw.vertex);
            const auto hint = s.rows.find(key);
            if (hint != s.rows.end())
            {
                const uint32_t r = hint->second;
                for (const State::Projection* e : candidates)
                {
                    if (!e || !e->valid || found || r + 4 > rows) continue;
                    float* at = &s.regs[r * 4];
                    if (std::memcmp(at, e->proj, 64) == 0) std::memcpy(at, s.drawingSky ? e->sky : e->view, 64);
                    else if (std::memcmp(at, e->projT, 64) == 0) std::memcpy(at, s.drawingSky ? e->skyT : e->viewT, 64);
                    else continue;
                    found = true;
                }
            }

            for (const State::Projection* e : candidates)
            {
                if (!e || !e->valid || found) continue;
                const float* rep  = s.drawingSky ? e->sky : e->view;
                const float* repT = s.drawingSky ? e->skyT : e->viewT;
                for (uint32_t r = 0; r + 4 <= rows; ++r)
                {
                    float* at = &s.regs[r * 4];
                    if (std::memcmp(at, e->proj, 64) == 0) std::memcpy(at, rep, 64);
                    else if (std::memcmp(at, e->projT, 64) == 0) std::memcpy(at, repT, 64);
                    else continue;
                    if (!found) s.rows[key] = r;
                    found = true;
                    r += 3;
                }
            }
            if (!found)
            {
                ++s.counting.noProjection;
                return;
            }
            c.vertexRegs = s.regs;
        }

        if (draw.fragment == FP::Terrain) c.terrainFragment |= pipelines::terrain::kFragmentSecondView;
        c.cull = draw.cull == 1u ? 2u : draw.cull == 2u ? 1u : 0u;   // a mirror turns the winding over
        c.viewport.x      *= s.scaleX;
        c.viewport.y      *= s.scaleY;
        c.viewport.width  *= s.scaleX;
        c.viewport.height *= s.scaleY;
        c.scissor.offset.x      = static_cast<int32_t>(ScaledInt(draw.scissor.offset.x, s.scaleX));
        c.scissor.offset.y      = static_cast<int32_t>(ScaledInt(draw.scissor.offset.y, s.scaleY));
        c.scissor.extent.width  = ScaledInt(static_cast<int32_t>(draw.scissor.offset.x + draw.scissor.extent.width), s.scaleX) - static_cast<uint32_t>(c.scissor.offset.x);
        c.scissor.extent.height = ScaledInt(static_cast<int32_t>(draw.scissor.offset.y + draw.scissor.extent.height), s.scaleY) - static_cast<uint32_t>(c.scissor.offset.y);

        if (pipelines::Record(s.cmd, frame::RenderPass(), frame::PassGeneration(), frame::Serial(), frame::Slot(), c)) ++s.counting.copied;
    }

    void End()
    {
        if (!s.recording) return;
        s.recording = false;
        if (frame::Serial() == s.serial && frame::RunSecondary(s.image, s.cmd, s.clear))
        {
            s.lastUsed = frame::Serial();
            s.drawn    = s.lastUsed;
        }
        // After vkCmdExecuteCommands the main buffer's bound state is undefined: its next draw binds it all again.
        pipelines::ForgetBound();
        s.cmd   = nullptr;
        s.stats = s.counting;
    }

    VkImageView Image(uint64_t serial)
    {
        return s.image.handle != VK_NULL_HANDLE && s.drawn == serial ? s.image.view : VK_NULL_HANDLE;
    }

    VkImageView Kept()
    {
        return s.image.handle != VK_NULL_HANDLE && s.drawn != 0 ? s.image.view : VK_NULL_HANDLE;
    }

    Stats GetStats() { return s.stats; }

    void Shutdown()
    {
        if (s.image.handle != VK_NULL_HANDLE) texture::DestroyIdle(s.image);
        s = State();
    }
}
