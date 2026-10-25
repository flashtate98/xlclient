#include "SXC/Ui/Ui.h"
#include "SXC/Ui/UiVulkan.h"

#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>
#include "SXC/Data/Data.h"
#include "SXC/Grass/Grass.h"
#include "SXC/Log.h"
#include "SXC/Profiler/Profiler.h"
#include "SXC/Settings/Settings.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "SXC/ThirdParty/imgui/imgui_internal.h"
#include "SXC/ThirdParty/imgui/backends/imgui_impl_vulkan.h"
#include "SXC/ThirdParty/imgui/backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace {
    constexpr const char* kTitle = "Seyri's tools";
    constexpr float kSidebarWidth = 220.0f;

    struct Entry {
        std::string title;
        SXC::Ui::DrawFn draw;
        void* user;
    };

    struct Module {
        std::string name;
        std::string description;
        std::vector<Entry> panels;
        std::vector<Entry> quick;
        bool layoutBuilt = false;  // the default dock layout was applied once this session
    };

    struct QuickPick {
        int module;
        size_t index;
        bool shown;
    };

    // The fixed sidebar entries after the modules.
    enum View { kViewModule = 0, kViewData = 1, kViewSettings = 2 };

    std::vector<Module> s_modules;
    std::vector<QuickPick> s_quickPicks;
    int s_generalModule = -1;
    int s_view = kViewModule;
    int s_selected = -1;

    // Overlay state.
    HWND s_hwnd;
    bool s_visible;
    bool s_win32;
    bool s_showDemo;

    // The shell's options, in WTF\SXC.ini. Built on first use, not at static initialisation.
    struct Options {
        SXC::Settings::Float textScale{ "Shell", "TextSize", 1.0f, 0.75f, 2.0f, "Text size", "Scale of all overlay text." };
        SXC::Settings::Bool sidebarRight{ "Shell", "SidebarRight", false, "Sidebar on the right", "Anchor the sidebar to the right edge instead of the left." };
        SXC::Settings::Bool showQuick{ "Shell", "QuickStrip", false, "Quick strip", "Show the Quick strip." };
        // "Module/Title" of each Quick entry the strip shows, separated by '|'.
        SXC::Settings::String quickPicks{ "Shell", "QuickPicks", "" };
    };

    Options& GetOptions() {
        static Options s_options;
        return s_options;
    }

    // Checkbox bound to a setting.
    bool SettingCheckbox(const char* label, SXC::Settings::Bool& setting) {
        bool value = setting.Get();

        if (!ImGui::Checkbox(label, &value)) {
            return false;
        }

        setting.Set(value);
        return true;
    }

    std::string QuickKey(const QuickPick& pick) {
        const Module& module = s_modules[pick.module];
        return module.name + "/" + module.quick[pick.index].title;
    }

    // The saved picks, including those of modules not loaded this session (kept as they are).
    std::vector<std::string> SavedQuickPicks() {
        std::vector<std::string> picks;
        const std::string& text = GetOptions().quickPicks.Get();
        size_t start = 0;

        while (start <= text.size()) {
            const size_t end = std::min(text.find('|', start), text.size());

            if (end > start) {
                picks.push_back(text.substr(start, end - start));
            }

            start = end + 1;
        }

        return picks;
    }

    void SaveQuickPick(const QuickPick& pick) {
        const std::string key = QuickKey(pick);
        std::string text;

        for (const std::string& saved : SavedQuickPicks()) {
            if (saved != key) {
                text += (text.empty() ? "" : "|") + saved;
            }
        }

        if (pick.shown) {
            text += (text.empty() ? "" : "|") + key;
        }

        GetOptions().quickPicks.Set(text.c_str());
    }

    // Vulkan backend state.
    bool s_vulkan;
    VkInstance s_instance;
    VkDevice s_device;
    VkRenderPass s_pass;
    PFN_vkGetInstanceProcAddr s_getInstanceProcAddr;
    PFN_vkDeviceWaitIdle s_deviceWaitIdle;

    // --- the built-in "Client" module ----------------------------------------------------------------

    float s_frameTimes[240];
    int s_frameTimeAt;

    void RecordFrameTime() {
        s_frameTimes[s_frameTimeAt] = ImGui::GetIO().DeltaTime * 1000.0f;
        s_frameTimeAt = (s_frameTimeAt + 1) % IM_ARRAYSIZE(s_frameTimes);
    }

    void DrawClientInfo(void* user) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("%.1f fps, %.2f ms", io.Framerate, io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
        ImGui::PlotLines("##frametimes", s_frameTimes, IM_ARRAYSIZE(s_frameTimes), s_frameTimeAt, "frame time (ms)", 0.0f, 50.0f, ImVec2(-FLT_MIN, 80.0f));

        // SXC: the instanced grass, switchable here to compare.
        bool grass = SXC::Grass::Enabled();

        if (ImGui::Checkbox("Instanced grass", &grass)) {
            SXC::Grass::SetEnabled(grass);
        }

        ImGui::SetItemTooltip("The ground-effect plants drawn instanced on the GPU (Vulkan) instead of re-baked on the CPU every frame. [Grass] Instanced");
        ImGui::TextWrapped("Grass: %s", SXC::Grass::Status());
        ImGui::TextDisabled("Overlay: Dear ImGui %s (docking)", ImGui::GetVersion());
    }

    void DrawClientQuick(void* user) {
        ImGui::Text("%.1f fps", ImGui::GetIO().Framerate);
    }

    void RegisterBuiltins() {
        static bool s_done;

        if (s_done) {
            return;
        }

        s_done = true;
        const int client = SXC::Ui::AddModule("Client", "The client itself.");
        SXC::Ui::AddPanel(client, "Info", DrawClientInfo);
        SXC::Ui::AddPanel(client, "Profiler", SXC::Profiler::DrawPanel);
        SXC::Ui::AddQuick(client, "Frame rate", DrawClientQuick);
    }

    PFN_vkVoidFunction LoadVulkan(const char* name, void* user) {
        return s_getInstanceProcAddr(s_instance, name);
    }

    void EnsureContext() {
        if (ImGui::GetCurrentContext()) {
            return;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = "WTF\\SXC_imgui.ini";
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // The game owns the cursor; over an overlay window ImGui draws its own (MouseDrawCursor below).
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ImGui::StyleColorsDark();
    }

    bool HasEntry(const std::vector<Entry>& entries, const char* title) {
        for (const Entry& entry : entries) {
            if (entry.title == title) {
                return true;
            }
        }

        return false;
    }

    // A window name unique per module, so two modules can both have a "Settings" panel.
    std::string PanelWindowName(const Module& module, const Entry& panel) {
        return panel.title + "##" + module.name;
    }

    // --- the sidebar ---------------------------------------------------------------------------------

    bool SidebarButton(const char* label, bool selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(selected ? ImGuiCol_ButtonActive : ImGuiCol_Button));
        const bool clicked = ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f));
        ImGui::PopStyleColor();
        return clicked;
    }

    void DrawSidebar(const ImGuiViewport* viewport) {
        const float x = GetOptions().sidebarRight.Get() ? viewport->WorkPos.x + viewport->WorkSize.x - kSidebarWidth : viewport->WorkPos.x;
        ImGui::SetNextWindowPos(ImVec2(x, viewport->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(kSidebarWidth, viewport->WorkSize.y));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                                     | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;

        if (ImGui::Begin("##SxcSidebar", nullptr, flags)) {
            ImGui::TextUnformatted(kTitle);
            ImGui::TextDisabled("%.0f fps", ImGui::GetIO().Framerate);
            ImGui::Separator();

            if (s_modules.empty()) {
                ImGui::TextDisabled("no modules registered");
            }

            for (int i = 0; i < static_cast<int>(s_modules.size()); i++) {
                // "General" only when something landed there.
                if (i == s_generalModule && s_modules[i].panels.empty() && s_modules[i].quick.empty()) {
                    continue;
                }

                ImGui::PushID(i);

                if (SidebarButton(s_modules[i].name.c_str(), s_view == kViewModule && s_selected == i)) {
                    s_view = kViewModule;
                    s_selected = s_selected == i ? -1 : i;
                }

                ImGui::PopID();
            }

            ImGui::Separator();

            if (SidebarButton("Data", s_view == kViewData)) {
                s_view = s_view == kViewData ? kViewModule : kViewData;
            }

            if (SidebarButton("Settings", s_view == kViewSettings)) {
                s_view = s_view == kViewSettings ? kViewModule : kViewSettings;
            }

            ImGui::Separator();
            SettingCheckbox("Quick strip", GetOptions().showQuick);

            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetTextLineHeightWithSpacing() - ImGui::GetStyle().WindowPadding.y);
            ImGui::TextDisabled("Ctrl+F9 hides this");
        }

        ImGui::End();
    }

    // --- views ---------------------------------------------------------------------------------------

    // The dock space beside the sidebar. Its empty middle passes input through to the game.
    ImGuiID DrawDockSpace(const ImGuiViewport* viewport) {
        const float left = GetOptions().sidebarRight.Get() ? viewport->WorkPos.x : viewport->WorkPos.x + kSidebarWidth;
        ImGui::SetNextWindowPos(ImVec2(left, viewport->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - kSidebarWidth, viewport->WorkSize.y));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
                                     | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
                                     | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
        ImGui::Begin("##SxcDockHost", nullptr, flags);
        ImGui::PopStyleVar();
        const ImGuiID id = ImGui::GetID("SxcDockSpace");
        ImGui::DockSpace(id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();
        return id;
    }

    // The first time a module's view opens in a session, and its panels have no saved place yet: they
    // go as tabs into a node on the side away from the sidebar, so the game stays visible.
    void BuildDefaultLayout(ImGuiID dockSpace, Module& module) {
        module.layoutBuilt = true;
        bool anyUnplaced = false;

        for (const Entry& panel : module.panels) {
            ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(PanelWindowName(module, panel).c_str()));

            if (!settings || !settings->DockId) {
                anyUnplaced = true;
            }
        }

        if (!anyUnplaced) {
            return;
        }

        if (!ImGui::DockBuilderGetNode(dockSpace)) {
            return;
        }

        // One side node for every module's default layout, split off the dock space once.
        static ImGuiID s_sideNode;

        if (!s_sideNode || !ImGui::DockBuilderGetNode(s_sideNode)) {
            ImGuiID rest = 0;
            ImGui::DockBuilderSplitNode(dockSpace, GetOptions().sidebarRight.Get() ? ImGuiDir_Left : ImGuiDir_Right, 0.30f, &s_sideNode, &rest);
        }

        for (const Entry& panel : module.panels) {
            ImGui::DockBuilderDockWindow(PanelWindowName(module, panel).c_str(), s_sideNode);
        }

        ImGui::DockBuilderFinish(dockSpace);
    }

    void DrawModuleView(ImGuiID dockSpace) {
        if (s_selected < 0 || s_selected >= static_cast<int>(s_modules.size())) {
            return;
        }

        Module& module = s_modules[s_selected];

        if (!module.layoutBuilt) {
            BuildDefaultLayout(dockSpace, module);
        }

        if (module.panels.empty()) {
            ImGui::SetNextWindowDockID(dockSpace, ImGuiCond_FirstUseEver);

            if (ImGui::Begin((module.name + "##SxcModuleEmpty").c_str())) {
                if (!module.description.empty()) {
                    ImGui::TextWrapped("%s", module.description.c_str());
                }

                ImGui::TextDisabled("this module has no panels");
            }

            ImGui::End();
            return;
        }

        for (const Entry& panel : module.panels) {
            if (ImGui::Begin(PanelWindowName(module, panel).c_str())) {
                if (&panel == &module.panels.front() && !module.description.empty()) {
                    ImGui::TextWrapped("%s", module.description.c_str());
                    ImGui::Separator();
                }

                panel.draw(panel.user);
            }

            ImGui::End();
        }
    }

    void DrawDataView() {
        ImGui::SetNextWindowSize(ImVec2(1200.0f, 650.0f), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Data##SxcData")) {
            SXC::Data::DrawBrowser();
        }

        ImGui::End();
    }

    void DrawSettingsView() {
        if (ImGui::Begin("Settings##SxcSettings")) {
            SXC::Settings::DrawEditor();
            ImGui::Separator();
            ImGui::Checkbox("ImGui demo window", &s_showDemo);
            ImGui::TextDisabled("Dear ImGui %s (docking)", ImGui::GetVersion());
            ImGui::TextDisabled("Reports go to Logs\\sxc.log");
        }

        ImGui::End();
    }

    void DrawQuickStrip() {
        if (!GetOptions().showQuick.Get()) {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);
        bool open = true;

        if (ImGui::Begin("Quick##SxcQuick", &open)) {
            if (s_quickPicks.empty()) {
                ImGui::TextDisabled("no module offers quick controls yet");
            }

            if (ImGui::BeginPopupContextWindow("SxcQuickPick")) {
                for (QuickPick& pick : s_quickPicks) {
                    const Module& module = s_modules[pick.module];
                    const std::string label = module.name + ": " + module.quick[pick.index].title;
                    if (ImGui::MenuItem(label.c_str(), nullptr, &pick.shown)) {
                        SaveQuickPick(pick);
                    }
                }

                ImGui::EndPopup();
            }

            bool any = false;

            for (const QuickPick& pick : s_quickPicks) {
                if (!pick.shown) {
                    continue;
                }

                const Module& module = s_modules[pick.module];
                const Entry& entry = module.quick[pick.index];
                any = true;

                ImGui::PushID(&entry);
                ImGui::SeparatorText((module.name + ": " + entry.title).c_str());
                entry.draw(entry.user);
                ImGui::PopID();
            }

            if (!s_quickPicks.empty() && !any) {
                ImGui::TextDisabled("right-click to choose what this shows");
            }
        }

        ImGui::End();

        if (!open) {
            GetOptions().showQuick.Set(false);
        }
    }

    void DrawOverlay() {
        RegisterBuiltins();
        RecordFrameTime();
        ImGui::GetStyle().FontScaleMain = GetOptions().textScale.Get();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImGuiID dockSpace = DrawDockSpace(viewport);
        DrawSidebar(viewport);

        switch (s_view) {
            case kViewData:
                DrawDataView();
                break;

            case kViewSettings:
                DrawSettingsView();
                break;

            default:
                DrawModuleView(dockSpace);
                break;
        }

        DrawQuickStrip();
        SXC::Data::DrawWindows();

        if (s_showDemo) {
            ImGui::ShowDemoWindow(&s_showDemo);
        }
    }

    bool InitVulkan(const SXC::Ui::VulkanTarget& target) {
        s_getInstanceProcAddr = target.getInstanceProcAddr;
        s_instance = target.instance;

        if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, LoadVulkan, nullptr)) {
            SXC::LogOnce("ui|vk-load", "ui: ImGui could not load its Vulkan functions; no overlay");
            return false;
        }

        ImGui_ImplVulkan_InitInfo info = {};
        info.ApiVersion = VK_API_VERSION_1_0;
        info.Instance = target.instance;
        info.PhysicalDevice = target.physical;
        info.Device = target.device;
        info.QueueFamily = target.queueFamily;
        info.Queue = target.queue;
        info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
        info.MinImageCount = 2;
        info.ImageCount = target.imageCount < 2 ? 2 : target.imageCount;
        info.PipelineInfoMain.RenderPass = target.renderPass;
        info.PipelineInfoMain.Subpass = 0;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

        if (!ImGui_ImplVulkan_Init(&info)) {
            SXC::LogOnce("ui|vk-init", "ui: ImGui's Vulkan backend failed to initialise; no overlay");
            return false;
        }

        s_device = target.device;
        s_pass = target.renderPass;
        s_deviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(s_getInstanceProcAddr(s_instance, "vkDeviceWaitIdle"));
        s_vulkan = true;
        return true;
    }
}

