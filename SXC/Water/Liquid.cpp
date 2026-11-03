// What the liquid is at a point, and the sxc.water service (SXC/Water/Liquid.h).
#include "SXC/Water/Liquid.h"

#include <cmath>
#include "Engine/Source/Tempest/C3Vector.h"
#include "SXC/Api/sxc_water.h"
#include "SXC/Water/Flow.h"
#include "SXC/Water/Ripples.h"
#include "WoW/Source/WorldClient/Map.h"

namespace SXC::Water {
    bool LiquidAt(const C3Vector& position, float& height, uint32_t& liquidType) {
        C3Vector p = position;
        int32_t type = 0;
        float h = 0.0f;

        if (CMap::QueryLiquidStatusMapObjsExt(&p, &type, &h) && std::isfinite(h)) {
            height = h;
            liquidType = static_cast<uint32_t>(type);
            return true;
        }

        // Far below every surface: the terrain's probe takes the first liquid above the point, i.e. the
        // surface here.
        p.z = -100000.0f;

        if (CMap::QueryLiquidTerrain(&p, &type, &h, 0) && std::isfinite(h)) {
            height = h;
            liquidType = static_cast<uint32_t>(type);
            return true;
        }

        return false;
    }
}

namespace {
    wxl_bool WXL_CALL ApiLiquidAt(const wxl_vec3* position, float* outHeight, uint32_t* outType) {
        float height = 0.0f;
        uint32_t type = 0;

        if (!position || !SXC::Water::LiquidAt(C3Vector(position->x, position->y, position->z), height, type)) {
            return WXL_FALSE;
        }

        if (outHeight) *outHeight = height;
        if (outType) *outType = type;
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL ApiRipple(float x, float y, float radius, float depth) {
        return SXC::Water::RippleAt(x, y, radius, depth) ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL ApiSetRivers(const sxc_water_river* rivers, uint32_t riverCount, const sxc_water_river_node* nodes, uint32_t nodeCount) {
        SXC::Water::SetRivers(rivers, riverCount, nodes, nodeCount);
    }

    wxl_bool WXL_CALL ApiFlowAt(float x, float y, float* outX, float* outY, float* outWeight) {
        float fx = 0.0f, fy = 0.0f, w = 0.0f;

        if (!SXC::Water::FlowAt(x, y, fx, fy, w)) {
            return WXL_FALSE;
        }

        if (outX) *outX = fx;
        if (outY) *outY = fy;
        if (outWeight) *outWeight = w;
        return WXL_TRUE;
    }

    const sxc_water_api s_api = { sizeof(sxc_water_api), ApiLiquidAt, ApiRipple, ApiSetRivers, ApiFlowAt };
}

namespace SXC::Water {
    const sxc_water_api* Api() {
        return &s_api;
    }
}
