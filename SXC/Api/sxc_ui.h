/*
 * sxc.ui: the overlay's shell ("Seyri's tools"): modules in the sidebar, their panels, Quick
 * strip entries. The C side of SXC/Ui/Ui.h.
 *
 * Panels draw with Dear ImGui. Unlike the SDK's wxl.ui, this service lends out its ImGui context:
 * a mod built against the same Dear ImGui (SXC/ThirdParty/imgui, the version imgui_version answers)
 * uses the whole ImGui API. sxc_ui.hpp sets the context and allocators up for C++ mods and refuses
 * a mismatched version.
 */

#ifndef SXC_API_UI_H
#define SXC_API_UI_H

#include "SXC/Api/sxc.h"

#define SXC_UI_SERVICE         "sxc.ui"
#define SXC_UI_SERVICE_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

/// Draws a panel or Quick entry inside an ImGui frame.
typedef void(WXL_CALL* sxc_draw_fn)(void* user);

/// Dear ImGui's allocator pair (ImGuiMemAllocFunc / ImGuiMemFreeFunc).
typedef void*(WXL_CALL* sxc_imgui_alloc_fn)(size_t size, void* user);
typedef void(WXL_CALL* sxc_imgui_free_fn)(void* ptr, void* user);

typedef struct sxc_ui_api
{
    uint32_t struct_size;

    /// A module's sidebar entry; the same name returns the same id. -1 on a bad name.
    int32_t(WXL_CALL* add_module)(const char* name, const char* description);
    /// A panel in a module's view, docked beside the sidebar. The same (module, title) again does nothing.
    wxl_result(WXL_CALL* add_panel)(int32_t module, const char* title, sxc_draw_fn draw, void* user);
    /// A small control set the user can pick for the Quick strip.
    wxl_result(WXL_CALL* add_quick)(int32_t module, const char* title, sxc_draw_fn draw, void* user);
    /// Whether the overlay is showing.
    wxl_bool(WXL_CALL* visible)(void);

    /// IMGUI_VERSION of the client's Dear ImGui, e.g. "1.92.9b WIP".
    const char*(WXL_CALL* imgui_version)(void);
    /// The client's ImGuiContext*, NULL before the overlay was first shown.
    void*(WXL_CALL* imgui_context)(void);
    /// The allocators the client's Dear ImGui uses; a mod's copy must use the same.
    void(WXL_CALL* imgui_allocators)(sxc_imgui_alloc_fn* alloc, sxc_imgui_free_fn* free, void** user);
} sxc_ui_api;

#ifdef __cplusplus
}
#endif

#endif
