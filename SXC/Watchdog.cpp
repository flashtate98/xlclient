// The hang watchdog (SXC/Watchdog.h).
#include "SXC/Watchdog.h"

#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "dbghelp.lib")

namespace {
    constexpr ULONGLONG kHang = 1000;      // ms without a frame: a hang
    constexpr ULONGLONG kAgain = 1000;     // ms between its stacks while it lasts
    constexpr int kMostStacks = 30;        // per hang (half a minute of them)
    constexpr int kDepth = 48;

    std::atomic<ULONGLONG> s_lastBeat{ 0 };
    std::atomic<bool> s_started{ false };
    HANDLE s_main = nullptr;
    HANDLE s_file = INVALID_HANDLE_VALUE;

    void Write(const char* text) {
        if (s_file == INVALID_HANDLE_VALUE) {
            char path[MAX_PATH];
            GetModuleFileNameA(nullptr, path, sizeof path);
            char* slash = std::strrchr(path, '\\');

            if (slash) {
                strcpy_s(slash + 1, sizeof path - (slash + 1 - path), "Logs\\hangs.log");
            }

            s_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }

        if (s_file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(s_file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
        }
    }

    // The main thread's return addresses, walked while it is suspended (nothing here allocates or locks:
    // the main thread may hold the heap's lock); named after it runs again.
    int Capture(DWORD64* out) {
        if (SuspendThread(s_main) == static_cast<DWORD>(-1)) {
            return 0;
        }

        CONTEXT context = {};
        context.ContextFlags = CONTEXT_FULL;
        int n = 0;

        if (GetThreadContext(s_main, &context)) {
            for (; n < kDepth && context.Rip; n++) {
                out[n] = context.Rip;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);

                if (!function) {
                    context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                    continue;
                }

                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
            }
        }

        ResumeThread(s_main);
        return n;
    }

    void Name(DWORD64 address, int depth) {
        HANDLE process = GetCurrentProcess();
        char where[MAX_PATH + 32] = "?";
        HMODULE module = nullptr;

        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) && module) {
            char path[MAX_PATH] = "?";
            GetModuleFileNameA(module, path, sizeof path);
            const char* name = std::strrchr(path, '\\');
            _snprintf_s(where, sizeof where, _TRUNCATE, "%s+0x%llX", name ? name + 1 : path, address - reinterpret_cast<DWORD64>(module));
        }

        char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 source = {};
        source.SizeOfStruct = sizeof source;
        DWORD lineDisplacement = 0;
        char line[1024];

        if (SymFromAddr(process, address, &displacement, symbol)) {
            if (SymGetLineFromAddr64(process, address, &lineDisplacement, &source)) {
                _snprintf_s(line, sizeof line, _TRUNCATE, "  #%02d %s %s+0x%llX (%s:%lu)\r\n", depth, where, symbol->Name, displacement, source.FileName, source.LineNumber);
            } else {
                _snprintf_s(line, sizeof line, _TRUNCATE, "  #%02d %s %s+0x%llX\r\n", depth, where, symbol->Name, displacement);
            }
        } else {
            _snprintf_s(line, sizeof line, _TRUNCATE, "  #%02d %s\r\n", depth, where);
        }

        Write(line);
    }

    void Stamp(char* out, size_t size) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        _snprintf_s(out, size, _TRUNCATE, "%02u/%02u %02u:%02u:%02u.%03u", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    }

    DWORD WINAPI Watch(void*) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        bool symbols = false;
        ULONGLONG hangFrom = 0, lastStack = 0;
        int stacks = 0;

        for (;;) {
            Sleep(200);
            const ULONGLONG now = GetTickCount64(), beat = s_lastBeat.load();

            if (IsDebuggerPresent()) {
                continue;
            }

            if (now - beat < kHang) {
                if (hangFrom) {
                    char text[160], when[32];
                    Stamp(when, sizeof when);
                    _snprintf_s(text, sizeof text, _TRUNCATE, "%s  the hang is over: no frame for %.1f s\r\n\r\n", when, (beat - hangFrom) / 1000.0);
                    Write(text);
                    hangFrom = 0;
                }

                continue;
            }

            if (!hangFrom) {
                hangFrom = beat;
                stacks = 0;
                lastStack = 0;
            }

            if (stacks >= kMostStacks || (lastStack && now - lastStack < kAgain)) {
                continue;
            }

            DWORD64 addresses[kDepth];
            const int n = Capture(addresses);
            lastStack = now;
            stacks++;

            if (!symbols) {
                SymInitialize(GetCurrentProcess(), nullptr, TRUE);
                symbols = true;
            }

            char text[160], when[32];
            Stamp(when, sizeof when);
            _snprintf_s(text, sizeof text, _TRUNCATE, "%s  no frame for %.1f s; the main thread is in:\r\n", when, (now - beat) / 1000.0);
            Write(text);

            for (int i = 0; i < n; i++) {
                Name(addresses[i], i);
            }
        }
    }
}

namespace SXC::Watchdog {
    void Beat() {
        s_lastBeat.store(GetTickCount64());

        if (s_started.exchange(true)) {
            return;
        }

        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_main, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);

        if (s_main) {
            if (HANDLE thread = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr)) {
                CloseHandle(thread);
            }
        }
    }
}
