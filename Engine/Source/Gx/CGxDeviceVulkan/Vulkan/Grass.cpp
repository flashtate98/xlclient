// SXC: the instanced grass's buffers (Vulkan/Grass.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Grass.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
    namespace frame  = gxvk::vk::frame;
    namespace memory = gxvk::vk::memory;
    using namespace gxvk::vk::grass;

    using gxvk::log::Write;
    using gxvk::log::Info;
    using gxvk::log::Error;

    struct Range
    {
        uint32_t first;
        uint32_t count;
    };

    struct Pending
    {
        Range    range;
        uint64_t serial;   ///< the frame that may still read it
    };

    struct MeshPlace
    {
        uint32_t firstIndex;
        int32_t  vertexOffset;
        uint32_t indexCount;
    };

    struct State
    {
        bool           ready  = false;
        bool           failed = false;
        memory::Buffer vertices, indices, plants, copies;
        uint32_t       vertexUsed = 0, indexUsed = 0;
        std::unordered_map<uint32_t, MeshPlace> meshes;
        std::vector<Range>   free;       ///< sorted by first, merged
        std::vector<Pending> pending;
        uint32_t       inUse = 0;
    };
    State s;

    // A range into the free list, merged with its neighbours.
    void Free(Range r)
    {
        auto it = std::lower_bound(s.free.begin(), s.free.end(), r.first, [](const Range& a, uint32_t f) { return a.first < f; });
        it = s.free.insert(it, r);
        if (it + 1 != s.free.end() && it->first + it->count == (it + 1)->first)
        {
            it->count += (it + 1)->count;
            s.free.erase(it + 1);
        }
        if (it != s.free.begin() && (it - 1)->first + (it - 1)->count == it->first)
        {
            (it - 1)->count += it->count;
            s.free.erase(it);
        }
    }
}

namespace gxvk::vk::grass
{
    bool Ready() { return s.ready; }
    VkBuffer MeshVertices() { return s.vertices.handle; }
    VkBuffer MeshIndices() { return s.indices.handle; }
    VkBuffer Plants() { return s.plants.handle; }
    VkBuffer Copies() { return s.copies.handle; }
    uint32_t PlantsInUse() { return s.inUse; }

    bool Init()
    {
        if (s.ready) return true;
        if (s.failed) return false;
        if (!memory::CreateBuffer(static_cast<VkDeviceSize>(kMaxVertices) * kVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory::Where::Stream, s.vertices)
            || !s.vertices.block.mapped
            || !memory::CreateBuffer(static_cast<VkDeviceSize>(kMaxIndices) * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, memory::Where::Stream, s.indices)
            || !s.indices.block.mapped
            || !memory::CreateBuffer(static_cast<VkDeviceSize>(kMaxPlants) * kPlantBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory::Where::Stream, s.plants)
            || !s.plants.block.mapped
            || !memory::CreateBuffer(static_cast<VkDeviceSize>(kMaxCopies + 1) * 16u, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory::Where::Stream, s.copies)
            || !s.copies.block.mapped)
        {
            Write(Error, "vulkan: grass: no buffers for the instanced grass");
            memory::DestroyBuffer(s.vertices);
            memory::DestroyBuffer(s.indices);
            memory::DestroyBuffer(s.plants);
            memory::DestroyBuffer(s.copies);
            s.failed = true;
            return false;
        }
        float* copy = static_cast<float*>(s.copies.block.mapped);
        for (uint32_t k = 0; k <= kMaxCopies; ++k)
        {
            copy[4 * k + 0] = static_cast<float>(k);
            copy[4 * k + 1] = copy[4 * k + 2] = copy[4 * k + 3] = 0.0f;
        }
        memory::FlushHost(s.copies.block);
        s.free.assign(1, Range{ 0, kMaxPlants });
        s.ready = true;
        Write(Info, "vulkan: grass: ready (%u plants, %u model vertices)", kMaxPlants, kMaxVertices);
        return true;
    }

    bool Mesh(uint32_t key, const float* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount, uint32_t& firstIndex,
              int32_t& vertexOffset, uint32_t& indexCountOut)
    {
        if (!s.ready) return false;
        const auto it = s.meshes.find(key);
        if (it != s.meshes.end())
        {
            firstIndex    = it->second.firstIndex;
            vertexOffset  = it->second.vertexOffset;
            indexCountOut = it->second.indexCount;
            return true;
        }
        if (!vertices || !indices || !vertexCount || !indexCount || s.vertexUsed + vertexCount > kMaxVertices || s.indexUsed + indexCount > kMaxIndices) return false;

        // Appended: nothing drawn before reads these bytes.
        std::memcpy(static_cast<uint8_t*>(s.vertices.block.mapped) + static_cast<size_t>(s.vertexUsed) * kVertexBytes, vertices,
                    static_cast<size_t>(vertexCount) * kVertexBytes);
        std::memcpy(static_cast<uint16_t*>(s.indices.block.mapped) + s.indexUsed, indices, static_cast<size_t>(indexCount) * sizeof(uint16_t));
        memory::FlushHost(s.vertices.block);
        memory::FlushHost(s.indices.block);
        const MeshPlace place{ s.indexUsed, static_cast<int32_t>(s.vertexUsed), indexCount };
        s.meshes.emplace(key, place);
        s.vertexUsed += vertexCount;
        s.indexUsed += indexCount;
        firstIndex    = place.firstIndex;
        vertexOffset  = place.vertexOffset;
        indexCountOut = place.indexCount;
        return true;
    }

    bool Allocate(uint32_t count, uint32_t& first)
    {
        if (!s.ready || !count) return false;

        // Ranges the GPU is done with go back to the free list first.
        const uint64_t done = frame::Completed();
        for (size_t i = 0; i < s.pending.size();)
        {
            if (s.pending[i].serial <= done)
            {
                Free(s.pending[i].range);
                s.pending[i] = s.pending.back();
                s.pending.pop_back();
            }
            else
                ++i;
        }

        for (size_t i = 0; i < s.free.size(); ++i)
        {
            Range& r = s.free[i];
            if (r.count < count) continue;
            first = r.first;
            r.first += count;
            r.count -= count;
            if (!r.count) s.free.erase(s.free.begin() + static_cast<std::ptrdiff_t>(i));
            s.inUse += count;
            return true;
        }
        return false;
    }

    void Write(uint32_t first, const void* plants, uint32_t count)
    {
        if (!s.ready || !plants || first + count > kMaxPlants) return;
        std::memcpy(static_cast<uint8_t*>(s.plants.block.mapped) + static_cast<size_t>(first) * kPlantBytes, plants, static_cast<size_t>(count) * kPlantBytes);
        memory::FlushHost(s.plants.block);
    }

    void Release(uint32_t first, uint32_t count)
    {
        if (!s.ready || !count) return;
        s.pending.push_back(Pending{ Range{ first, count }, frame::Serial() });
        s.inUse -= std::min(s.inUse, count);
    }

    void Shutdown()
    {
        memory::DestroyBuffer(s.vertices);
        memory::DestroyBuffer(s.indices);
        memory::DestroyBuffer(s.plants);
        memory::DestroyBuffer(s.copies);
        s = State();
    }
}
