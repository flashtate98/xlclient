#include <cstdint>
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Tempest/CAaSphere.h"
#include "Engine/Source/Tempest/CFrustum.h"

// SXC: a frustum's six planes from its corners (as CalcPlanesFromCorners), facing inward.
namespace {
    void PlanesFrom(const C3Vector* corners, C4Plane* planes) {
        planes[P_TOP].From3Pos(corners[NEAR_UL], corners[FAR_UL], corners[FAR_UR]);
        planes[P_BOTTOM].From3Pos(corners[NEAR_LL], corners[FAR_LR], corners[FAR_LL]);
        planes[P_LEFT].From3Pos(corners[NEAR_LL], corners[FAR_LL], corners[FAR_UL]);
        planes[P_RIGHT].From3Pos(corners[NEAR_LR], corners[FAR_UR], corners[FAR_LR]);
        planes[P_FAR].From3Pos(corners[FAR_UL], corners[FAR_LL], corners[FAR_UR]);
        planes[P_NEAR].n.x = -planes[P_FAR].n.x;
        planes[P_NEAR].n.y = -planes[P_FAR].n.y;
        planes[P_NEAR].n.z = -planes[P_FAR].n.z;
        planes[P_NEAR].d = -(planes[P_NEAR].n.z * corners[NEAR_UR].z + planes[P_NEAR].n.y * corners[NEAR_UR].y + planes[P_NEAR].n.x * corners[NEAR_UR].x);

        // A mirrored set of corners winds the other way: every plane faces out. Turned round if the middle
        // of the corners lies outside.
        C3Vector middle(0.0f, 0.0f, 0.0f);

        for (uint32_t i = 0; i < NUM_CORNERS; i++) {
            middle.x += corners[i].x / NUM_CORNERS;
            middle.y += corners[i].y / NUM_CORNERS;
            middle.z += corners[i].z / NUM_CORNERS;
        }

        if (planes[P_TOP].n.x * middle.x + planes[P_TOP].n.y * middle.y + planes[P_TOP].n.z * middle.z + planes[P_TOP].d < 0.0f) {
            for (uint32_t i = 0; i < NUM_PLANES; i++) {
                planes[i].n.x = -planes[i].n.x;
                planes[i].n.y = -planes[i].n.y;
                planes[i].n.z = -planes[i].n.z;
                planes[i].d = -planes[i].d;
            }
        }
    }

    bool BoxOutside(const C4Plane* planes, const CAaBox& box) {
        const C3Vector* extremes[2] = { &box.t, &box.b };

        for (uint32_t i = 0; i < NUM_PLANES; i++) {
            const C4Plane& plane = planes[i];
            const C3Vector* x = extremes[*reinterpret_cast<const uint32_t*>(&plane.n.x) >> 31];
            const C3Vector* y = extremes[*reinterpret_cast<const uint32_t*>(&plane.n.y) >> 31];
            const C3Vector* z = extremes[*reinterpret_cast<const uint32_t*>(&plane.n.z) >> 31];

            if (z->z * plane.n.z + y->y * plane.n.y + x->x * plane.n.x + plane.d < -0.019444443f) {
                return true;
            }
        }

        return false;
    }

    bool SphereOutside(const C4Plane* planes, const CAaSphere& sphere) {
        for (uint32_t i = 0; i < NUM_PLANES; i++) {
            const C4Plane& plane = planes[i];

            if (plane.n.x * sphere.c.x + plane.n.z * sphere.c.z + plane.n.y * sphere.c.y + plane.d < -sphere.r) {
                return true;
            }
        }

        return false;
    }
}

void CFrustum::SetOr(const C3Vector* corners) {
    for (uint32_t i = 0; i < NUM_CORNERS; i++) {
        this->orCorners[i] = corners[i];
    }

    PlanesFrom(this->orCorners, this->orPlanes);
    this->hasOr = true;
}

void CFrustum::ClearOr() {
    this->hasOr = false;
}

// VERIFY: e0dd73d2 auto
// OFFSET: 0x00983990
void CFrustum::Extents(CAaBox& box) {
    box = CAaBox::Bounding(this->corners, NUM_CORNERS);
}

// VERIFY: 835d8937 auto
// OFFSET: 0x009839E0
WorldCullStatus CFrustum::Cull(const CAaBox& box) {
    const C3Vector* extremes[2] = { &box.t, &box.b };

    for (uint32_t i = 0; i < NUM_PLANES; i++) {
        const C4Plane& plane = this->planes[i];
        const C3Vector* x = extremes[*reinterpret_cast<const uint32_t*>(&plane.n.x) >> 31];
        const C3Vector* y = extremes[*reinterpret_cast<const uint32_t*>(&plane.n.y) >> 31];
        const C3Vector* z = extremes[*reinterpret_cast<const uint32_t*>(&plane.n.z) >> 31];

        if (z->z * plane.n.z + y->y * plane.n.y + x->x * plane.n.x + plane.d < -0.019444443f) {
            return this->hasOr && !BoxOutside(this->orPlanes, box) ? WorldCull_notOutside : WorldCull_outside;  // SXC: or the second volume
        }
    }

    return WorldCull_notOutside;
}

