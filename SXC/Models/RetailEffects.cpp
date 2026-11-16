#include "SXC/Models/RetailEffects.h"

namespace SXC::Models {

    namespace {
        bool s_edgeFade = false;
    }

    void SetDrawEdgeFade(bool on) {
        s_edgeFade = on;
    }

    float DrawEdgeFade() {
        return s_edgeFade ? 1.0f : 0.0f;
    }

}
