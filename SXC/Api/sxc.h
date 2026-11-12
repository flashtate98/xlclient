/*
 * SXC's services for mods: what our client offers beyond the WarcraftXL SDK.
 *
 * The client exports sxc_get_service from its executable. The sxc.bridge mod (SXC/Bridge) loads
 * first and publishes every service below through the SDK host, so a mod resolves them like any
 * other service: wxl_host::get_service("sxc.data", 1), and lists them under "requires" in its
 * mod.json with "loadAfter": ["sxc.bridge"].
 *
 * The SDK's boundary rules hold (wxl/abi.h): plain C, UTF-8, the allocating side frees, structs
 * grow by appending and bumping struct_size.
 *
 * C++ mods use the header-only wrappers next to these (sxc_data.hpp, sxc_settings.hpp, sxc_ui.hpp).
 */

#ifndef SXC_API_H
#define SXC_API_H

#include "wxl/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The executable's export: the service table for (name, version), or NULL.
typedef const void*(WXL_CALL* sxc_get_service_fn)(const char* name, uint32_t version);

#define SXC_GET_SERVICE_EXPORT "sxc_get_service"

#ifdef __cplusplus
}
#endif

#endif