// VERIFY: 93cc8ec1 auto
// OFFSET: 0x00983A60
WorldCullStatus CFrustum::CullInv(const CAaBox& box) {
    const C3Vector* extremes[2] = { &box.t, &box.b };

    for (uint32_t i = 0; i < NUM_PLANES; i++) {
        const C4Plane& plane = this->planes[i];
        const C3Vector* x = extremes[*reinterpret_cast<const uint32_t*>(&plane.n.x) >> 31];
        const C3Vector* y = extremes[*reinterpret_cast<const uint32_t*>(&plane.n.y) >> 31];
        const C3Vector* z = extremes[*reinterpret_cast<const uint32_t*>(&plane.n.z) >> 31];

        if (z->z * plane.n.z + y->y * plane.n.y + x->x * plane.n.x + plane.d > 0.019444443f) {
            return WorldCull_outside;
        }
    }

    return WorldCull_notOutside;
}

// VERIFY: 2f6339ef manual
// OFFSET: 0x00983AE0
void CFrustum::Translate(const C3Vector& v) {
    for (uint32_t i = 0; i < NUM_CORNERS; i++) {
        this->corners[i] += v;
    }

    for (uint32_t i = 0; i < NUM_PLANES; i++) {
        this->planes[i].d = this->planes[i].d - (this->planes[i].n.z * v.z + this->planes[i].n.y * v.y + v.x * this->planes[i].n.x);
    }

    this->lookPos += v;
    this->lookAt += v;

    if (this->hasOr) {  // SXC: the second volume with it
        for (uint32_t i = 0; i < NUM_CORNERS; i++) {
            this->orCorners[i] += v;
        }

        PlanesFrom(this->orCorners, this->orPlanes);
    }
}

// VERIFY: b3982b68 manual
// OFFSET: 0x00983D20
WorldCullStatus CFrustum::Cull(const CAaSphere& sphere) {
    float limit = -sphere.r;

    for (uint32_t i = 0; i < NUM_PLANES; i++) {
        const C4Plane& plane = this->planes[i];

        if (plane.n.x * sphere.c.x + plane.n.z * sphere.c.z + plane.n.y * sphere.c.y + plane.d < limit) {
            return this->hasOr && !SphereOutside(this->orPlanes, sphere) ? WorldCull_notOutside : WorldCull_outside;  // SXC: or the second volume
        }
    }

    return WorldCull_notOutside;
}

// VERIFY: 3dfc1f7d manual
// OFFSET: 0x00983D70
void CFrustum::Cull(const C3Vector& point, uint8_t& outside) const {
    outside = 0;

    for (uint32_t i = 0; i < NUM_PLANES; i++) {
        const C4Plane& plane = this->planes[i];

        if (plane.n.z * point.z + plane.n.y * point.y + point.x * plane.n.x + plane.d < -0.019444443f) {
            outside |= 1 << i;
        }
    }
}

// VERIFY: ccfad69b manual
// OFFSET: 0x00983E70
void CFrustum::CalcPlanesFromCorners() {
    this->planes[P_TOP].From3Pos(this->corners[NEAR_UL], this->corners[FAR_UL], this->corners[FAR_UR]);
    this->planes[P_BOTTOM].From3Pos(this->corners[NEAR_LL], this->corners[FAR_LR], this->corners[FAR_LL]);
    this->planes[P_LEFT].From3Pos(this->corners[NEAR_LL], this->corners[FAR_LL], this->corners[FAR_UL]);
    this->planes[P_RIGHT].From3Pos(this->corners[NEAR_LR], this->corners[FAR_UR], this->corners[FAR_LR]);
    this->planes[P_FAR].From3Pos(this->corners[FAR_UL], this->corners[FAR_LL], this->corners[FAR_UR]);

    this->planes[P_NEAR].n.x = -this->planes[P_FAR].n.x;
    this->planes[P_NEAR].n.y = -this->planes[P_FAR].n.y;
    this->planes[P_NEAR].n.z = -this->planes[P_FAR].n.z;
    this->planes[P_NEAR].d = -(this->planes[P_NEAR].n.z * this->corners[NEAR_UR].z + this->planes[P_NEAR].n.y * this->corners[NEAR_UR].y + this->planes[P_NEAR].n.x * this->corners[NEAR_UR].x);
}

