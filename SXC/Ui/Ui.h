#pragma once

#include <cstdint>

// "Seyri's tools": SXC's in-client tools overlay, Dear ImGui (docking branch) over the game, toggled
// with Ctrl+F9 (plain F9 is a game binding, a bag by default; WXL's panel used F9).
//
// A sidebar anchored to a screen edge lists the modules, plus "Data" and "Settings". Selecting a module
// shows its view: its panels, docked into the free area beside the sidebar (as tabs at first; the user
// can rearrange them, ImGui remembers the layout in WTF\SXC_imgui.ini). Where nothing is docked, the
// game stays clickable. Quick entries are small controls a module offers for the "Quick" strip.
//
// Draw functions run inside an ImGui frame and may use the whole ImGui API (SXC/ThirdParty/imgui/imgui.h).
// Strings passed in are copied.
//
// Renderers: Vulkan (SXC/Ui/UiVulkan.h, called from the backend's present pass). D3D9 to follow.
namespace SXC::Ui {
    using DrawFn = void (*)(void* user);
    using PanelFn = DrawFn;

    // A module's entry in the sidebar. The same name returns the same id. `description` is shown at the
    // top of its view (may be null).
    int AddModule(const char* name, const char* description = nullptr);

    // A panel in a module's view. The same (module, title) again does nothing.
    void AddPanel(int module, const char* title, DrawFn draw, void* user = nullptr);

    // A small control set for the Quick strip (the user picks which ones it shows).
    void AddQuick(int module, const char* title, DrawFn draw, void* user = nullptr);

    // A panel without a module of its own: listed under "General".
    void AddPanel(const char* title, DrawFn draw, void* user = nullptr);

    // Whether the overlay is showing.
    bool Visible();

    // The game window's message procedure hands every message here first. True when the overlay used
    // it and the game must not see it (Ctrl+F9; mouse and keyboard input while ImGui wants it).
    bool WndProc(void* hwnd, uint32_t message, uintptr_t wparam, intptr_t lparam);
}
