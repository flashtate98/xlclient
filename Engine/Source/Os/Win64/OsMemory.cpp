#if defined(WOWDECOMP_ARCH_64)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>
#include <windows.h>
#include "Engine/Source/Os/OsMemory.h"

static const uintptr_t s_nearRange = 0x60000000;

// SXC: blocks near an anchor (a model's data: its .anim files and vertices take 32-bit offsets from it)
// come from arenas, 64 MB reserved near some anchor and handed out in pieces. Before, every block was
// its own VirtualAlloc (64 KB at the least) found by walking the address space region by region from
// the anchor, and every block added a region: the walk grew with every file loaded, to freezes of 11
// and 30 s at a unit's first animation (the watchdog, 2026-11-03). The walk now only places a new arena.
// Blocks bigger than an arena keep their own VirtualAlloc. Blocks are zeroed (VirtualAlloc's were).
namespace {
    const uintptr_t kArenaSize = 64u * 1024u * 1024u;
    const uintptr_t kAlign = 16;

    // Where no 64 MB gap is left near an anchor, smaller arenas, before a reservation per block.
    const uintptr_t kArenaSizes[] = { kArenaSize, 16u * 1024u * 1024u, 4u * 1024u * 1024u };

    struct Arena {
        uintptr_t base = 0;
        uintptr_t size = 0;
        uintptr_t committed = 0;               // bytes committed from base
        std::map<uintptr_t, uintptr_t> free;   // offset -> size, neighbours merged
        std::map<uintptr_t, uintptr_t> used;   // offset -> size
    };

    std::vector<Arena*> s_arenas;
    SRWLOCK s_lock = SRWLOCK_INIT;

    // `bytes` (granularity-rounded) of free space within the near range of the anchor, as near to it as
    // the free gaps allow, reserved (and committed when `commit`). One pass upward through the whole range:
    // VirtualQuery describes a region from the queried page upward, so the walk down from the anchor it
    // replaces moved a page per call and never saw a gap bigger than a page - ~390,000 calls for nothing on
    // every new arena once the space above the anchor was full (freezes of 30 s and more, 2026-11-13).
    uintptr_t FindNear(uintptr_t center, uintptr_t bytes, bool commit) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        const uintptr_t granularity = info.dwAllocationGranularity;
        bytes = (bytes + granularity - 1) & ~(granularity - 1);
        uintptr_t minimum = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
        uintptr_t maximum = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
        const DWORD type = commit ? (MEM_RESERVE | MEM_COMMIT) : MEM_RESERVE;
        const DWORD protect = commit ? PAGE_READWRITE : PAGE_NOACCESS;

        if (center > minimum + s_nearRange) {
            minimum = center - s_nearRange;
        }

        if (center + s_nearRange < maximum) {
            maximum = center + s_nearRange;
        }

        // Each free gap's place nearest the anchor, tried nearest first (another thread may take one first).
        std::vector<std::pair<uintptr_t, uintptr_t>> candidates;   // distance, address
        MEMORY_BASIC_INFORMATION region;
        uintptr_t address = minimum;

        while (address < maximum && VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
            const uintptr_t end = base + region.RegionSize;

            if (end <= address) {
                break;
            }

            if (region.State == MEM_FREE) {
                const uintptr_t low = ((base > minimum ? base : minimum) + granularity - 1) & ~(granularity - 1);
                const uintptr_t high = end < maximum ? end : maximum;

                if (low + bytes <= high) {
                    const uintptr_t last = (high - bytes) & ~(granularity - 1);
                    uintptr_t at = center & ~(granularity - 1);
                    at = at < low ? low : at > last ? last : at;
                    candidates.emplace_back(at > center ? at - center : center - at, at);
                }
            }

            address = end;
        }

        std::sort(candidates.begin(), candidates.end());

        for (const auto& c : candidates) {
            if (void* ptr = VirtualAlloc(reinterpret_cast<void*>(c.second), bytes, type, protect)) {
                return reinterpret_cast<uintptr_t>(ptr);
            }
        }