namespace SXC::Ui {
    int AddModule(const char* name, const char* description) {
        if (!name || !*name) {
            return -1;
        }

        for (int i = 0; i < static_cast<int>(s_modules.size()); i++) {
            if (s_modules[i].name == name) {
                if (description && s_modules[i].description.empty()) {
                    s_modules[i].description = description;
                }

                return i;
            }
        }

        Module module;
        module.name = name;
        module.description = description ? description : "";
        s_modules.push_back(module);
        return static_cast<int>(s_modules.size()) - 1;
    }

    void AddPanel(int module, const char* title, DrawFn draw, void* user) {
        if (module < 0 || module >= static_cast<int>(s_modules.size()) || !title || !draw || HasEntry(s_modules[module].panels, title)) {
            return;
        }

        s_modules[module].panels.push_back(Entry{ title, draw, user });
        s_modules[module].layoutBuilt = false;
    }

    void AddQuick(int module, const char* title, DrawFn draw, void* user) {
        if (module < 0 || module >= static_cast<int>(s_modules.size()) || !title || !draw || HasEntry(s_modules[module].quick, title)) {
            return;
        }

        s_modules[module].quick.push_back(Entry{ title, draw, user });
        QuickPick pick{ module, s_modules[module].quick.size() - 1, false };
        const std::vector<std::string> saved = SavedQuickPicks();
        pick.shown = std::find(saved.begin(), saved.end(), QuickKey(pick)) != saved.end();
        s_quickPicks.push_back(pick);
    }

