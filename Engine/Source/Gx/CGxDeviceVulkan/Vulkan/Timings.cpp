// SXC: GPU timestamps for a frame profiler (Vulkan/Timings.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Timings.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"

#include <cstdio>
#include <vector>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace timings = gxvk::vk::timings;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Warn;
    using gxvk::vk::ResultName;

    /// Per slot: the pre-pass lane's stamps first, then the main lane's. A stamp closes the interval
    /// since the one before it, for the category it carries.
    constexpr uint32_t kPreStamps  = 32;
    constexpr uint32_t kMainStamps = 2048;
    constexpr uint32_t kStamps     = kPreStamps + kMainStamps;

    struct Lane
    {
        uint32_t first = 0;   ///< its first query in the pool
        uint32_t cap   = 0;
        uint32_t used  = 0;
        int      current = 0; ///< the category the open interval belongs to
        int      cats[kMainStamps] = {};
        bool     full  = false;
    };

    struct Slot
    {
        VkQueryPool pool    = VK_NULL_HANDLE;
        bool        pending = false;   ///< stamped and submitted; read when the slot opens again
        uint32_t    preUsed = 0, mainUsed = 0;
        int         preCats[kPreStamps] = {};
        int         mainCats[kMainStamps] = {};
    };

    struct State
    {
        bool                  ready     = false;
        bool                  tried     = false;
        VkDevice              device    = nullptr;
        const loader::Device* dapi      = nullptr;
        double                msPerTick = 0.0;
        uint64_t              mask      = ~0ull;   ///< the valid bits of a timestamp
        Slot                  slots[frame::kFramesInFlight];
        timings::Sink         sink = nullptr;
        bool                  want = false;   ///< Enable()
        bool                  open = false;   ///< this frame is stamped
        uint32_t              slot = 0;
        VkCommandBuffer       pre  = nullptr;
        VkCommandBuffer       main = nullptr;
        Lane                  preLane;
        Lane                  mainLane;
        int                   mainCategory = 0;   ///< kept between frames: the next frame starts in it
        uint64_t              read = 0, notReady = 0, overflows = 0;
        char                  status[160] = "";
    };
    State s;

    /// A stamp closing `closes`' interval (-1: the lane's start). The last query is kept for the frame's end.
    void Stamp(VkCommandBuffer cmd, Lane& lane, int closes, bool end = false)
    {
        if (lane.used >= (end ? lane.cap : lane.cap - 1))
        {
            lane.full = true;
            return;
        }
        s.dapi->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.slots[s.slot].pool, lane.first + lane.used);
        lane.cats[lane.used] = closes;
        ++lane.used;
    }

    /// Adds a lane's intervals to `out` (indexed by category, `count` entries).
    bool Sum(VkQueryPool pool, uint32_t first, uint32_t used, const int* cats, double* out, int count)
    {
        if (used < 2) return true;
        static std::vector<uint64_t> t;
        t.resize(used);
        const VkResult r = s.dapi->GetQueryPoolResults(s.device, pool, first, used, used * sizeof(uint64_t), t.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS) return false;
        for (uint32_t k = 1; k < used; ++k)
        {
            const int c = cats[k];
            if (c < 0 || c >= count) continue;
            const uint64_t ticks = (t[k] - t[k - 1]) & s.mask;
            out[c] += static_cast<double>(ticks) * s.msPerTick;
        }
        return true;
    }

    void ReadSlot(Slot& slot)
    {
        if (!slot.pending) return;
        slot.pending = false;
        if (!s.sink) return;
        double main[timings::kMaxCategories] = {};
        double pre[static_cast<int>(timings::Pre::Count)] = {};
        if (!Sum(slot.pool, 0, slot.preUsed, slot.preCats, pre, static_cast<int>(timings::Pre::Count))
            || !Sum(slot.pool, kPreStamps, slot.mainUsed, slot.mainCats, main, timings::kMaxCategories))
        {
            ++s.notReady;
            return;
        }
        ++s.read;
        s.sink(main, pre);
    }
}

namespace gxvk::vk::timings
{
    bool Init()
    {
        if (s.ready) return true;
        if (s.tried) return false;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi || !h->iapi) return false;
        s.tried = true;

