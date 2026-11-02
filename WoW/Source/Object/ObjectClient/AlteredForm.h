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

// How a form change is animated (Retail's Worgen Two Forms). Into the race's own form, the altered body plays
// toNativeAnim (Retail 486); at toNativeSwapMs (per sex) the body is swapped and the own body plays the same
// animation on from that moment (its part is frozen before it). Into the altered form, the own body plays
// toAlteredAnim (496) and the body is swapped when it ends. A body without the animation swaps at once.
struct AlteredFormTransition {
    uint32_t toNativeAnim;
    uint32_t toNativeSwapMs[2];     // male, female
    uint32_t toAlteredAnim;
};

inline const AlteredFormTransition* AlteredFormTransitionFor(uint32_t race) {
    static const AlteredFormTransition kWorgen = { 486, { 3200, 2700 }, 496 };
    switch (race) {
        case 16: return &kWorgen;
        default: return nullptr;
    }
}

// The race a race's altered form is drawn as, 0 for a race without one.
inline uint32_t AlteredFormRace(uint32_t race) {
    switch (race) {
        case 16: return 54;     // Worgen -> Gilnean
        default: return 0;
    }
}

#endif
