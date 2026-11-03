// The hang watchdog: a thread that notices when the main thread hasn't finished a frame for a second and
// writes where it is (its call stack, with function names and lines where the PDB is there) to
// Logs\hangs.log, again every second while it lasts, then how long it took. For freezes that come back
// by themselves and leave nothing else behind (Seyri, 2026-11-03). Off while a debugger is attached.
#pragma once

namespace SXC::Watchdog {
    // The main thread finished a frame (Profiler::FrameEnd). The first call starts the watchdog.
    void Beat();
}
