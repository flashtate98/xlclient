// The shore field (kb: layer-liquid, the liquid map's fine tier): around the player, per cell of a sixteenth
// of a terrain chunk (~2.08 yd), whether terrain liquid stands there, its depth (surface minus ground), and
// the signed distance (yd, positive over water) and direction to the nearest shore. 256 x 256 cells (16 x 16
// chunks, +-267 yd), kept toroidally: a cell at global (gx, gy) lives at (gx & 255, gy & 255), so the
// window moves without copying. WMO liquid isn't in it.
//
// Sampled from the loaded chunks (the client's own liquid and terrain heights), a few changed chunks per
// frame; the distances made again when something changed, at most five times a second.
//
// The cells also carry what our near water mesh (SXC/Water: drawn over the window in place of the stock
// terrain water) needs: the water's surface height (land takes its nearest water's), the ground, the
// LiquidType, and whether the mesh covers the cell: water, and land a little way from it where the water
// could reach (land below the nearest water's surface, a map's water cut short, farther; land above it,
// a few cells). Not where two waters at different heights meet (falls, stacked lakes): the stock mesh
// draws those.
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
        // Per stored cell, full floats (heights are world z): the surface (yd), the ground (yd), the
        // LiquidType, 1 where the near mesh covers the cell.
        float cells[kShoreSize * kShoreSize * 4] = {};
        // The LiquidTypes the near mesh covers.
        static constexpr uint32_t kMaxTypes = 8;
        uint32_t types[kMaxTypes] = {};
        uint32_t typeCount = 0;
    };

    // Brought up to date for this frame (call once per frame, from the water's first draw).
    const ShoreMap& ShoreField();

    // Why a water cell under a model's bounds counts as the model standing in the water (the Water panel's
    // "Model cells" markers): the rays' verdict, and whether the shore field took it (specks of a cell or
    // two stay water).
    enum class ModelVerdict : uint8_t {
        None,     // not under a model's bounds, or the down ray hit nothing
        Gap,      // something above, with a gap under it (a bridge): water
        Low,      // the first thing above the water is within 2.5 yd of it (a low rock, a hull, a pier)
        Inside,   // both rays hit the same point, and something stands at the waterline: a solid (a tall rock)
        Sheet,    // both rays hit the same point, but nothing at the waterline: a single sheet above (planks, a
                  // vault): water
    };

    struct ModelCell {
        float x = 0.0f, y = 0.0f, z = 0.0f;   // the cell's centre on the water's surface
        ModelVerdict verdict = ModelVerdict::None;
        float down = 0.0f;                    // yd above the water: where the down ray hit
        float up = -1.0f;                     // yd above the water: where the up ray hit, < 0 none
        bool speck = false;                   // counted, but too small: the shore field keeps it water
    };

    // The cells under models within `range` yd of (x, y) (verdict not None), at most `max`; how many.
    int32_t ModelCells(float x, float y, float range, ModelCell* out, int32_t max);
}
