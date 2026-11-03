#ifndef ENGINE_SOURCE_MODEL2_M2LOG_H
#define ENGINE_SOURCE_MODEL2_M2LOG_H

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include "Engine/Source/Storm/H/storm.h"

// Not in the original: Model2's load errors (a missing file, a corrupt model, a skin that will not load) also
// go to Logs/Models.log. DebugLog prints nothing in release builds, so a model that failed only showed as the
// error cube, or not at all.
inline void M2Log(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    size_t length = strlen(buffer);
    while (length && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
        buffer[--length] = '\0';
    }

    DebugLog("%s\n", buffer);
    Blizzard::Log::Printf(1, "Logs/Models.log", "%s\n", buffer);
}

#endif
