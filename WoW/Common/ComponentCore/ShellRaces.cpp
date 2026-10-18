#include "WoW/Common/ComponentCore/ShellRaces.h"

#include <cctype>
#include <cstddef>

#include "Engine/Source/Model2/M2Model.h"
#include "WoW/Common/ComponentCore/CharacterComponent.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/ItemDisplayInfoRec.h"

namespace {

// Races that wear shell armor. Add a race here once its model has 200xx shells and
// customization groups laid out like Esteria's Drakonid.
constexpr uint32_t kShellRaces[] = {
    20, // Drakonid (Alliance)
    21, // Drakonid (Horde)
};

// Helmet models that leave the horns visible. Taken from Esteria's catalog: it marks every display
// of every other helmet model as horn-hiding, with no model split both ways (checked against
// FlashWOW's ItemDisplayInfo: 270 models always hide, these 30 never do). Lower case, no extension.
constexpr const char* kHornSafeHelmets[] = {
    "helm_blindfold_a_01",
    "helm_circlet_a_01",
    "helm_circlet_b_01",
    "helm_circlet_b_02",
    "helm_circlet_c_01",
    "helm_cloth_b_05",
    "helm_draenei_giftofnaaru_01",
    "helm_engi_b_02",
    "helm_eyepatch_a_01",
    "helm_goggles_b_01",
    "helm_goggles_b_02engi",
    "helm_goggles_b_03",
    "helm_goggles_b_04",
    "helm_goggles_c_01",
    "helm_goggles_d_01",
    "helm_goggles_xray_a_01",
    "helm_leather_raiddruid_e_01",
    "helm_leather_raiddruid_g_01",
    "helm_magic_b_01",
    "helm_magic_b_02",
    "helm_magic_c_01",
    "helm_mail_a_01",
    "helm_misc_rose_a_01",
    "helm_monocle_a_01",
    "helm_monocle_b_01",
    "helm_monocle_b_02engi",
    "helm_monocle_c_01",
    "helm_plate_raidpaladin_f_01",
    "helm_robe_dungeonpriest_b_01",
    "helm_robe_holiday_summerfest_a_01",
};

// Case-insensitive: does modelName ("Helm_Circlet_A_01.mdx") name the model `base` ("helm_circlet_a_01")?
bool ModelIs(const char* modelName, const char* base) {
    size_t i = 0;

    for (; base[i]; i++) {
        if (!modelName[i] || std::tolower(static_cast<unsigned char>(modelName[i])) != base[i]) {
            return false;
        }
    }

    return modelName[i] == '\0' || modelName[i] == '.';
}

// Typical coverage by slot, for an equipped display the client has no ItemDisplayInfo row for
// (a server-only custom item).
uint32_t SlotFallback(int32_t slot) {
    switch (slot) {
        case ITEMSLOT_SHIRT:
        case ITEMSLOT_CHEST:
            return 0x1B; // arms + torso
        case ITEMSLOT_BELT:
            return 0x10; // lower torso
        case ITEMSLOT_PANTS:
            return 0x60; // upper + lower leg
        case ITEMSLOT_BOOTS:
            return 0xC0; // lower leg + foot
        case ITEMSLOT_BRACERS:
            return 0x02; // lower arm
        case ITEMSLOT_GLOVES:
            return 0x06; // lower arm + hand
        case ITEMSLOT_TABARD:
            return 0x38; // torso + upper leg
        default:
            return 0;
    }
}

} // namespace

bool ShellRaces::IsShellRace(uint32_t raceID) {
    for (uint32_t race : kShellRaces) {
        if (race == raceID) {
            return true;
        }
    }

    return false;
}

uint32_t ShellRaces::DisplayCoverage(const ItemDisplayInfoRec& rec) {
    uint32_t mask = 0;

    for (uint32_t region = 0; region < 8; region++) {
        if (rec.m_texture[region] && rec.m_texture[region][0]) {
            mask |= 1u << region;
        }
    }

    return mask;
}

bool ShellRaces::HelmetHidesHorns(const ItemDisplayInfoRec& rec) {
    const char* model = rec.m_modelName[0];

    if (!model || !model[0]) {
        return false;
    }

    for (const char* safe : kHornSafeHelmets) {
        if (ModelIs(model, safe)) {
            return false;
        }
    }

    return true;
}

void ShellRaces::ApplyGeosets(CCharacterComponent* component) {
    const CharacterPreferences& prefs = component->m_data.m_preferences;
    CM2Model* model = component->m_data.m_model;

    if (!model || !ShellRaces::IsShellRace(prefs.m_raceID)) {
        return;
    }

    ItemDisplayInfoRec rec;

    // Which body regions equipped gear covers (shirt through tabard).
    uint32_t coverage = 0;

    for (int32_t slot = ITEMSLOT_SHIRT; slot <= ITEMSLOT_TABARD; slot++) {
        uint32_t displayID = component->m_itemDisplayID[slot];

        if (!displayID) {
            continue;
        }

        coverage |= g_itemDisplayInfoDB.GetRecord(displayID, &rec) ? ShellRaces::DisplayCoverage(rec) : SlotFallback(slot);
    }

    bool hideHorns = false;
    uint32_t helmID = component->m_itemDisplayID[ITEMSLOT_HEAD];

    if (helmID && g_itemDisplayInfoDB.GetRecord(helmID, &rec)) {
        hideHorns = ShellRaces::HelmetHidesHorns(rec);
    }

    // Customization pieces. The stock rebuild only resets 0-2000, so each group is hidden whole
    // first and exactly one piece shown: two head shapes can never be visible at once.
    uint32_t face = prefs.m_faceId % 2;                              // head shape 3101 / 3102
    uint32_t horns = prefs.m_hairStyleId > 5 ? 0 : prefs.m_hairStyleId; // horns 3201..3206
    uint32_t spines = prefs.m_facialHairId % 2;                      // spines 3501 / 3502

    model->SetGeometryVisible(3100, 3199, 0);
    model->SetGeometryVisible(3101 + face, 3101 + face, 1);

    model->SetGeometryVisible(3200, 3299, 0);

    if (!hideHorns) {
        model->SetGeometryVisible(3201 + horns, 3201 + horns, 1);
    }

    model->SetGeometryVisible(3500, 3599, 0);
    model->SetGeometryVisible(3501 + spines, 3501 + spines, 1);

    // Armor shells: geoset 200xy is set x, region y-1. Set 0 fits either spine style, set 1 only
    // spines 3501, set 2 only spines 3502. Only regions the gear covers are shown.
    model->SetGeometryVisible(20000, 20029, 0);

    for (uint32_t geoset = 20001; geoset <= 20028; geoset++) {
        uint32_t set = (geoset - 20001) / 10;
        uint32_t region = (geoset - 20001) % 10;

        if (region >= 8 || !(coverage & (1u << region))) {
            continue;
        }

        if ((set == 1 && spines != 0) || (set == 2 && spines != 1)) {
            continue;
        }

        model->SetGeometryVisible(geoset, geoset, 1);
    }
}
