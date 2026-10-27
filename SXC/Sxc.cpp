#include "SXC/Sxc.h"

#include "Engine/Source/Event/Event.h"
#include "SXC/Data/Data.h"
#include "SXC/Settings/Settings.h"
#include "SXC/Terrain/TextureParams.h"

namespace SXC::Api {
    void Keep();
}

namespace {
    // Every frame, in the glue screens as in the world, whatever the renderer.
    int32_t Idle(const void* data, void* param) {
        SXC::Settings::Tick();
        SXC::Data::Tick();
        return 1;
    }
}

namespace SXC {
    void Startup() {
        static bool s_started;

        if (s_started) {
            return;
        }

        s_started = true;
        // The services mods reach through the sxc.bridge mod (SXC/Api): keep their object linked.
        Api::Keep();
        Terrain::Register();
        Data::Start();
        EventRegister(EVENT_ID_IDLE, Idle);
    }
}
