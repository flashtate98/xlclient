#pragma once

// SXC: how NPCs with a CreatureDisplayInfoExtra row look (kb: npc-outfits).
//
// The stock client draws such an NPC's body with one baked texture (Textures\BakedNpcTextures\<BakeName>)
// and builds no character at all when the row has no BakeName. With baked textures ignored, the NPC is
// composited like a player instead: skin, face, hair and facial hair from the row, the item textures
// on top. Off by default (the stock look).
namespace SXC::Units {
    // [Units] IgnoreBakedNpcTextures. Read when a unit's look is built: NPCs already in view keep
    // theirs until they're rebuilt (out of range and back, a relog).
    bool IgnoreBakedNpcTextures();
}
