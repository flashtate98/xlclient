// sxc.bridge: a mod that publishes the client's sxc.* services (SXC/Api) through the WarcraftXL
// host, so other mods resolve them like any SDK service. The executable can't publish them itself:
// the host hands publish_service to mods only.
//
// On a client that isn't ours (no sxc_get_service export) it declines to load, and the mods that
// require sxc.* are refused with that reason.

#define WXL_BUILDING_MOD

#include <windows.h>
#include <string>
#include "SXC/Api/sxc_data.h"
#include "SXC/Api/sxc_settings.h"
#include "SXC/Api/sxc_ui.h"
#include "SXC/Api/sxc_sky.h"
#include "SXC/Api/sxc_world.h"
#include "SXC/Api/sxc_fog.h"
#include "SXC/Api/sxc_cover.h"
#include "SXC/Api/sxc_lights.h"
#include "SXC/Api/sxc_grass.h"
#include "SXC/Api/sxc_profiler.h"
#include "wxl/mod.h"

namespace {
    constexpr const char* kTag = "sxc.bridge";

    struct Service {
        const char* name;
        uint32_t version;
    };

    const Service kServices[] = {
        { SXC_DATA_SERVICE, SXC_DATA_SERVICE_VERSION },
        { SXC_SETTINGS_SERVICE, SXC_SETTINGS_SERVICE_VERSION },
        { SXC_UI_SERVICE, SXC_UI_SERVICE_VERSION },
        { SXC_WORLD_SERVICE, SXC_WORLD_SERVICE_VERSION },
        { SXC_SKY_SERVICE, SXC_SKY_SERVICE_VERSION },
        { SXC_FOG_SERVICE, SXC_FOG_SERVICE_VERSION },
        { SXC_COVER_SERVICE, SXC_COVER_SERVICE_VERSION },
        { SXC_LIGHTS_SERVICE, SXC_LIGHTS_SERVICE_VERSION },
        { SXC_GRASS_SERVICE, SXC_GRASS_SERVICE_VERSION },
        { SXC_PROFILER_SERVICE, SXC_PROFILER_SERVICE_VERSION },
    };

    void Log(const wxl_host* host, wxl_log_level level, const std::string& line) {
        host->log(level, kTag, wxl_str{ line.data(), static_cast<uint32_t>(line.size()) });
    }
}

extern "C" WXL_EXPORT const wxl_mod_info* WXL_CALL wxl_mod_query(void) {
    static const wxl_mod_info info = { sizeof(wxl_mod_info), WXL_ABI_VERSION_MAJOR, WXL_ABI_VERSION_MINOR, "sxc.bridge", "SXC bridge", 0u, WXL_ABI_VERSION_FEATURE };
    return &info;
}

extern "C" WXL_EXPORT wxl_result WXL_CALL wxl_mod_load(const wxl_host* host) {
    if (!host) {
        return WXL_E_INVALID;
    }

    const auto get = reinterpret_cast<sxc_get_service_fn>(GetProcAddress(GetModuleHandleW(nullptr), SXC_GET_SERVICE_EXPORT));

    if (!get) {
        Log(host, WXL_LOG_WARN, "this client has no sxc services (not an SXC build); not loading");
        return WXL_E_UNSUPPORTED;
    }

    for (const Service& service : kServices) {
        const void* table = get(service.name, service.version);

        if (!table) {
            Log(host, WXL_LOG_WARN, std::string("the client does not offer ") + service.name + " version " + std::to_string(service.version));
            continue;
        }

        const wxl_result result = host->publish_service(service.name, service.version, table);
        Log(host, result == WXL_OK ? WXL_LOG_INFO : WXL_LOG_WARN,
            std::string(result == WXL_OK ? "published " : "could not publish ") + service.name + " " + std::to_string(service.version));
    }

    return WXL_OK;
}