        // The graphics family's valid bits (0: no timestamps there) and the tick's length.
        uint32_t familyCount = 0;
        h->iapi->GetPhysicalDeviceQueueFamilyProperties(h->physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        h->iapi->GetPhysicalDeviceQueueFamilyProperties(h->physical, &familyCount, families.data());
        const uint32_t bits = h->graphicsFamily < familyCount ? families[h->graphicsFamily].timestampValidBits : 0;
        VkPhysicalDeviceProperties props = {};
        h->iapi->GetPhysicalDeviceProperties(h->physical, &props);
        if (bits == 0 || props.limits.timestampPeriod <= 0.0f)
        {
            std::snprintf(s.status, sizeof s.status, "the graphics queue has no timestamps");
            Write(Debug, "vulkan: timings: %s", s.status);
            return false;
        }

        s.device    = h->device;
        s.dapi      = h->dapi;
        s.mask      = bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);
        s.msPerTick = static_cast<double>(props.limits.timestampPeriod) * 1e-6;
        for (Slot& slot : s.slots)
        {
            VkQueryPoolCreateInfo info = {};
            info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kStamps;
            const VkResult r = s.dapi->CreateQueryPool(s.device, &info, nullptr, &slot.pool);
            if (r != VK_SUCCESS)
            {
                slot.pool = VK_NULL_HANDLE;
                Write(Warn, "vulkan: timings: vkCreateQueryPool failed: %s; no GPU times", ResultName(r));
                Shutdown();
                s.tried = true;
                return false;
            }
        }
        s.preLane.first  = 0;
        s.preLane.cap    = kPreStamps;
        s.mainLane.first = kPreStamps;
        s.mainLane.cap   = kMainStamps;
        s.ready = true;
        Write(Debug, "vulkan: timings: %u valid bits, %.3f ns a tick", bits, props.limits.timestampPeriod);
        return true;
    }

    void Shutdown()
    {
        if (s.dapi && s.device)
        {
            for (Slot& slot : s.slots)
                if (slot.pool != VK_NULL_HANDLE) s.dapi->DestroyQueryPool(s.device, slot.pool, nullptr);
        }
        const Sink sink = s.sink;
        const bool want = s.want;
        s = State();
        s.sink = sink;
        s.want = want;
    }

    void SetSink(Sink sink) { s.sink = sink; }

    void Enable(bool on) { s.want = on; }

    void Mark(int category)
    {
        if (category < 0 || category >= kMaxCategories) return;
        if (s.open && category != s.mainLane.current)
        {
            Stamp(s.main, s.mainLane, s.mainLane.current);
            s.mainLane.current = category;
        }
        s.mainCategory = category;
    }

    void PreMark(Pre kind)
    {
        const int k = static_cast<int>(kind);
        if (!s.open || k == s.preLane.current) return;
        Stamp(s.pre, s.preLane, s.preLane.current);
        s.preLane.current = k;
    }

    void FrameOpened(uint32_t slot, VkCommandBuffer pre, VkCommandBuffer main)
    {
        s.open = false;
        if (!s.ready || slot >= frame::kFramesInFlight) return;
        Slot& sl = s.slots[slot];
        ReadSlot(sl);
        if (!s.want || !s.sink || !pre || !main) return;

        s.slot = slot;
        s.pre  = pre;
        s.main = main;
        s.open = true;
        s.dapi->CmdResetQueryPool(pre, sl.pool, 0, kStamps);
        s.preLane.used  = 0;
        s.preLane.full  = false;
        s.mainLane.used = 0;
        s.mainLane.full = false;
        Stamp(pre, s.preLane, -1);
        s.preLane.current = static_cast<int>(Pre::Uploads);
        Stamp(main, s.mainLane, -1);
        s.mainLane.current = s.mainCategory;
    }

    void FrameClosing()
    {
        if (!s.open) return;
        s.open = false;
        Stamp(s.pre, s.preLane, s.preLane.current, true);
        Stamp(s.main, s.mainLane, s.mainLane.current, true);
        if (s.preLane.full || s.mainLane.full) ++s.overflows;

        Slot& sl = s.slots[s.slot];
        sl.preUsed  = s.preLane.used;
        sl.mainUsed = s.mainLane.used;
        for (uint32_t k = 0; k < sl.preUsed; ++k) sl.preCats[k] = s.preLane.cats[k];
        for (uint32_t k = 0; k < sl.mainUsed; ++k) sl.mainCats[k] = s.mainLane.cats[k];
        sl.pending = true;
    }

    const char* Status()
    {
        if (s.ready)
            std::snprintf(s.status, sizeof s.status, "Vulkan timestamps, %.2f ns a tick; %llu frames read, %llu not ready, %llu with too many stamps",
                          s.msPerTick * 1e6, static_cast<unsigned long long>(s.read), static_cast<unsigned long long>(s.notReady),
                          static_cast<unsigned long long>(s.overflows));
        else if (!s.status[0])
            return "not started";
        return s.status;
    }
}
