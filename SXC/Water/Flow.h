// The rivers' flow field (kb: rivers, layer-liquid): the rivers a mod hands over (sxc.water's set_rivers,
// each a path of nodes with a width and a speed) drawn into the shore field's window, so it moves and is
// stored as the shore field is (256 x 256 cells of ~2.08 yd, a cell at global (gx, gy) at (gx & 255,
// gy & 255)). Per cell: the flow (world x and y, yd / s), how rough the water is (0 calm, 1 rapids) and
// how much of a river is there (0..1); the banks slow the flow and turn it along them, from the shore
// field's distance and direction to the shore.
#pragma once

#include <cstdint>
#include "SXC/Water/Shore.h"

struct sxc_water_river;
struct sxc_water_river_node;

namespace SXC::Water {
    struct FlowMap {
        bool valid = false;
        int32_t originX = 0, originY = 0;   // the shore field's window it was made for
        uint32_t version = 0;               // changes when `texels` does
        // Per stored cell (row-major, y then x): the flow in world x and y (yd / s), roughness, weight, as
        // half floats.
        uint16_t texels[kShoreSize * kShoreSize * 4] = {};
    };

    // The rivers of the player's map (sxc.water's set_rivers): copied, replacing the last.
    void SetRivers(const sxc_water_river* rivers, uint32_t riverCount, const sxc_water_river_node* nodes, uint32_t nodeCount);

    // How many rivers and nodes were handed over.
    uint32_t RiverCount();
    uint32_t RiverNodeCount();

    // Made again for the shore field when it, its window or the rivers changed.
    const FlowMap& FlowField(const ShoreMap& shore);

    // The flow at a world point from the last field (yd / s, world x and y) and how much river is there;
    // false outside the field.
    bool FlowAt(float x, float y, float& flowX, float& flowY, float& weight);
}
