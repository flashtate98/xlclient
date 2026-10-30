#ifndef WOW_SOURCE_SOUNDINTERFACE_SOUNDINTERFACE2EMOTES_H
#define WOW_SOURCE_SOUNDINTERFACE_SOUNDINTERFACE2EMOTES_H

#include <cstddef>
#include <cstdint>

// Not in the original: the stock table held races 0-21, so emote sounds of any later race (FlashWOW's
// Eredar, Naga, Broken, Dark Iron Dwarf, ...) were dropped. ChrRaces IDs are bytes; 64 covers every race used.
constexpr uint32_t kTextEmoteRaces = 64;

struct TEXTEMOTELOOKUP {
    /* 0000 */ int32_t m_soundIds[kTextEmoteRaces][2];
    /*      */ int32_t m_emotesTextId;

    TEXTEMOTELOOKUP();
};

#endif
