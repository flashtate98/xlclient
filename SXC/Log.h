#pragma once

// SXC's log: Logs\sxc.log beside the client (created on first use, fresh each session) and the
// in-game console. For things a modder should look into, such as broken custom data, not for
// per-frame tracing.
namespace SXC {
    void Log(const char* format, ...);

    // Like Log, but only the first time `key` is seen in this session, so a problem hit every frame
    // is reported once. `key` is copied.
    void LogOnce(const char* key, const char* format, ...);
}
