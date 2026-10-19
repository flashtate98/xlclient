#include "SXC/Log.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <unordered_set>
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {
    HSLOG s_log;
    bool s_logOpened;
    std::unordered_set<std::string> s_once;

    void Write(const char* format, va_list args) {
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), format, args);

        if (!s_logOpened) {
            s_logOpened = true;

            if (!SLogCreate("Logs\\sxc.log", SLOG_FLAG_OPENFILE, &s_log)) {
                s_log = nullptr;
            }
        }

        if (s_log) {
            SLogWrite(s_log, "%s", buffer);
            SLogFlush(s_log);
        }

        // Dropped by the console until it is initialised; the file has it either way.
        ConsolePrintf("SXC: %s", buffer);
    }
}

namespace SXC {
    void Log(const char* format, ...) {
        va_list args;
        va_start(args, format);
        Write(format, args);
        va_end(args);
    }

    void LogOnce(const char* key, const char* format, ...) {
        if (!s_once.insert(key).second) {
            return;
        }

        va_list args;
        va_start(args, format);
        Write(format, args);
        va_end(args);
    }
}
