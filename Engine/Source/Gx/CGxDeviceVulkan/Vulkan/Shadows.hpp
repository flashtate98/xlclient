// SXC: the lights' cube shadow maps (port plan step 6c). One depth image array holds six faces per slot;
// a slot is drawn (in the frame's pre-pass, ahead of every draw that reads it) from the WMO and terrain
// triangles SXC/Lights gathered around a light, depth only (Shaders/LightShadow.vert), and kept until the
// light or its surroundings change. Read by Shaders/Lights.glsl through the lights' set 3 (binding 3).
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::shadows
{
    constexpr uint32_t kMaxSlots   = 64;
    constexpr uint32_t kFaceSize   = 256;      ///< texels per face side
    constexpr float    kNear       = 0.25f;    ///< yd: nothing closer to a light casts a shadow (a sconce's wall, a lantern's frame)
    constexpr uint32_t kMaxVertices = 512 * 1024; ///< per frame, all slots drawn in it together (20 bytes each)
    constexpr uint32_t kMaxTexturedParts = 512;   ///< alpha-keyed parts per frame

    /// A run of a job's vertices drawn alike: solid, or alpha-keyed by a texture (see-through texels cast
    /// nothing).
    struct Part
    {
        uint32_t    firstVertex = 0;   ///< into the job's vertices
        uint32_t    vertexCount = 0;
        VkImageView texture = VK_NULL_HANDLE; ///< null: solid; else its alpha keys (SHADER_READ_ONLY_OPTIMAL)
    };

    /// One slot to draw: triangles (three vertices each: xyz relative to the light, then u, v) out to
    /// `radius`. Without parts every vertex is solid.
    struct Job
    {
        uint32_t     slot = 0;
        float        radius = 0.0f;
        const float* vertices = nullptr;
        uint32_t     vertexCount = 0;
        const Part*  parts = nullptr;
        uint32_t     partCount = 0;
    };

    /// The image, its sampler and the pipeline; false when they can't be had (the lights then can't either).
    bool Init();
    bool Ready();

    /// Slots this device holds (0 before Init(); fewer than kMaxSlots where image arrays are short).
    uint32_t Slots();

    /// The whole array (2D array view, the depth aspect) in DEPTH_STENCIL_READ_ONLY_OPTIMAL, and the
    /// comparing sampler (less or equal: 1 lit).
    VkImageView View();
    VkSampler Sampler();

    /**
     * Records the jobs into this frame's pre-pass (the first call also puts every layer in its resting
     * layout). Jobs are drawn in order while the frame's vertex room lasts.
     *
     * @return uint32 drawn : how many of the jobs were drawn (the first ones)
     */
    uint32_t Record(const Job* jobs, uint32_t count);

    void Shutdown();
}
