// SXC's frame profiler (SXC/Profiler/Profiler.h): the category stack, the CPU and GPU accounting, the
// second's averages, the panel and the sxc.profiler service.

#include "SXC/Profiler/Profiler.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include "SXC/Api/sxc_profiler.h"
#include "SXC/Settings/Settings.h"
#include "SXC/ThirdParty/imgui/imgui.h"

namespace {
    using namespace SXC::Profiler;

    // --- the categories ---------------------------------------------------------------------------------

    struct Info {
        char name[64];
        uint32_t flags;
    };

    Info s_info[kMaxCategories] = {
        { "other (main loop, unlisted)", 0 },
        { "game update (objects, Lua OnUpdate)", 0 },
        { "mods' update", 0 },
        { "world update (units, missiles)", 0 },
        { "interface (frames, text)", 0 },
        { "world: rest", 0 },
        { "world: culling, portals, sorting", 0 },
        { "models: animation", 0 },
        { "terrain", 0 },
        { "WMOs", 0 },
        { "sky", 0 },
        { "models: handed to the scene", kCpuOnly },
        { "models", 0 },
        { "grass", 0 },
        { "liquids", 0 },
        { "fog (ours)", 0 },
        { "weather", 0 },
        { "see-through, particles, spells", 0 },
        { "unit shadows (client)", 0 },
        { "full-screen glow (client)", 0 },
        { "world: scene objects", 0 },
        { "present, tools overlay", 0 },
        { "waiting: frame rate cap", kCpuOnly },
        { "lights (ours): scan", kCpuOnly },
        { "lights (ours): shadow assignment", kCpuOnly },
        { "lights (ours): shadow checks", kCpuOnly },
        { "lights (ours): shadow gathering", kCpuOnly },
        { "lights (ours): cells", kCpuOnly },
        { "covers (ours): frame data", kCpuOnly },
        { "grass (ours): chunks, runs", kCpuOnly },
        { "pre-pass: uploads", 0 },
        { "pre-pass: shadow maps (ours)", 0 },
    };
    int s_count = kBuiltinCount;

    bool CpuOnly(int c) {
        return (s_info[c].flags & kCpuOnly) != 0;
    }

    // --- settings ---------------------------------------------------------------------------------------

    SXC::Settings::Bool& EnabledSetting() {
        static SXC::Settings::Bool s_setting("Profiler", "On", false, "Frame profiler", "Measure every frame (the Client module's Profiler panel). A little overhead while on.");
        return s_setting;
    }

    SXC::Settings::Bool& GpuSetting() {
        static SXC::Settings::Bool s_setting("Profiler", "Gpu", true, "Profiler: GPU times", "GPU timestamps per category (Vulkan).");
        return s_setting;
    }

    // --- the stack and the frame --------------------------------------------------------------------------

    constexpr int kMaxDepth = 64;
    int s_stack[kMaxDepth];
    int s_depth;
    bool s_frameOn;         // this frame is measured (fixed at its start)
    DWORD s_mainThread;
    uint64_t s_offThread;   // scopes from other threads, ignored
    int64_t s_last;         // QPC at the last transition
    int64_t s_qpcFreq = 1;
    int64_t s_frameStart;

    const Gpu* s_gpu;
    bool s_gpuOn;           // the device is told to stamp
    int s_gpuCategory = -1; // what the device was last told

    struct FrameAccum {
        int64_t cpu[kMaxCategories];
        uint32_t draws[kMaxCategories];
        uint64_t tris[kMaxCategories];
    } s_frame;

    // About a second of frames, summed.
    struct Window {
        int64_t cpu[kMaxCategories];
        uint64_t draws[kMaxCategories];
        uint64_t tris[kMaxCategories];
        double gpu[kMaxCategories];
        uint32_t frames;
        uint32_t gpuFrames;   // device frames read back
        int64_t ticks;
        int64_t start;
    } s_window;

    // What the panel shows: per frame.
    struct Shown {
        double cpuMs[kMaxCategories];
        double gpuMs[kMaxCategories];
        double draws[kMaxCategories];
        double tris[kMaxCategories];
        double frameMs;
        double gpuTotal;
        uint32_t frames;
        uint32_t gpuFrames;
        int count;            // categories when it was made
        bool valid;
    } s_shown;

