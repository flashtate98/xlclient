#if defined(WOWDECOMP_ARCH_64)

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

    struct Arena {
        uintptr_t base = 0;
        uintptr_t committed = 0;               // bytes committed from base
        std::map<uintptr_t, uintptr_t> free;   // offset -> size, neighbours merged
        std::map<uintptr_t, uintptr_t> used;   // offset -> size
    };

    std::vector<Arena*> s_arenas;
    SRWLOCK s_lock = SRWLOCK_INIT;

    // The address space walked from the anchor for `bytes` (granularity-rounded) of free space within
    // the near range, reserved (and committed when `commit`). The original OsAllocNear's search.
    uintptr_t FindNear(uintptr_t center, uintptr_t bytes, bool commit) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        uintptr_t granularity = info.dwAllocationGranularity;
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

        MEMORY_BASIC_INFORMATION region;
        uintptr_t address = center;

        while (address < maximum && VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) {
            uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
            uintptr_t end = base + region.RegionSize;

            if (region.State == MEM_FREE) {
                uintptr_t candidate = ((base > address ? base : address) + granularity - 1) & ~(granularity - 1);

                if (candidate + bytes <= end && candidate + bytes <= maximum) {
                    if (void* ptr = VirtualAlloc(reinterpret_cast<void*>(candidate), bytes, type, protect)) {
                        return reinterpret_cast<uintptr_t>(ptr);
                    }
                }
            }

            address = end;
        }

        address = center;

        while (address > minimum && VirtualQuery(reinterpret_cast<void*>(address - 1), &region, sizeof(region))) {
            uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
            uintptr_t end = base + region.RegionSize;

            if (end > address) {
                end = address;
            }

            if (region.State == MEM_FREE && end - base >= bytes) {
                uintptr_t candidate = (end - bytes) & ~(granularity - 1);

                if (candidate >= base && candidate >= minimum) {
                    if (void* ptr = VirtualAlloc(reinterpret_cast<void*>(candidate), bytes, type, protect)) {
                        return reinterpret_cast<uintptr_t>(ptr);
                    }
                }
            }

            address = base;
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
        if (Near(center, a->base, kArenaSize)) {
            if (uintptr_t p = Take(*a, bytes)) {
                ReleaseSRWLockExclusive(&s_lock);
                return reinterpret_cast<void*>(p);
            }
        }
    }

    uintptr_t p = 0;

    if (uintptr_t base = FindNear(center, kArenaSize, false)) {
        Arena* a = new Arena();
        a->base = base;
        a->free.emplace(0, kArenaSize);
        s_arenas.push_back(a);
        p = Take(*a, bytes);
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
        if (p >= a->base && p < a->base + kArenaSize) {
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
