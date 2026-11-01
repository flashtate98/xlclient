#ifndef WOW_SOURCE_OBJECT_OBJECTCLIENT_ALTEREDFORM_H
#define WOW_SOURCE_OBJECT_OBJECTCLIENT_ALTEREDFORM_H

#include <cstdint>

// Not in the original: FlashWOW Two Forms, after Cataclysm's ChrRaces AlteredFormRaceID (which 3.3.5 lacks).
//
// A race with an altered form (Worgen) is drawn as that form's race (Gilnean) while the server sets
// kAlteredFormFlag2 in UNIT_FIELD_FLAGS_2 and the unit shows its native display. The form race has its own
// model (its ChrRaces display) and customization tables; the five appearance bytes and the stored Retail
// choices are the active form's, swapped by the server when the form changes. Druid forms, morphs and
// mounts change the display, so they show as usual and the form returns with the native display.
//
// The flag is Cataclysm's UNIT_FLAG2_WORGEN_TRANSFORM, unused in 3.3.5.
constexpr uint32_t kAlteredFormFlag2 = 0x00080000;

// The race a race's altered form is drawn as, 0 for a race without one.
inline uint32_t AlteredFormRace(uint32_t race) {
    switch (race) {
        case 16: return 54;     // Worgen -> Gilnean
        default: return 0;
    }
}

#endif
