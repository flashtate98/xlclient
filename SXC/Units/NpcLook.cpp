// SXC: how NPCs with a CreatureDisplayInfoExtra row look (SXC/Units/NpcLook.h).
#include "SXC/Units/NpcLook.h"

#include "SXC/Settings/Settings.h"

namespace SXC::Units {
    bool IgnoreBakedNpcTextures() {
        static SXC::Settings::Bool s_setting("Units", "IgnoreBakedNpcTextures", false, "Ignore baked NPC textures",
                                             "NPCs dressed like players: skin, face and hair from their CreatureDisplayInfoExtra row with "
                                             "their items' textures composited on top, instead of the row's low-resolution baked "
                                             "texture. Rows without a BakeName work too. Costs a texture composite per NPC. Applies to "
                                             "NPCs as they load (out of range and back, or relog).");
        return s_setting.Get();
    }
}
