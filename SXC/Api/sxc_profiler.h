/*
 * sxc.profiler: a mod's own rows in the client's frame profiler (the Client module's "Profiler" panel).
 * A mod registers a category per step of its frame and brackets the step with enter / leave; the panel
 * shows each one's CPU time (exclusive: a nested category isn't counted again in the one around it) and,
 * unless registered CPU only, the GPU time of what it drew.
 *
 * Main thread only; enter / leave cost next to nothing while the profiler is off.
 */

#ifndef SXC_API_PROFILER_H
#define SXC_API_PROFILER_H

#include "SXC/Api/sxc.h"

#define SXC_PROFILER_SERVICE         "sxc.profiler"
#define SXC_PROFILER_SERVICE_VERSION 1

/// A category that draws nothing, or is entered many times a frame: no GPU timestamps for it.
#define SXC_PROFILER_CPU_ONLY 0x1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sxc_profiler_api
{
    uint32_t struct_size;

    /// A category ("immersive-azeroth: fog field"); the same name returns the same id. -1 when the
    /// profiler has no room left.
    int32_t(WXL_CALL* register_category)(const char* name, uint32_t flags);

    void(WXL_CALL* enter)(int32_t category);
    void(WXL_CALL* leave)(int32_t category);

    /// Whether this frame is being measured.
    wxl_bool(WXL_CALL* on)(void);
} sxc_profiler_api;

#ifdef __cplusplus
}
#endif

#endif