    bool s_hold;            // the panel keeps its numbers

    int64_t Now() {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    bool OnMainThread() {
        if (GetCurrentThreadId() == s_mainThread) {
            return true;
        }

        s_offThread++;
        return false;
    }

    int Current() {
        return s_depth > 0 ? s_stack[s_depth - 1] : kOther;
    }

    // The category GPU time goes to: the innermost one that isn't CPU only.
    int GpuCurrent() {
        for (int i = s_depth - 1; i >= 0; i--) {
            if (!CpuOnly(s_stack[i])) {
                return s_stack[i];
            }
        }

        return kOther;
    }

    void MarkGpu() {
        if (!s_gpuOn || !s_gpu) {
            return;
        }

        const int c = GpuCurrent();

        if (c != s_gpuCategory) {
            s_gpuCategory = c;
            s_gpu->mark(c);
        }
    }

    // The time since the last transition goes to the current category; then the GPU follows.
    void Transition() {
        const int64_t now = Now();
        s_frame.cpu[Current()] += now - s_last;
        s_last = now;
        MarkGpu();
    }

    void SetGpuOn(bool on) {
        if (on == s_gpuOn) {
            return;
        }

        s_gpuOn = on;
        s_gpuCategory = -1;

        if (s_gpu) {
            s_gpu->enable(on);
        }
    }

    void Publish(int64_t now) {
        Shown s = {};
        const Window& w = s_window;
        s.count = s_count;

        if (w.frames) {
            const double n = w.frames;
            const double toMs = 1000.0 / static_cast<double>(s_qpcFreq);

            for (int c = 0; c < s_count; c++) {
                s.cpuMs[c] = static_cast<double>(w.cpu[c]) * toMs / n;
                s.draws[c] = static_cast<double>(w.draws[c]) / n;
                s.tris[c] = static_cast<double>(w.tris[c]) / n;
                s.gpuMs[c] = w.gpuFrames ? w.gpu[c] / n : 0.0;
                s.gpuTotal += s.gpuMs[c];
            }

            s.frameMs = static_cast<double>(w.ticks) * toMs / n;
            s.frames = w.frames;
            s.gpuFrames = w.gpuFrames;
            s.valid = true;
        }

        if (!s_hold) {
            s_shown = s;
        }

        s_window = Window{};
        s_window.start = now;
    }

    // --- the report -------------------------------------------------------------------------------------

    // The rows in the panel's order: biggest first by CPU or GPU, whichever is larger.
    int Order(int* order, bool empty) {
        const Shown& s = s_shown;
        int n = 0;

        for (int c = 0; c < s.count; c++) {
            if (empty || s.cpuMs[c] >= 0.005 || s.gpuMs[c] >= 0.005 || s.draws[c] >= 0.5) {
                order[n++] = c;
            }
        }

        std::sort(order, order + n, [&](int a, int b) { return std::max(s.cpuMs[a], s.gpuMs[a]) > std::max(s.cpuMs[b], s.gpuMs[b]); });
        return n;
    }

    std::string Report() {
        const Shown& s = s_shown;
        std::string out;
        char line[256];
        std::snprintf(line, sizeof(line), "frame: %.2f ms CPU (%.0f fps), GPU busy %.2f ms%s\n", s.frameMs, s.frameMs > 0.0 ? 1000.0 / s.frameMs : 0.0,
                      s.gpuTotal, s.gpuFrames ? "" : " (no GPU times)");
        out += line;
        out += "category                                   CPU ms   GPU ms   draws   ktris\n";
        int order[kMaxCategories];
        const int n = Order(order, false);
        double draws = 0.0;
        double tris = 0.0;

        for (int k = 0; k < n; k++) {
            const int c = order[k];
            draws += s.draws[c];
            tris += s.tris[c];
            std::snprintf(line, sizeof(line), "%-40s %8.2f %8.2f %7.0f %7.1f\n", s_info[c].name, s.cpuMs[c], s.gpuMs[c], s.draws[c], s.tris[c] / 1000.0);
            out += line;
        }

        std::snprintf(line, sizeof(line), "%-40s %8.2f %8.2f %7.0f %7.1f\n", "total", s.frameMs, s.gpuTotal, draws, tris / 1000.0);
        out += line;
        return out;
    }

    void CopyToClipboard(const std::string& text) {
        if (!OpenClipboard(nullptr)) {
            return;
        }

        EmptyClipboard();
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);

        if (memory) {
            std::memcpy(GlobalLock(memory), text.c_str(), text.size() + 1);
            GlobalUnlock(memory);

            if (!SetClipboardData(CF_TEXT, memory)) {
                GlobalFree(memory);
            }
        }

        CloseClipboard();
    }

