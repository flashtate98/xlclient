#pragma once

// SXC's entry points from the client.
namespace SXC {
    // Once, from InitializeClient after the client's DBCs are loaded (SFile works, the window may not
    // exist yet): loads the tables registered so far and registers SXC's per-frame work (the idle
    // event). Mods register theirs later, at WXL_STAGE_READY, through sxc.data.
    void Startup();
}
