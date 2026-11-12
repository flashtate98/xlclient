// What the liquid is at a point, terrain and WMO alike (SXC/Water/Liquid.cpp), and the sxc.water service
// (SXC/Api/sxc_water.h).
#pragma once

#include <cstdint>

class C3Vector;
struct sxc_water_api;

namespace SXC::Water {
    // A WMO's liquid where the point is inside its group, else the terrain's: the surface's height and its
    // LiquidType. False where there is none.
    bool LiquidAt(const C3Vector& position, float& height, uint32_t& liquidType);

    const sxc_water_api* Api();
}
