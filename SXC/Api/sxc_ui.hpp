// sxc.ui for C++ mods: modules, panels and Quick entries in the overlay's shell, drawn with the full
// Dear ImGui API. Header-only; needs the SDK's wxl/sdk.hpp, the sxc.ui service, and the mod built
// with the client's Dear ImGui sources (SXC/ThirdParty/imgui, the same version and imconfig.h).
//
//     const int module = sxc::ui::AddModule("Immersive Azeroth", "...");
//     sxc::ui::AddPanel(module, "Overview", [](void*) { ImGui::Text("hello"); });
//
// Each draw call runs with the client's ImGui context and allocators set in the mod's copy of ImGui.
// A mod whose ImGui version differs from the client's gets no panels (Available() says so).

#pragma once

#include <cstring>
#include <memory>
#include <vector>
#include "SXC/Api/sxc_ui.h"
#include "imgui.h"
#include "wxl/sdk.hpp"

namespace sxc::ui {
    using DrawFn = void (*)(void* user);

    inline const sxc_ui_api* Api() {
        static const sxc_ui_api* s_api =
            wxl::Host() ? static_cast<const sxc_ui_api*>(wxl::Host()->get_service(SXC_UI_SERVICE, SXC_UI_SERVICE_VERSION)) : nullptr;
        return s_api;
    }

    /// The service is there and its Dear ImGui is the one this mod was built with.
    inline bool Available() {
        return Api() && std::strcmp(Api()->imgui_version(), IMGUI_VERSION) == 0;
    }

    namespace detail {
        struct Entry {
            DrawFn draw;
            void* user;
        };

        inline std::vector<std::unique_ptr<Entry>>& Entries() {
            static std::vector<std::unique_ptr<Entry>> s_entries;
            return s_entries;
        }

        // Points the mod's copy of ImGui at the client's context, then draws.
        inline void WXL_CALL Draw(void* user) {
            static bool s_allocators;

            if (!s_allocators) {
                s_allocators = true;
                sxc_imgui_alloc_fn alloc;
                sxc_imgui_free_fn free;
                void* data;
                Api()->imgui_allocators(&alloc, &free, &data);
                ImGui::SetAllocatorFunctions(alloc, free, data);
            }

            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(Api()->imgui_context()));
            const Entry* entry = static_cast<const Entry*>(user);
            entry->draw(entry->user);
        }

        inline void* Wrap(DrawFn draw, void* user) {
            Entries().emplace_back(new Entry{ draw, user });
            return Entries().back().get();
        }
    }

    /// A sidebar entry; -1 when the shell isn't available.
    inline int AddModule(const char* name, const char* description = nullptr) {
        return Available() ? Api()->add_module(name, description) : -1;
    }

    inline void AddPanel(int module, const char* title, DrawFn draw, void* user = nullptr) {
        if (Available() && module >= 0) Api()->add_panel(module, title, detail::Draw, detail::Wrap(draw, user));
    }

    inline void AddQuick(int module, const char* title, DrawFn draw, void* user = nullptr) {
        if (Available() && module >= 0) Api()->add_quick(module, title, detail::Draw, detail::Wrap(draw, user));
    }

    inline bool Visible() {
        return Api() && Api()->visible();
    }
}