    void AddPanel(const char* title, DrawFn draw, void* user) {
        if (s_generalModule < 0) {
            s_generalModule = AddModule("General", "Panels without a module of their own.");
        }

        AddPanel(s_generalModule, title, draw, user);
    }

    bool Visible() {
        return s_visible;
    }

    bool WndProc(void* hwnd, uint32_t message, uintptr_t wparam, intptr_t lparam) {
        if (!s_hwnd) {
            s_hwnd = static_cast<HWND>(hwnd);
        }

        // Ctrl+F9: show / hide. The first press only (bit 30 = the key was already down). Plain F9 stays
        // the game's (a bag by default).
        if (message == WM_KEYDOWN && wparam == VK_F9 && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (!(lparam & (1 << 30))) {
                s_visible = !s_visible;
            }

            return true;
        }

        if (!s_visible || !s_win32) {
            return false;
        }

        ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), message, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
        const ImGuiIO& io = ImGui::GetIO();

        if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
            return io.WantCaptureMouse;
        }

        if ((message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_CHAR) {
            return io.WantCaptureKeyboard;
        }

        return false;
    }

    void RenderVulkan(VkCommandBuffer cmd, const VulkanTarget& target) {
        if (!s_visible || !s_hwnd || !cmd) {
            return;
        }

        EnsureContext();

        if (!s_win32) {
            ImGui_ImplWin32_Init(s_hwnd);
            s_win32 = true;
        }

        // A new present pass (the swapchain format changed): rebuild ImGui's pipeline for it.
        if (s_vulkan && s_pass != target.renderPass) {
            ShutdownVulkan();
        }

        if (!s_vulkan && !InitVulkan(target)) {
            return;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = io.WantCaptureMouse;

        DrawOverlay();

        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }

    void ShutdownVulkan() {
        if (!s_vulkan) {
            return;
        }

        if (s_deviceWaitIdle) {
            s_deviceWaitIdle(s_device);
        }

        ImGui_ImplVulkan_Shutdown();
        s_vulkan = false;
        s_pass = VK_NULL_HANDLE;
    }
}
