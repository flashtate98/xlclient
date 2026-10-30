// The shore field (kb: layer-liquid, the liquid map's fine tier): around the player, per cell of a sixteenth
// of a terrain chunk (~2.08 yd), whether terrain liquid stands there, its depth (surface minus ground), and
// the signed distance (yd, positive over water) and direction to the nearest shore. 256 x 256 cells (16 x 16
// chunks, +-267 yd), kept toroidally: a cell at global (gx, gy) lives at (gx & 255, gy & 255), so the
// window moves without copying. WMO liquid isn't in it.
//
// Sampled from the loaded chunks (the client's own liquid and terrain heights), a few changed chunks per
// frame; the distances made again when something changed, at most five times a second.
#pragma once

#include <cstdint>

namespace SXC::Water {
    constexpr int32_t kShoreSize = 256;           // cells per side
    constexpr float kShoreCell = 533.33333f / 256.0f;   // yd

    struct ShoreMap {
        bool valid = false;
        int32_t originX = 0, originY = 0;   // the window's first cell (global, the client's counting: x along -y)
        uint32_t version = 0;               // changes when `texels` does
        // Per stored cell (row-major, y then x): signed shore distance (yd), depth (yd), the direction to the
        // shore in world x and y (unit), as half floats.
        uint16_t texels[kShoreSize * kShoreSize * 4] = {};
    };

    // Brought up to date for this frame (call once per frame, from the water's first draw).
    const ShoreMap& ShoreField();
}