// VERIFY: 88718e38 auto
// OFFSET: 0x00983F40
void CFrustum::Transform(const C44Matrix& m) {
    for (uint32_t i = 0; i < NUM_CORNERS; i++) {
        this->corners[i] *= m;
    }

    this->CalcPlanesFromCorners();

    this->lookPos *= m;
    this->lookAt *= m;

    if (this->hasOr) {  // SXC: the second volume with it
        for (uint32_t i = 0; i < NUM_CORNERS; i++) {
            this->orCorners[i] *= m;
        }

        PlanesFrom(this->orCorners, this->orPlanes);
    }
}

// VERIFY: 75e79820 auto
// OFFSET: 0x00983FB0
WorldCullStatus CFrustum::Cull(const C3Vector& center, float radius) {
    CAaSphere sphere;
    sphere.c = center;
    sphere.r = radius;
    return this->Cull(sphere);
}

// VERIFY: 0ece7724 manual
// OFFSET: 0x00601650
CFrustum::CFrustum() {
    for (uint32_t i = 0; i < NUM_PLANES; i++) {
        this->planes[i].n.x = 0.0f;
        this->planes[i].n.y = 0.0f;
        this->planes[i].d = 0.0f;
        this->planes[i].n.z = 1.0f;
    }

    for (uint32_t i = 0; i < NUM_CORNERS; i++) {
        this->corners[i].x = 0.0f;
        this->corners[i].y = 0.0f;
        this->corners[i].z = 0.0f;
    }

    this->lookPos.x = 0.0f;
    this->lookPos.y = 0.0f;
    this->lookPos.z = 0.0f;
    this->lookAt.x = 0.0f;
    this->lookAt.y = 0.0f;
    this->lookAt.z = 0.0f;
    this->lookUp.x = 0.0f;
    this->lookUp.y = 0.0f;
    this->lookUp.z = 0.0f;
}

// VERIFY: 6f48db85 manual
// OFFSET: 0x00983FE0
CFrustum::CFrustum(const C3Vector* corners) {
    for (uint32_t i = 0; i < NUM_PLANES; i++) {
        this->planes[i].n.x = 0.0f;
        this->planes[i].n.y = 0.0f;
        this->planes[i].n.z = 1.0f;
        this->planes[i].d = 0.0f;
    }

    for (uint32_t i = 0; i < NUM_CORNERS; i++) {
        this->corners[i] = corners[i];
    }

    this->CalcPlanesFromCorners();
}

// VERIFY: 6d9a59ac manual
// OFFSET: 0x00984240
void CFrustum::CalcPlanesFromCorners(const C3Vector* corners) {
    for (uint32_t i = 0; i < NUM_CORNERS; i++) {
        this->corners[i] = corners[i];
    }

    this->CalcPlanesFromCorners();
}

// VERIFY: 1bd5200d auto
// OFFSET: 0x00790020
CFrustum& CFrustum::operator=(const CFrustum& source) {
    if (this != &source) {
        for (uint32_t i = 0; i < NUM_PLANES; i++) {
            this->planes[i] = source.planes[i];
        }

        for (uint32_t i = 0; i < NUM_CORNERS; i++) {
            this->corners[i] = source.corners[i];
        }

        this->lookPos = source.lookPos;
        this->lookAt = source.lookAt;
        this->lookUp = source.lookUp;
        this->fovy = source.fovy;
        this->aspect = source.aspect;
        this->minz = source.minz;
        this->maxz = source.maxz;

        this->hasOr = source.hasOr;  // SXC: the second volume

        if (source.hasOr) {
            for (uint32_t i = 0; i < NUM_CORNERS; i++) {
                this->orCorners[i] = source.orCorners[i];
            }

            for (uint32_t i = 0; i < NUM_PLANES; i++) {
                this->orPlanes[i] = source.orPlanes[i];
            }
        }
    }

    return *this;
}

// VERIFY: cc1bdc3a manual
// OFFSET: 0x007BA960
void CFrustum::InvertPlanes() {
    for (uint32_t i = 0; i < NUM_PLANES; i++) {
        this->planes[i].n.x = -this->planes[i].n.x;
        this->planes[i].n.y = -this->planes[i].n.y;
        this->planes[i].n.z = -this->planes[i].n.z;
        this->planes[i].d = -this->planes[i].d;
    }
}

// VERIFY: aa9403a2 auto
// OFFSET: 0x007BAAA0
void CFrustum::SwapCorners() {
    C3Vector corner = this->corners[NEAR_LL];
    this->corners[NEAR_LL] = this->corners[NEAR_LR];
    this->corners[NEAR_LR] = corner;

    corner = this->corners[NEAR_UL];
    this->corners[NEAR_UL] = this->corners[NEAR_UR];
    this->corners[NEAR_UR] = corner;

    corner = this->corners[FAR_LL];
    this->corners[FAR_LL] = this->corners[FAR_LR];
    this->corners[FAR_LR] = corner;

    corner = this->corners[FAR_UL];
    this->corners[FAR_UL] = this->corners[FAR_UR];
    this->corners[FAR_UR] = corner;
}