        return 0;
    }

    // Whether all of [base, base + size) lies within the near range of the anchor.
    bool Near(uintptr_t anchor, uintptr_t base, uintptr_t size) {
        const uintptr_t low = anchor > s_nearRange ? anchor - s_nearRange : 0;
        return base >= low && base + size <= anchor + s_nearRange;
    }

    void Give(Arena& a, uintptr_t offset, uintptr_t bytes);

    // A piece of the arena (first fit), committed and zeroed; 0 when it has no room.
    uintptr_t Take(Arena& a, uintptr_t bytes) {
        for (auto it = a.free.begin(); it != a.free.end(); ++it) {
            if (it->second < bytes) {
                continue;
            }

            const uintptr_t offset = it->first;
            const uintptr_t left = it->second - bytes;
            a.free.erase(it);

            if (left) {
                a.free.emplace(offset + bytes, left);
            }

            const uintptr_t end = offset + bytes;

            if (end > a.committed) {
                const uintptr_t page = 64u * 1024u;
                const uintptr_t to = (end + page - 1) & ~(page - 1);

                if (!VirtualAlloc(reinterpret_cast<void*>(a.base + a.committed), to - a.committed, MEM_COMMIT, PAGE_READWRITE)) {
                    Give(a, offset, bytes);
                    return 0;
                }

                a.committed = to;
            }

            a.used.emplace(offset, bytes);
            std::memset(reinterpret_cast<void*>(a.base + offset), 0, bytes);
            return a.base + offset;
        }

        return 0;
    }

    // The piece back into the free map, merged with its neighbours.
    void Give(Arena& a, uintptr_t offset, uintptr_t bytes) {
        auto next = a.free.lower_bound(offset);

        if (next != a.free.end() && offset + bytes == next->first) {
            bytes += next->second;
            next = a.free.erase(next);
        }

        if (next != a.free.begin()) {
            auto prev = std::prev(next);

            if (prev->first + prev->second == offset) {
                prev->second += bytes;
                return;
            }
        }

        a.free.emplace(offset, bytes);
    }
}

void* OsAllocNear(const void* anchor, size_t size) {
    const uintptr_t center = reinterpret_cast<uintptr_t>(anchor);
    const uintptr_t bytes = (static_cast<uintptr_t>(size ? size : 1) + kAlign - 1) & ~(kAlign - 1);

    // Too big for an arena: its own reservation, as before.
    if (bytes > kArenaSize / 4) {
        return reinterpret_cast<void*>(FindNear(center, bytes, true));
    }

    AcquireSRWLockExclusive(&s_lock);

    for (Arena* a : s_arenas) {
        if (Near(center, a->base, a->size)) {
            if (uintptr_t p = Take(*a, bytes)) {
                ReleaseSRWLockExclusive(&s_lock);
                return reinterpret_cast<void*>(p);
            }
        }
    }

    uintptr_t p = 0;

    for (uintptr_t size : kArenaSizes) {
        if (bytes > size / 4) {
            break;
        }

        if (uintptr_t base = FindNear(center, size, false)) {
            Arena* a = new Arena();
            a->base = base;
            a->size = size;
            a->free.emplace(0, size);
            s_arenas.push_back(a);
            p = Take(*a, bytes);
            break;
        }
    }

    ReleaseSRWLockExclusive(&s_lock);

    // No room for an arena near the anchor: a reservation of its own, as before.
    return p ? reinterpret_cast<void*>(p) : reinterpret_cast<void*>(FindNear(center, bytes, true));
}

void OsFreeNear(void* ptr) {
    if (!ptr) {
        return;
    }

    const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    AcquireSRWLockExclusive(&s_lock);

    for (Arena* a : s_arenas) {
        if (p >= a->base && p < a->base + a->size) {
            auto it = a->used.find(p - a->base);

            if (it != a->used.end()) {
                Give(*a, it->first, it->second);
                a->used.erase(it);
            }

            ReleaseSRWLockExclusive(&s_lock);
            return;
        }
    }

    ReleaseSRWLockExclusive(&s_lock);
    VirtualFree(ptr, 0, MEM_RELEASE);
}

#endif