    // --- sxc.profiler -------------------------------------------------------------------------------------

    int32_t WXL_CALL ApiRegister(const char* name, uint32_t flags) {
        return Register(name, (flags & SXC_PROFILER_CPU_ONLY) ? kCpuOnly : 0u);
    }

    void WXL_CALL ApiEnter(int32_t category) {
        Enter(category);
    }

    void WXL_CALL ApiLeave(int32_t category) {
        Leave(category);
    }

    wxl_bool WXL_CALL ApiOn() {
        return s_frameOn ? WXL_TRUE : WXL_FALSE;
    }

    const sxc_profiler_api s_api = { sizeof(sxc_profiler_api), ApiRegister, ApiEnter, ApiLeave, ApiOn };
}

namespace SXC::Profiler {
    const sxc_profiler_api* Api() {
        return &s_api;
    }

    int Register(const char* name, uint32_t flags) {
        if (!name || !*name) {
            return -1;
        }

        for (int c = 0; c < s_count; c++) {
            if (std::strcmp(s_info[c].name, name) == 0) {
                return c;
            }
        }

        if (s_count >= kMaxCategories) {
            return -1;
        }

        Info& info = s_info[s_count];
        std::snprintf(info.name, sizeof(info.name), "%s", name);
        info.flags = flags;
        return s_count++;
    }

    void Enter(int category) {
        if (!s_frameOn || category < 0 || category >= s_count || !OnMainThread() || s_depth >= kMaxDepth) {
            return;
        }

        Transition();
        s_stack[s_depth++] = category;
        MarkGpu();
    }

    void Leave(int category) {
        if (!s_frameOn || !OnMainThread()) {
            return;
        }

        // The topmost matching entry: scopes that don't nest perfectly (an Enter / Leave pair around a
        // scope of another kind) still come out right.
        for (int i = s_depth - 1; i >= 0; i--) {
            if (s_stack[i] == category) {
                Transition();
                std::memmove(s_stack + i, s_stack + i + 1, sizeof(int) * (s_depth - i - 1));
                s_depth--;
                MarkGpu();
                return;
            }
        }
    }

    void CountDraw(uint32_t triangles) {
        if (!s_frameOn || GetCurrentThreadId() != s_mainThread) {
            return;
        }

        const int c = Current();
        s_frame.draws[c]++;
        s_frame.tris[c] += triangles;
    }

    bool On() {
        return s_frameOn;
    }

    void FrameEnd() {
        const int64_t now = Now();

        if (!s_mainThread) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            s_qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;
            s_mainThread = GetCurrentThreadId();
            s_window.start = now;
        }

        if (s_frameOn) {
            s_frame.cpu[Current()] += now - s_last;

            for (int c = 0; c < s_count; c++) {
                s_window.cpu[c] += s_frame.cpu[c];
                s_window.draws[c] += s_frame.draws[c];
                s_window.tris[c] += s_frame.tris[c];
            }

            s_window.ticks += now - s_frameStart;
            s_window.frames++;
        }

        std::memset(&s_frame, 0, sizeof(s_frame));

        // A scope left open (a Leave that never came) doesn't pile up.
        if (s_depth > kMaxDepth / 2) {
            s_depth = 0;
        }

        if (now - s_window.start >= s_qpcFreq) {
            Publish(now);
        }

        // The next frame.
        const bool on = EnabledSetting().Get();

        if (on != s_frameOn) {
            s_window = Window{};
            s_window.start = now;
            s_depth = 0;
        }

