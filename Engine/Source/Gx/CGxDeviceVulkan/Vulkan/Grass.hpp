// SXC: the instanced grass's buffers (port plan step 7; SXC/Grass). The plant models' vertices and
// indices in one buffer pair (each model once, appended), and every chunk's plants in one pool (a plant
// is its placement's three rows, its normal and its colour: 64 bytes), so all of a texture's plants draw
// in one bind (the draw itself: CGxDeviceVulkan::SxcGrassDraw). A freed range of the pool is reused once
// the frames that may still read it have completed. The density's copies (step 7d) are the same runs drawn
// again, each with its copy number from a small constant buffer.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::grass
{
    /// rows 0..2 (48), normal (12), colour (4: B, G, R, A), motion (16: root V, 1 / span, wind, push), density (8:
    /// copies, radius, spread, 0 as halves), the spot in its terrain cell (8: 2 x triangle + s, t)
    constexpr uint32_t kPlantBytes   = 96;
    constexpr uint32_t kVertexBytes  = 20;       ///< position (12), texture coordinate (8)
    constexpr uint32_t kMaxPlants    = 1u << 20; ///< the pool (96 MB)
    constexpr uint32_t kMaxCopies    = 15;       ///< step 7d: copy numbers 1..15 (the copy stream's entries 0..15)
    constexpr uint32_t kMaxVertices  = 1u << 18; ///< the plant models' vertices, all together
    constexpr uint32_t kMaxIndices   = 1u << 20;

    bool Init();
    bool Ready();

    /// A plant model's geometry, appended once (by the caller's key: the same key answers the same
    /// place). False when the buffers are full.
    bool Mesh(uint32_t key, const float* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount, uint32_t& firstIndex,
              int32_t& vertexOffset, uint32_t& indexCountOut);

    /// `count` plants of the pool; false when it has no room (the caller draws those the stock way).
    bool Allocate(uint32_t count, uint32_t& first);
    /// Writes plants into an allocated range (the frames in flight never read a range before it is written).
    void Write(uint32_t first, const void* plants, uint32_t count);
    /// Gives a range back; reused once the frames that drew it have completed.
    void Release(uint32_t first, uint32_t count);

    VkBuffer MeshVertices();
    VkBuffer MeshIndices();
    VkBuffer Plants();
    /// Step 7d: entry k (16 bytes) holds the copy number k; read at stride 0, so a run's copies all read
    /// the entry its offset picks.
    VkBuffer Copies();

    /// The pool's use (plants), for the status line.
    uint32_t PlantsInUse();

    void Shutdown();
}
