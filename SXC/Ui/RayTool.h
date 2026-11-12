#pragma once

// The Ray panel (Client module, kb: tools-overlay): what lies under the mouse and how far it is from the
// camera and the character - the terrain, a WMO group, a doodad with collision, the water's surface -
// plus pinned spots whose distances follow you as you move, and a ring around you at a chosen radius.
// For judging reaches and pop-in (the mirror's, the fog's, the grass's).
namespace SXC::RayTool {
    void DrawPanel(void* user);
}