        s_frameOn = on;
        s_frameStart = now;
        s_last = now;
        SetGpuOn(on && GpuSetting().Get());
    }

    void SetGpu(const Gpu* gpu) {
        s_gpu = gpu;
        const bool on = s_gpuOn;
        s_gpuOn = false;
        s_gpuCategory = -1;

        if (gpu) {
            SetGpuOn(on);
        }
    }

    void GpuTimes(const double* ms, int count) {
        if (!s_frameOn || !ms) {
            return;
        }

        for (int c = 0; c < count && c < s_count; c++) {
            s_window.gpu[c] += ms[c];
        }

        s_window.gpuFrames++;
    }

    void DrawPanel(void* user) {
        bool on = EnabledSetting().Get();

        if (ImGui::Checkbox("Measure the frame", &on)) {
            EnabledSetting().Set(on);
        }

        ImGui::SetItemTooltip("A little overhead while on: a timer per scope, a GPU timestamp per change of category. [Profiler] On");
        ImGui::SameLine();
        bool gpu = GpuSetting().Get();

        if (ImGui::Checkbox("GPU times", &gpu)) {
            GpuSetting().Set(gpu);
        }

        ImGui::SetItemTooltip("Timestamps on the GPU per category (Vulkan only). [Profiler] Gpu");
        ImGui::SameLine();
        ImGui::Checkbox("Hold", &s_hold);
        ImGui::SetItemTooltip("Keep the numbers shown (measuring goes on).");
        ImGui::SameLine();
        static bool s_empty;
        ImGui::Checkbox("Empty rows", &s_empty);

        ImGui::TextDisabled("GPU: %s", s_gpu ? s_gpu->status() : "no timestamps on this renderer");

        if (s_gpu) {
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            ImGui::SetItemTooltip("With vsync on, the frame's GPU work can wait for its swapchain image, and the wait lands in the first rows of the frame. "
                                  "Turn vsync off to measure.");
        }

        if (!on) {
            ImGui::TextDisabled("(off)");
            return;
        }

        const Shown& s = s_shown;

        if (!s.valid) {
            ImGui::TextDisabled("(collecting...)");
            return;
        }

        ImGui::Text("%.2f ms a frame on the CPU (%.0f fps), the GPU busy %.2f ms", s.frameMs, s.frameMs > 0.0 ? 1000.0 / s.frameMs : 0.0, s.gpuTotal);
        ImGui::SameLine();

        if (ImGui::SmallButton("Copy")) {
            CopyToClipboard(Report());
        }

        ImGui::SetItemTooltip("The table as text, to paste somewhere.");

        // Which side holds the frame back: the GPU's work against the CPU's frame.
        if (s.gpuFrames && s.frameMs > 0.0) {
            ImGui::TextDisabled(s.gpuTotal > s.frameMs * 0.9 ? "The GPU is the limit: read the GPU column." : "The CPU is the limit: read the CPU column.");
        }

        if (s_offThread) {
            ImGui::TextDisabled("%llu scopes from other threads ignored", static_cast<unsigned long long>(s_offThread));
        }

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;

        if (!ImGui::BeginTable("##profile", 5, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
            return;
        }

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("category", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("CPU ms");
        ImGui::TableSetupColumn("GPU ms");
        ImGui::TableSetupColumn("draws");
        ImGui::TableSetupColumn("k tris");
        ImGui::TableHeadersRow();

        const double most = std::max(s.frameMs, s.gpuTotal);
        int order[kMaxCategories];
        const int n = Order(order, s_empty);

        for (int k = 0; k < n; k++) {
            const int c = order[k];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const float width = ImGui::GetContentRegionAvail().x;
            ImGui::TextUnformatted(s_info[c].name);

            // The bigger of the two times, as a share of the frame, under the name.
            if (most > 0.0) {
                const float share = static_cast<float>(std::min(std::max(s.cpuMs[c], s.gpuMs[c]) / most, 1.0));
                const ImVec2 min = ImGui::GetItemRectMin();
                const float bottom = ImGui::GetItemRectMax().y;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x, bottom - 2.0f), ImVec2(min.x + width * share, bottom), ImGui::GetColorU32(ImGuiCol_PlotHistogram));
            }

            ImGui::TableNextColumn();
            ImGui::Text("%6.2f", s.cpuMs[c]);
            ImGui::TableNextColumn();

            if (CpuOnly(c)) {
                ImGui::TextDisabled("     -");
            } else {
                ImGui::Text("%6.2f", s.gpuMs[c]);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%5.0f", s.draws[c]);
            ImGui::TableNextColumn();
            ImGui::Text("%6.1f", s.tris[c] / 1000.0);
        }

        ImGui::EndTable();
    }
}
