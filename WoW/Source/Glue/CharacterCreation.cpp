#include "CharacterCreation.h"
#include <cstring>
#include <vector>  // SXC: GetRandomClassID
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/CRandom.h"
#include "WoW/Common/ComponentCore/CharacterComponent.h"
#include "WoW/Common/ComponentCore/RetailCharacter.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/CharBaseInfoRec.h"
#include "WoW/Source/DB/Rec/CharStartOutfitRec.h"
#include "WoW/Source/DB/Rec/ChrClassesRec.h"
#include "WoW/Source/DB/Rec/ChrRacesRec.h"
#include "WoW/Source/DB/Rec/CreatureModelDataRec.h"
#include "WoW/Source/DB/Rec/ItemDisplayInfoRec.h"
#include "WoW/Source/Game/GameClient/CDrawBowString.h"
#include "WoW/Source/Glue/CGlueLoading.h"
#include "WoW/Source/Glue/CGlueMgr.h"
#include "WoW/Source/Glue/CharacterSelection.h"
#include "WoW/Source/Glue/ComponentData.h"
#include "WoW/Source/Glue/CSimpleModelFFX.h"
#include "WoW/Source/Glue/Types.h"
#include "WoW/Source/LoadingScreen.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/WowServices/Types.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/ClientServices.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/Types.h"
#if defined(WOWDECOMP_RETAIL_MODELS)
#include <map>
#include "WoW/Source/Object/ObjectClient/AlteredForm.h"
#include <algorithm>
#include <cmath>
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#endif

// STATIC: 0x00AC4220
int32_t CCharacterCreation::m_selectedClassID = 1;

// STATIC: 0x00AC4224
int32_t CCharacterCreation::m_existingCharacterIndex = -1;

// STATIC: 0x00B6B0D0
CharacterPreferences* CCharacterCreation::m_charPreferences[128][2]; // FlashWOW: was [22][2]

// STATIC: 0x00B6B180
uint32_t CCharacterCreation::m_raceIndex;

// STATIC: 0x00B6B184
CSimpleModelFFX* CCharacterCreation::m_charCustomizeFrame;

// STATIC: 0x00B6B188
float CCharacterCreation::m_charFacing;

// STATIC: 0x00B6B18C
uint32_t CCharacterCreation::m_prevSkinIndex;

// STATIC: 0x00B6B190
uint32_t CCharacterCreation::m_prevFaceIndex;

// STATIC: 0x00B6B194
uint32_t CCharacterCreation::m_prevHairColorIndex;

// STATIC: 0x00B6B198
uint32_t CCharacterCreation::m_prevHairStyleIndex;

// STATIC: 0x00B6B19C
uint32_t CCharacterCreation::m_prevFacialHairIndex;

// STATIC: 0x00B6B1A0
CCharacterComponent* CCharacterCreation::m_character;

// STATIC: 0x00B6B1A4
TSFixedArray<ChrClassesRec*> CCharacterCreation::m_classes;

// STATIC: 0x00B6B1CC
TSGrowableArray<int32_t> CCharacterCreation::m_races;

// STATIC: 0x00B6B1DC
CNameGen CCharacterCreation::m_nameGen;

#if defined(WOWDECOMP_RETAIL_MODELS)
// Not in the original: FlashWOW Two Forms on the create screen (Cataclysm's SetViewingAlteredForm). A race with
// an altered form (AlteredForm.h) is edited in both forms: one component shows the form being edited, and the
// other form's look waits here, per race and sex, until the player switches back or creates the character.
// The selected race stays the race's own; the character is always created from its own form, and the
// altered form's look goes along with the create packet (RetailCustomizationNet.cpp).
namespace {

struct ParkedForm {
    bool valid = false;
    CharacterPreferences preferences = {};
    RetailCustomizationExtras extras;
};

bool s_viewingAlteredForm = false;
bool s_clothingHidden = false;
// Create screen zoom (CharacterCreationZoom): 0 shows the scene's own camera, 1 the character's head close up.
// s_zoom eases toward s_zoomGoal every frame.
float s_zoom = 0.0f;
float s_zoomGoal = 0.0f;
uint32_t s_zoomTime = 0;
// Per-race tuning from the glue (SetCharacterCreateZoomTuning): the aim moved up and right by these shares of
// the character's height, and the closest the camera may come (world units).
float s_zoomUp = 0.0f;
float s_zoomRight = 0.0f;
float s_zoomMinDistance = 0.0f;
float s_zoomFrame = 0.0f;      // 0: kZoomFrame
std::map<int32_t, ParkedForm[2]> s_parkedForms;     // by the race's own ID, then sex

int32_t SelectedBaseRace() {
    if (CCharacterCreation::m_raceIndex < CCharacterCreation::m_races.Count()) {
        return CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex];
    }

    return CCharacterCreation::m_character ? CCharacterCreation::m_character->m_data.m_preferences.m_raceID : 0;
}

// Shows the race's own form (false) or its altered form (true), parking the form shown until now.
void ShowForm(bool altered) {
    CCharacterComponent* character = CCharacterCreation::m_character;
    const int32_t race = SelectedBaseRace();
    const uint32_t form = AlteredFormRace(race);

    if (!character || !form || altered == s_viewingAlteredForm) {
        return;
    }

    const uint32_t sex = character->m_data.m_preferences.m_sexID & 1;
    ParkedForm& parked = s_parkedForms[race][sex];

    ParkedForm shown;
    shown.valid = true;
    character->GetPreferences(&shown.preferences);
    RetailCustomizationTakeExtras(character, shown.extras);

    ComponentData data;
    const bool randomize = !parked.valid;

    if (parked.valid) {
        data.SetPreferences(&parked.preferences);
    }

    data.m_preferences.m_raceID = altered ? form : race;
    data.m_preferences.m_sexID = character->m_data.m_preferences.m_sexID;
    data.m_preferences.m_classId = CCharacterCreation::m_selectedClassID;

    if (parked.valid) {
        CCharacterComponent::ValidateComponentData(&data, COMPONENT_CONTEXT_0);
    }

    // InitCharacterComponents carries the shown component's Retail rows over; hand it the other form's.
    RetailCustomizationDiscard(character);
    RetailCustomizationGiveExtras(character, parked.extras);
    parked = shown;
    s_viewingAlteredForm = altered;
    CCharacterCreation::InitCharacterComponents(&data, randomize);
    CCharacterCreation::Dress();
    CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
}

}

bool CharacterCreationHasAlteredForm() {
    const uint32_t form = AlteredFormRace(SelectedBaseRace());
    return CCharacterCreation::m_existingCharacterIndex < 0 && CCharacterCreation::m_character && form && g_chrRacesDB.GetRecord(form);
}

// How close the camera comes at full zoom: the share of the character's height that fills the view
// (0.35: head and shoulders). The glue can set its own (SetCharacterCreateZoomTuning).
constexpr float kZoomFrame = 0.35f;
// The face's share of the character's height, for models without a Head attachment and for framing.
constexpr float kZoomFocus = 0.86f;
// How fast the zoom follows the wheel (per second).
constexpr float kZoomEase = 9.0f;
// How fast the aim follows the head up and down (per second); low, to keep idle animations from rocking it.
constexpr float kZoomSettle = 1.2f;
// The Head attachment (where helmets go).
constexpr uint32_t kHeadAttachment = 11;
// The closest the camera ever comes to the aim point (world units), whatever the race asks for.
constexpr float kZoomMinDistance = 0.45f;

// The camera hook (g_simpleModelCameraHook): for the create screen's model frame, moves the camera from the
// scene's own toward the character's face by s_zoom.
bool CharacterCreationZoomCamera(CSimpleModel* frame, C3Vector& eye, C3Vector& target, float fov) {
    if (!CCharacterCreation::m_charCustomizeFrame || frame != CCharacterCreation::m_charCustomizeFrame) {
        return false;
    }

    const uint32_t now = OsGetAsyncTimeMs();
    const float elapsed = s_zoomTime ? std::min(0.1f, (now - s_zoomTime) / 1000.0f) : 0.0f;
    s_zoomTime = now;
    s_zoom += (s_zoomGoal - s_zoom) * std::min(1.0f, elapsed * kZoomEase);
    if (std::fabs(s_zoomGoal - s_zoom) < 0.001f) {
        s_zoom = s_zoomGoal;
    }
    if (s_zoom <= 0.0f) {
        return false;
    }

    // Where the camera looks: straight above the character's feet (the point it turns about, so turning the
    // character leaves the camera still), at the height of its Head attachment (the point helmets hang on).
    // The height eases, so breathing and idle animations do not rock the camera. Models without a Head
    // attachment use kZoomFocus of their bounds. While the model reloads (a new race or sex) the last point
    // is kept, so the camera does not jump back out.
    static C3Vector s_face(0.0f, 0.0f, 0.0f);
    static float s_height = 0.0f;
    static bool s_haveFace = false;
    CCharacterComponent* character = CCharacterCreation::m_character;
    CM2Model* model = character ? character->m_data.m_model : nullptr;
    if (model && model->m_scene && model->IsLoaded(0, 0)) {
        const C3Vector feet = C3Vector(0.0f, 0.0f, 0.0f) * model->m_matrixF4 * model->m_scene->m_viewInv;
        float headHeight = 0.0f;
        if (model->HasAttachment(kHeadAttachment)) {
            const C3Vector head = model->GetAttachmentPosition(kHeadAttachment);
            headHeight = head.z - feet.z;
        }
        if (!(headHeight > 0.0f)) {
            const CAaBox box = model->GetBoundingBox();
            const C3Vector top = C3Vector(0.0f, 0.0f, box.t.z) * model->m_matrixF4 * model->m_scene->m_viewInv;
            headHeight = (top.z - feet.z) * kZoomFocus;
        }
        if (headHeight > 0.0f) {
            const C3Vector goal(feet.x, feet.y, feet.z + headHeight);
            const float height = headHeight / kZoomFocus;
            const float follow = s_haveFace ? std::min(1.0f, elapsed * kZoomSettle) : 1.0f;
            s_face = C3Vector(goal.x, goal.y, s_face.z + (goal.z - s_face.z) * follow);
            s_height += (height - s_height) * follow;
            s_haveFace = true;
        }
    }
    const float worldHeight = s_height;
    const C3Vector face = s_face;

    // Back from the face along the scene camera's own line of sight, far enough to frame kZoomFrame of the height.
    C3Vector back(eye.x - target.x, eye.y - target.y, eye.z - target.z);
    const float length = std::sqrt(back.x * back.x + back.y * back.y + back.z * back.z);
    const float half = std::tan(std::max(0.1f, fov) * 0.5f);
    if (!(length > 0.001f) || !(worldHeight > 0.0f)) {
        return false;
    }
    // Never so close that the camera ends up inside the head (small and hunched bodies frame very tight).
    const float frameShare = s_zoomFrame > 0.0f ? s_zoomFrame : kZoomFrame;
    const float distance = std::max(std::max(s_zoomMinDistance, kZoomMinDistance), worldHeight * frameShare * 0.5f / half);
    // The per-race nudge: up the world's Z, and right across the view (the line of sight crossed with up).
    const float rightX = -back.y / length;
    const float rightY = back.x / length;
    const float rightLength = std::sqrt(rightX * rightX + rightY * rightY);
    C3Vector aim = face;
    aim.z += s_zoomUp * worldHeight;
    if (rightLength > 0.001f) {
        aim.x += rightX / rightLength * s_zoomRight * worldHeight;
        aim.y += rightY / rightLength * s_zoomRight * worldHeight;
    }
    const C3Vector closeEye(aim.x + back.x / length * distance, aim.y + back.y / length * distance, aim.z + back.z / length * distance);

    // Diagnostics: one line in Logs/Zoom.log each time the zoom settles on a new value.
    static float s_logged = -1.0f;
    if (s_zoom == s_zoomGoal && s_logged != s_zoomGoal) {
        s_logged = s_zoomGoal;
        Blizzard::Log::Printf(1, "Logs/Zoom.log",
            "zoom %.2f fov %.3f eye (%.3f %.3f %.3f) target (%.3f %.3f %.3f) face (%.3f %.3f %.3f) height %.3f distance %.3f\n",
            s_zoom, fov, eye.x, eye.y, eye.z, target.x, target.y, target.z, face.x, face.y, face.z, worldHeight, distance);
    }

    // Smoothstep, so the move starts and ends gently.
    const float t = s_zoom * s_zoom * (3.0f - 2.0f * s_zoom);
    eye = C3Vector(eye.x + (closeEye.x - eye.x) * t, eye.y + (closeEye.y - eye.y) * t, eye.z + (closeEye.z - eye.z) * t);
    target = C3Vector(target.x + (aim.x - target.x) * t, target.y + (aim.y - target.y) * t, target.z + (aim.z - target.z) * t);
    return true;
}

void CharacterCreationSetZoomTuning(float up, float right, float minDistance, float frame) {
    s_zoomUp = up;
    s_zoomRight = right;
    s_zoomMinDistance = std::max(0.0f, minDistance);
    s_zoomFrame = frame > 0.0f ? std::min(frame, 2.0f) : 0.0f;
}

float CharacterCreationZoom() {
    return s_zoomGoal;
}

void CharacterCreationSetZoom(float zoom, bool instant) {
    s_zoomGoal = std::max(0.0f, std::min(1.0f, zoom));
    if (instant) {
        s_zoom = s_zoomGoal;
    }
    g_simpleModelCameraHook = &CharacterCreationZoomCamera;
}

bool CharacterCreationClothingHidden() {
    return s_clothingHidden;
}

void CharacterCreationSetClothingHidden(bool hidden) {
    if (hidden == s_clothingHidden) {
        return;
    }
    s_clothingHidden = hidden;
    if (CCharacterCreation::m_character) {
        CCharacterCreation::Dress();
        CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
    }
}

bool CharacterCreationViewingAlteredForm() {
    return s_viewingAlteredForm;
}

void CharacterCreationSetViewingAlteredForm(bool altered) {
    if (!altered || CharacterCreationHasAlteredForm()) {
        ShowForm(altered);
    }
}

bool CharacterCreationAlteredFormLook(uint8_t look[5], RetailCustomizationExtras& extras) {
    extras.clear();
    const int32_t race = SelectedBaseRace();

    if (!CCharacterCreation::m_character || !AlteredFormRace(race) || s_viewingAlteredForm) {
        return false;
    }

    const auto found = s_parkedForms.find(race);

    if (found == s_parkedForms.end()) {
        return false;
    }

    const ParkedForm& parked = found->second[CCharacterCreation::m_character->m_data.m_preferences.m_sexID & 1];

    if (!parked.valid) {
        return false;
    }

    look[0] = static_cast<uint8_t>(parked.preferences.m_skinID);
    look[1] = static_cast<uint8_t>(parked.preferences.m_faceId);
    look[2] = static_cast<uint8_t>(parked.preferences.m_hairStyleId);
    look[3] = static_cast<uint8_t>(parked.preferences.m_hairColorId);
    look[4] = static_cast<uint8_t>(parked.preferences.m_facialHairId);
    extras = parked.extras;
    return true;
}
#endif

// VERIFY: cbaddeb2 manual
// OFFSET: 0x004E1C20
void CCharacterCreation::Initialize() {
    CCharacterCreation::m_charFacing = 0.0f;
    CCharacterCreation::m_charCustomizeFrame = nullptr;
    CCharacterCreation::m_existingCharacterIndex = -1;
    memset(CCharacterCreation::m_charPreferences, 0, sizeof(CCharacterCreation::m_charPreferences));
    CCharacterCreation::m_races.m_count = 0;

    for (int32_t faction = 0; faction < 2; faction++) {
        for (int32_t i = 0; i < g_chrRacesDB.m_numRecords; i++) {
            ChrRacesRec* race = i >= 0 && i < g_chrRacesDB.m_numRecords ? &g_chrRacesDB.m_records[i] : nullptr;

            if (race && (race->m_flags & 0x1)) {
                continue;
            }

            FactionTemplateRec* factionTemplate = g_factionTemplateDB.GetRecord(race->m_factionID);

            if (!factionTemplate) {
                continue;
            }

            for (int32_t j = 0; j < g_factionGroupDB.m_numRecords; j++) {
                FactionGroupRec* group = j >= 0 && j < g_factionGroupDB.m_numRecords ? &g_factionGroupDB.m_records[j] : nullptr;

                if (!group || !group->m_maskID || !((1 << group->m_maskID) & factionTemplate->m_factionGroup)) {
                    continue;
                }

                if (!SStrCmpI(group->m_internalName, faction == 0 ? "Alliance" : "Horde", STORM_MAX_STR)) {
                    CCharacterCreation::m_races.New(race->m_id);
                }
            }
        }
    }
}

// VERIFY: 3b1595cf manual
// OFFSET: 0x004E1E20
void CCharacterCreation::Shutdown() {
    if (CCharacterCreation::m_character) {
        CCharacterComponent::FreeComponent(CCharacterCreation::m_character);
        CCharacterCreation::m_character = nullptr;
    }

    CCharacterCreation::m_nameGen.m_names.Clear();
    CCharacterCreation::m_nameGen.m_dictionary.Clear();

    for (uint32_t i = 0; i < 128; i++) { // FlashWOW: was 22
        for (uint32_t j = 0; j < 2; j++) {
            delete CCharacterCreation::m_charPreferences[i][j];
            CCharacterCreation::m_charPreferences[i][j] = nullptr;
        }
    }
}

// VERIFY: e90576fb auto
// OFFSET: 0x004DFA50
int32_t CCharacterCreation::Idle(const void* data, void* param) {
    CGlueLoading::Idle(CCharacterCreation::m_character);
    return 1;
}

// VERIFY: 95595a1c manual
// OFFSET: 0x004DFF10
void CCharacterCreation::GetRandomRaceAndSex(ComponentData* data) {
    data->m_preferences.m_sexID = static_cast<uint32_t>((static_cast<uint64_t>(2) * CRandom::uint32_(g_rndSeed)) >> 32);

    ChrRacesRec* raceRec;

    do {
        uint32_t count = CCharacterCreation::m_races.Count();
        uint32_t index = count > 0 ? static_cast<uint32_t>((static_cast<uint64_t>(count) * CRandom::uint32_(g_rndSeed)) >> 32) : 0;
        data->m_preferences.m_raceID = CCharacterCreation::m_races.m_data[index];
        raceRec = g_chrRacesDB.GetRecord(data->m_preferences.m_raceID);
    } while (raceRec->m_requiredExpansion > ClientServices::Connection()->m_accountExpansion);
}

// VERIFY: 45c27290 auto
// OFFSET: 0x004DFFC0
void CCharacterCreation::OnHide() {
    EventUnregister(EVENT_ID_IDLE, &CCharacterCreation::Idle);
#if defined(WOWDECOMP_RETAIL_MODELS)
    CharacterCreationSetZoom(0.0f, true);
#endif
    LoadingScreenMiniDisable();
}

// VERIFY: f3353031 auto
// OFFSET: 0x004DFFE0
bool IsRaceClassValid(int32_t raceID, int32_t classID) {
    for (int32_t i = 0; i < g_charBaseInfoDB.m_numRecords; i++) {
        CharBaseInfoRec* rec = i >= 0 && i < g_charBaseInfoDB.m_numRecords ? &g_charBaseInfoDB.m_records[i] : nullptr;

        if (rec && static_cast<uint8_t>(rec->m_raceID) == raceID && static_cast<uint8_t>(rec->m_classID) == classID) {
            return true;
        }
    }

    return false;
}

// VERIFY: 0df0d554 auto
// OFFSET: 0x004E0040
void CCharacterCreation::SetCharCustomizeModel(const char* modelName) {
    if (!CCharacterCreation::m_charCustomizeFrame || !modelName || !*modelName) {
        return;
    }

    if (CCharacterCreation::m_charCustomizeFrame->m_model && !SStrCmpI(modelName, CCharacterCreation::m_charCustomizeFrame->m_model->m_shared->m_fileName, STORM_MAX_STR)) {
        return;
    }

    CCharacterCreation::m_charCustomizeFrame->SetModel(modelName);
    CCharacterCreation::m_charCustomizeFrame->m_lightArray1[0].unk_01B5 = 1;

    CM2Model* model = CCharacterCreation::m_charCustomizeFrame->m_model;

    if (model) {
        CCharacterCreation::m_charCustomizeFrame->m_lightArray1[0].unk_01BC = 1;
        model->m_lightingCallback = &CCharacterSelection::GenericLightingCallback;
        model->m_lightingArg = CCharacterCreation::m_charCustomizeFrame->m_lightArray1;
        model->IsDrawable(1, 1);
    }

    if (CCharacterCreation::m_charCustomizeFrame->m_model && CCharacterCreation::m_character->m_data.m_model) {
        CCharacterCreation::m_character->m_data.m_model->AttachToParent(CCharacterCreation::m_charCustomizeFrame->m_model, 0, nullptr, 0);
    }
}

// VERIFY: 218e9a92 auto
// OFFSET: 0x004E0160
void CCharacterCreation::SetCharacterFacing(float facing) {
    CCharacterCreation::m_charFacing = facing;

    if (CCharacterCreation::m_character->m_data.m_model) {
        C3Vector position(0.0f, 0.0f, 0.0f);
#if defined(WOWDECOMP_RETAIL_MODELS)
        // Not in the original: a Retail body is drawn at its world size (RetailCharacterGlueScale).
        CCharacterCreation::m_character->m_data.m_model->SetWorldTransform(position, facing, RetailCharacterGlueScale(CCharacterCreation::m_character));
#else
        CCharacterCreation::m_character->m_data.m_model->SetWorldTransform(position, facing, 1.0f);
#endif
    }
}

// VERIFY: 308d1147 auto
// OFFSET: 0x004E01B0
bool CCharacterCreation::IsClassAvailable(int32_t classID) {
    for (uint32_t i = 0; i < CCharacterCreation::m_classes.Count(); i++) {
        if (CCharacterCreation::m_classes.m_data[i]->m_id == classID) {
            return true;
        }
    }

    return false;
}

// VERIFY: e48266b8 auto
// OFFSET: 0x004E01F0
void CCharacterCreation::CycleCharCustomization(CHAR_CUSTOMIZATION_TYPE type, int32_t delta) {
    if (!delta) {
        return;
    }

    switch (type) {
        case CHAR_CUSTOMIZATION_SKIN: {
            if (delta > 0) {
                CCharacterCreation::m_character->SetNextSkin(COMPONENT_CONTEXT_0);
            } else {
                CCharacterCreation::m_character->SetPrevSkin(COMPONENT_CONTEXT_0);
            }

            CCharacterCreation::m_prevSkinIndex = CCharacterCreation::m_character->m_data.m_preferences.m_skinID;
            CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
            return;
        }

        case CHAR_CUSTOMIZATION_FACE: {
            if (delta > 0) {
                CCharacterCreation::m_character->SetNextFace(COMPONENT_CONTEXT_0, CCharacterCreation::m_prevSkinIndex);
            } else {
                CCharacterCreation::m_character->SetPrevFace(COMPONENT_CONTEXT_0, CCharacterCreation::m_prevSkinIndex);
            }

            break;
        }

        case CHAR_CUSTOMIZATION_HAIR_STYLE: {
            if (delta > 0) {
                CCharacterCreation::m_character->SetNextHairStyle(COMPONENT_CONTEXT_0);
            } else {
                CCharacterCreation::m_character->SetPrevHairStyle(COMPONENT_CONTEXT_0);
            }

            break;
        }

        case CHAR_CUSTOMIZATION_HAIR_COLOR: {
            if (delta > 0) {
                CCharacterCreation::m_character->SetNextHairColor(COMPONENT_CONTEXT_0);
            } else {
                CCharacterCreation::m_character->SetPrevHairColor(COMPONENT_CONTEXT_0);
            }

            break;
        }

        case CHAR_CUSTOMIZATION_FACIAL_FEATURE: {
            if (delta > 0) {
                CCharacterCreation::m_character->SetNextFacialFeature(COMPONENT_CONTEXT_0);
            } else {
                CCharacterCreation::m_character->SetPrevFacialFeature(COMPONENT_CONTEXT_0);
            }

            break;
        }

        default: {
            break;
        }
    }

    CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
}

// VERIFY: 0b8a8ec0 manual
// OFFSET: 0x004E0380
void CCharacterCreation::CreateCharacter(const char* name) {
    WOWGUID guid = WOWGUID();
    CharacterSelectionDisplay* display = nullptr;
    bool validate = true;

    if (CCharacterCreation::m_existingCharacterIndex >= 0) {
        display = CCharacterSelection::GetCharacterDisplay(CCharacterCreation::m_existingCharacterIndex);

        if (!display) {
            return;
        }

        guid = display->m_characterInfo.m_guid;

        if (name) {
            if (!SStrCmpI(display->m_characterInfo.m_name, name, STORM_MAX_STR)) {
                validate = false;
            }
        } else {
            name = display->m_characterInfo.m_name;
        }
    }

    if (validate) {
        int32_t result = CHAR_NAME_NO_NAME;

        if (!name || (result = ClientServices::CharacterValidateName(name)) != CHAR_NAME_SUCCESS) {
            FrameScript_SignalEvent(GLUE_EVENT_OPEN_STATUS_DIALOG, "%s%s", "OKAY", FrameScript_GetText(ClientServices::GetErrorToken(result), -1, GENDER_NOT_APPLICABLE));
            return;
        }
    }

#if defined(WOWDECOMP_RETAIL_MODELS)
    // Not in the original: FlashWOW Two Forms; the character is made from its own form.
    ShowForm(false);
#endif
    CHARACTER_CREATE_INFO info;
    SStrCopy(info.m_name, name, sizeof(info.m_name));
    info.m_raceID = static_cast<uint8_t>(CCharacterCreation::m_character->m_data.m_preferences.m_raceID);
    info.m_classID = static_cast<uint8_t>(CCharacterCreation::m_selectedClassID);
    info.m_sexID = static_cast<uint8_t>(CCharacterCreation::m_character->m_data.m_preferences.m_sexID);
    info.m_skinID = static_cast<uint8_t>(CCharacterCreation::m_character->m_data.m_preferences.m_skinID);
    info.m_hairColorID = static_cast<uint8_t>(CCharacterCreation::m_character->m_data.m_preferences.m_hairColorId);
    info.m_hairStyleID = static_cast<uint8_t>(CCharacterCreation::m_character->m_data.m_preferences.m_hairStyleId);
    info.m_facialHairStyleID = static_cast<uint8_t>(CCharacterCreation::m_character->m_data.m_preferences.m_facialHairId);
    info.m_faceID = static_cast<uint8_t>(CCharacterCreation::m_character->m_data.m_preferences.m_faceId);
    info.m_outfitID = 0;

    if (guid && display) {
        uint32_t flags = display->m_characterInfo.m_customizeFlags;

        if (flags & 0x100000) {
            CGlueMgr::RaceChange(guid, &info);
        } else if (flags & 0x10000) {
            CGlueMgr::FactionChange(guid, &info);
        } else {
            CGlueMgr::CustomizeCharacter(guid, &info);
        }
    } else {
        CGlueMgr::CreateCharacter(&info);
    }
}

// VERIFY: a51378a3 auto
// OFFSET: 0x004E0500
int32_t CCharacterCreation::GetExistingRaceIndex() {
    CharacterSelectionDisplay* display = CCharacterSelection::GetCharacterDisplay(CCharacterCreation::m_existingCharacterIndex);

    if (!display) {
        return -1;
    }

    for (uint32_t i = 0; i < CCharacterCreation::m_races.Count(); i++) {
        if (CCharacterCreation::m_races.m_data[i] == display->m_characterInfo.m_raceID) {
            return i;
        }
    }

    return -1;
}

// VERIFY: 31f4168d auto
// OFFSET: 0x004E0550
int32_t CCharacterCreation::GetExistingClassIndex() {
    CharacterSelectionDisplay* display = CCharacterSelection::GetCharacterDisplay(CCharacterCreation::m_existingCharacterIndex);

    if (!display) {
        return -1;
    }

    for (int32_t i = 0; i < g_chrClassesDB.m_numRecords; i++) {
        ChrClassesRec* rec = i >= 0 && i < g_chrClassesDB.m_numRecords ? &g_chrClassesDB.m_records[i] : nullptr;

        if (rec->m_id == display->m_characterInfo.m_classID) {
            return i;
        }
    }

    return -1;
}

// VERIFY: 6c4329fd manual
// OFFSET: 0x004E0F50
int32_t CCharacterCreation::GetRandomClassID() {
    // SXC: as many slots as there are classes (stock: 10, which more classes overflow on the stack;
    // tswow's "unlimited race/class pairs" patch enlarges the frame instead).
    std::vector<int32_t> classes(CCharacterCreation::m_classes.Count() ? CCharacterCreation::m_classes.Count() : 1, 0);
    int32_t count = 0;
    uint8_t expansion = ClientServices::Connection()->m_accountExpansion;

    for (uint32_t i = 0; i < CCharacterCreation::m_classes.Count(); i++) {
        ChrClassesRec* rec = CCharacterCreation::m_classes.m_data[i];

        if (expansion >= rec->m_requiredExpansion) {
            classes[count++] = rec->m_id;
        }
    }

    if (count <= 0) {
        return classes[0];
    }

    return classes[static_cast<uint32_t>((static_cast<uint64_t>(count) * CRandom::uint32_(g_rndSeed)) >> 32)];
}

// VERIFY: b15e5451 auto
// OFFSET: 0x004E0FD0
void CCharacterCreation::Dress() {
#if defined(WOWDECOMP_RETAIL_MODELS)
    // Not in the original: FlashWOW "show clothing" off; everything comes off, weapons included.
    if (s_clothingHidden) {
        for (uint32_t i = 0; i < NUM_ITEM_SLOTS; i++) {
            CCharacterCreation::m_character->RemoveItem(static_cast<ITEM_SLOT>(i));
        }
        CCharacterComponent::RemoveHandItem(CCharacterCreation::m_character->m_data.m_model, INVSLOT_MAINHAND, SHEATHE_NONE, false);
        CCharacterComponent::RemoveHandItem(CCharacterCreation::m_character->m_data.m_model, INVSLOT_OFFHAND, SHEATHE_NONE, false);
        CCharacterComponent::RemoveHandItem(CCharacterCreation::m_character->m_data.m_model, INVSLOT_OFFHAND, SHEATHE_NONE, true);
        CCharacterComponent::RemoveHandItem(CCharacterCreation::m_character->m_data.m_model, INVSLOT_RANGED, SHEATHE_NONE, false);
        return;
    }
#endif
    if (CCharacterCreation::m_existingCharacterIndex >= 0) {
        CharacterSelectionDisplay* display = CCharacterSelection::GetCharacterDisplay(CCharacterCreation::m_existingCharacterIndex);

        if (display) {
            for (uint32_t slot = 0; slot < 23; slot++) {
                int32_t displayID = display->m_characterInfo.m_inventoryItemDisplayID[slot];
                int32_t itemVisual = display->m_characterInfo.m_inventoryItemVisualID[slot];

                if (!displayID) {
                    continue;
                }

                if (display->m_characterInfo.m_classID == 3) {
                    if (slot == INVSLOT_MAINHAND || slot == INVSLOT_OFFHAND) {
                        continue;
                    }
                } else if (slot == INVSLOT_RANGED) {
                    continue;
                }

                if (slot == INVSLOT_HEAD || ((display->m_characterInfo.m_flags & 0x800) && slot == INVSLOT_BACK)) {
                    continue;
                }

                ItemDisplayInfoRec itemRec;
                int32_t found = g_itemDisplayInfoDB.GetRecord(displayID, &itemRec);
                int32_t visual = found && g_itemVisualsDB.GetRecord(itemRec.m_itemVisual) ? 0 : itemVisual;

                if (slot != INVSLOT_MAINHAND && slot != INVSLOT_OFFHAND && slot != INVSLOT_RANGED) {
                    CCharacterCreation::m_character->AddItemBySlot(static_cast<INVENTORY_SLOTS>(slot), displayID, visual);
                } else if (found) {
                    CCharacterComponent::AddHandItem(CCharacterCreation::m_character->m_data.m_model, &itemRec, static_cast<INVENTORY_SLOTS>(slot), SHEATHE_NONE, false, display->m_characterInfo.m_inventoryItemType[slot] == INVTYPE_SHIELD, false, visual);
                }

                if (slot == INVSLOT_TABARD && found && (itemRec.m_flags & 0x1)) {
                    int32_t emblemStyle;
                    int32_t emblemColor;
                    int32_t borderStyle;
                    int32_t borderColor;
                    int32_t background;

                    if (GuildGetGuildTabard(display->m_characterInfo.m_guid, display->m_characterInfo.m_guildID, nullptr, nullptr, emblemStyle, emblemColor, borderStyle, borderColor, background)) {
                        CCharacterCreation::m_character->ApplyGuildColor(emblemStyle, emblemColor, borderStyle, borderColor, background);
                    }
                }
            }

            return;
        }
    }

    for (uint32_t i = 0; i < NUM_ITEM_SLOTS; i++) {
        CCharacterCreation::m_character->RemoveItem(static_cast<ITEM_SLOT>(i));
    }

    CCharacterComponent::RemoveHandItem(CCharacterCreation::m_character->m_data.m_model, INVSLOT_MAINHAND, SHEATHE_NONE, false);
    CCharacterComponent::RemoveHandItem(CCharacterCreation::m_character->m_data.m_model, INVSLOT_OFFHAND, SHEATHE_NONE, false);
    CCharacterComponent::RemoveHandItem(CCharacterCreation::m_character->m_data.m_model, INVSLOT_OFFHAND, SHEATHE_NONE, true);
    CCharacterComponent::RemoveHandItem(CCharacterCreation::m_character->m_data.m_model, INVSLOT_RANGED, SHEATHE_NONE, false);

    for (int32_t i = 0; i < g_charStartOutfitDB.m_numRecords; i++) {
        CharStartOutfitRec* outfit = i >= 0 && i < g_charStartOutfitDB.m_numRecords ? &g_charStartOutfitDB.m_records[i] : nullptr;

#if defined(WOWDECOMP_RETAIL_MODELS)
        // Not in the original: FlashWOW Two Forms; the altered form wears its own race's starting outfit.
        const uint32_t outfitRace = SelectedBaseRace();
#else
        const uint32_t outfitRace = CCharacterCreation::m_character->m_data.m_preferences.m_raceID;
#endif
        if (static_cast<uint8_t>(outfit->m_raceID) != outfitRace || static_cast<uint8_t>(outfit->m_classID) != CCharacterCreation::m_selectedClassID || static_cast<uint8_t>(outfit->m_sexID) != CCharacterCreation::m_character->m_data.m_preferences.m_sexID) {
            continue;
        }

        for (int32_t j = 0; j < 24; j++) {
            int32_t displayID = outfit->m_displayItemID[j];
            int32_t type = outfit->m_inventoryType[j];

            if (displayID <= 0 || type == INVTYPE_HEAD) {
                continue;
            }

            if (CCharacterCreation::m_selectedClassID == 3) {
                switch (type) {
                    case INVTYPE_WEAPON:
                    case INVTYPE_2HWEAPON:
                    case INVTYPE_WEAPONMAINHAND:
                    case INVTYPE_WEAPONOFFHAND: {
                        continue;
                    }

                    case INVTYPE_RANGED:
                    case INVTYPE_RANGEDRIGHT: {
                        bool mainHand = type == INVTYPE_RANGEDRIGHT || type == INVTYPE_THROWN;
                        ItemDisplayInfoRec itemRec;

                        if (g_itemDisplayInfoDB.GetRecord(displayID, &itemRec)) {
                            GEOCOMPONENTLINKS link = CCharacterComponent::AddHandItem(CCharacterCreation::m_character->m_data.m_model, &itemRec, INVSLOT_RANGED, SHEATHE_NONE, false, false, mainHand, 0);

                            if (type == INVTYPE_RANGED) {
                                for (CM2Model* attached = CCharacterCreation::m_character->m_data.m_model->m_attachList; attached; attached = attached->m_attachNext) {
                                    if (attached->m_attachmentId == static_cast<uint32_t>(link)) {
                                        attached->SetLoadedCallback(&CCharacterSelection::ModelLoadedCallback, nullptr);
                                        break;
                                    }
                                }
                            }
                        }

                        break;
                    }

                    case INVTYPE_BAG: {
                        ItemDisplayInfoRec itemRec;

                        if (g_itemDisplayInfoDB.GetRecord(displayID, &itemRec) && *itemRec.m_modelName[0]) {
                            CCharacterCreation::m_character->AddItem(ITEMSLOT_11, &itemRec, 0);
                        }

                        break;
                    }

                    default: {
                        break;
                    }
                }
            }

            CCharacterCreation::m_character->AddItemByType(static_cast<INVENTORY_TYPES>(type), displayID);
        }

        return;
    }
}

// VERIFY: 6d456576 auto
// OFFSET: 0x004E13A0
void CCharacterCreation::InitCharacterComponents(ComponentData* data, bool randomize) {
    CreatureModelDataRec* modelRec = GetModelDataRec(data->m_preferences.m_raceID, data->m_preferences.m_sexID);

    if (!modelRec || !*modelRec->m_modelName) {
        return;
    }

#if defined(WOWDECOMP_RETAIL_MODELS)
    // Not in the original: the Retail rows the player set survive the rebuild (the create button
    // re-selects the class, which lands here, just before the character is sent).
    RetailCustomizationExtras extras;
    RetailCustomizationTakeExtras(CCharacterCreation::m_character, extras);
#endif
    if (CCharacterCreation::m_character) {
        if (CCharacterCreation::m_character->m_data.m_model->m_attachParent) {
            CCharacterCreation::m_character->m_data.m_model->DetachFromParent();
        }

        CCharacterComponent::FreeComponent(CCharacterCreation::m_character);
    }

    CCharacterCreation::m_character = CCharacterComponent::AllocComponent();
#if defined(WOWDECOMP_RETAIL_MODELS)
    if (!randomize) {
        RetailCustomizationGiveExtras(CCharacterCreation::m_character, extras);
    }
#endif
    data->m_model = CCharacterCreation::m_charCustomizeFrame->GetScene()->CreateModel(modelRec->m_modelName, 0);

    if (!data->m_model) {
        return;
    }

    CCharacterCreation::m_charCustomizeFrame->m_lightArray2[0].unk_01BC = 1;
    data->m_model->m_lightingCallback = &CCharacterSelection::GenericLightingCallback;
    data->m_model->m_lightingArg = CCharacterCreation::m_charCustomizeFrame->m_lightArray2;
    data->m_model->SetBoneSequence(0xFFFFFFFF, 0, 0, 0, 1.0f, 1, 1);
    data->m_flags |= 0x2;

    CCharacterCreation::m_character->Init(data, nullptr);

    if (randomize) {
        CCharacterCreation::m_character->SetRandomSkin(COMPONENT_CONTEXT_0);
        CCharacterCreation::m_character->SetRandomHairColor(COMPONENT_CONTEXT_0);
        CCharacterCreation::m_character->SetRandomHairStyle(COMPONENT_CONTEXT_0);
        CCharacterCreation::m_character->SetRandomFace(COMPONENT_CONTEXT_0);
        CCharacterCreation::m_character->SetRandomFacialFeature(COMPONENT_CONTEXT_0);
    }

    CCharacterCreation::m_prevSkinIndex = CCharacterCreation::m_character->m_data.m_preferences.m_skinID;
    CCharacterCreation::m_prevFaceIndex = CCharacterCreation::m_character->m_data.m_preferences.m_faceId;
    CCharacterCreation::m_prevHairColorIndex = CCharacterCreation::m_character->m_data.m_preferences.m_hairColorId;
    CCharacterCreation::m_prevHairStyleIndex = CCharacterCreation::m_character->m_data.m_preferences.m_hairStyleId;
    CCharacterCreation::m_prevFacialHairIndex = CCharacterCreation::m_character->m_data.m_preferences.m_facialHairId;

    CCharacterCreation::SetCharacterFacing(CCharacterCreation::m_charFacing);
    CCharacterCreation::Dress();
    CCharacterCreation::m_character->RenderPrep(0);

    if (CCharacterCreation::m_charCustomizeFrame->m_model) {
        CCharacterCreation::m_character->m_data.m_model->AttachToParent(CCharacterCreation::m_charCustomizeFrame->m_model, 0, nullptr, 0);
    }
}

// VERIFY: 68a365e1 manual
// OFFSET: 0x004E1540
void CCharacterCreation::SetSelectedSex(uint32_t sexID) {
    if (sexID >= 3 || sexID == CCharacterCreation::m_character->m_data.m_preferences.m_sexID) {
        return;
    }

#if defined(WOWDECOMP_RETAIL_MODELS)
    ShowForm(false);    // Not in the original: FlashWOW Two Forms; sexes are switched in the race's own form
#endif

    CharacterPreferences* preferences = CCharacterCreation::m_charPreferences[CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex]][CCharacterCreation::m_character->m_data.m_preferences.m_sexID];

    if (!preferences) {
        preferences = static_cast<CharacterPreferences*>(SMemAlloc(sizeof(CharacterPreferences), __FILE__, __LINE__, 0));
        CCharacterCreation::m_charPreferences[CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex]][CCharacterCreation::m_character->m_data.m_preferences.m_sexID] = preferences;
    }

    CCharacterCreation::m_character->GetPreferences(preferences);

    ComponentData data;
    int32_t classID = CCharacterCreation::m_selectedClassID;
    data.m_preferences.m_raceID = CCharacterCreation::m_character->m_data.m_preferences.m_raceID;
    data.m_preferences.m_sexID = sexID;
    data.m_preferences.m_classId = classID;

    if (CCharacterCreation::m_existingCharacterIndex >= 0) {
        CharacterSelectionDisplay* display = CCharacterSelection::GetCharacterDisplay(CCharacterCreation::m_existingCharacterIndex);

        if (display && display->m_characterInfo.m_sexID == sexID && (display->m_characterInfo.m_customizeFlags & 0x1)) {
            data.m_preferences.m_raceID = display->m_characterInfo.m_raceID;
            data.m_preferences.m_sexID = display->m_characterInfo.m_sexID;
            data.m_preferences.m_classId = display->m_characterInfo.m_classID;
            data.m_preferences.m_skinID = display->m_characterInfo.m_skinColorID;
            data.m_preferences.m_hairStyleId = display->m_characterInfo.m_hairStyleID;
            data.m_preferences.m_hairColorId = display->m_characterInfo.m_hairColorID;
            data.m_preferences.m_facialHairId = display->m_characterInfo.m_facialHairStyleID;
            data.m_preferences.m_faceId = display->m_characterInfo.m_faceID;
            CCharacterCreation::InitCharacterComponents(&data, false);

            CCharacterCreation::m_nameGen.LoadNames(CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex], sexID);
            CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
            return;
        }

        classID = CCharacterCreation::m_selectedClassID;
    }

    CharacterPreferences* saved = CCharacterCreation::m_charPreferences[CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex]][sexID];

    if (saved) {
        data.SetPreferences(saved);
        data.m_preferences.m_classId = classID;
        CCharacterComponent::ValidateComponentData(&data, COMPONENT_CONTEXT_0);
        CCharacterCreation::InitCharacterComponents(&data, false);
    } else {
        data.m_preferences.m_raceID = CCharacterCreation::m_character->m_data.m_preferences.m_raceID;
        data.m_preferences.m_sexID = sexID;
        data.m_preferences.m_classId = classID;
        CCharacterCreation::InitCharacterComponents(&data, true);
    }

    CCharacterCreation::m_nameGen.LoadNames(CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex], sexID);
    CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
}

// VERIFY: 60798dcb auto
// OFFSET: 0x004E1740
void CCharacterCreation::SetSelectedClass(int32_t classID) {
    if (!CCharacterCreation::IsClassAvailable(classID)) {
        return;
    }

    CCharacterCreation::m_selectedClassID = classID;

    ComponentData data;
    data.m_preferences.m_raceID = CCharacterCreation::m_character->m_data.m_preferences.m_raceID;
    data.m_preferences.m_sexID = CCharacterCreation::m_character->m_data.m_preferences.m_sexID;
    data.m_preferences.m_classId = classID;
    data.m_preferences.m_skinID = CCharacterCreation::m_character->m_data.m_preferences.m_skinID;
    data.m_preferences.m_hairStyleId = CCharacterCreation::m_character->m_data.m_preferences.m_hairStyleId;
    data.m_preferences.m_hairColorId = CCharacterCreation::m_character->m_data.m_preferences.m_hairColorId;
    data.m_preferences.m_facialHairId = CCharacterCreation::m_character->m_data.m_preferences.m_facialHairId;
    data.m_preferences.m_faceId = CCharacterCreation::m_character->m_data.m_preferences.m_faceId;
    CCharacterComponent::ValidateComponentData(&data, COMPONENT_CONTEXT_0);
    CCharacterCreation::InitCharacterComponents(&data, false);
    CCharacterCreation::Dress();
    CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
}

// VERIFY: ca66e480 auto
// OFFSET: 0x004E17F0
void CCharacterCreation::RandomizeCharCustomization() {
    CCharacterCreation::m_character->SetRandomSkin(COMPONENT_CONTEXT_0);
    CCharacterCreation::m_character->SetRandomFace(COMPONENT_CONTEXT_0);
    CCharacterCreation::m_character->SetRandomHairColor(COMPONENT_CONTEXT_0);
    CCharacterCreation::m_character->SetRandomHairStyle(COMPONENT_CONTEXT_0);
    CCharacterCreation::m_character->SetRandomFacialFeature(COMPONENT_CONTEXT_0);

    CCharacterCreation::m_prevSkinIndex = CCharacterCreation::m_character->m_data.m_preferences.m_skinID;
    CCharacterCreation::m_prevFaceIndex = CCharacterCreation::m_character->m_data.m_preferences.m_faceId;
    CCharacterCreation::m_prevHairColorIndex = CCharacterCreation::m_character->m_data.m_preferences.m_hairColorId;
    CCharacterCreation::m_prevHairStyleIndex = CCharacterCreation::m_character->m_data.m_preferences.m_hairStyleId;
    CCharacterCreation::m_prevFacialHairIndex = CCharacterCreation::m_character->m_data.m_preferences.m_facialHairId;

    CCharacterCreation::Dress();
    CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
}

// VERIFY: f5f53eff manual
// OFFSET: 0x004E1ED0
void CCharacterCreation::CalcClasses(int32_t raceID) {
    uint32_t count = 0;

    for (int32_t i = 0; static_cast<uint32_t>(i) < static_cast<uint32_t>(g_charBaseInfoDB.m_numRecords); i++) {
        CharBaseInfoRec* rec = i >= 0 && i < g_charBaseInfoDB.m_numRecords ? &g_charBaseInfoDB.m_records[i] : nullptr;

        if (rec && static_cast<uint8_t>(rec->m_raceID) == raceID) {
            count++;
        }
    }

    CCharacterCreation::m_classes.SetCount(count);

    uint32_t index = 0;

    for (int32_t i = 0; i < g_charBaseInfoDB.m_numRecords; i++) {
        CharBaseInfoRec* rec = i >= 0 && i < g_charBaseInfoDB.m_numRecords ? &g_charBaseInfoDB.m_records[i] : nullptr;

        if (rec && static_cast<uint8_t>(rec->m_raceID) == raceID) {
            CCharacterCreation::m_classes.m_data[index++] = g_chrClassesDB.GetRecord(static_cast<uint8_t>(rec->m_classID));
        }
    }
}

// VERIFY: 32675449 auto
// OFFSET: 0x004E1FD0
void CCharacterCreation::ResetCharCustomizeInfo() {
    if (!CCharacterCreation::m_charCustomizeFrame) {
        return;
    }

    CCharacterCreation::m_existingCharacterIndex = -1;
#if defined(WOWDECOMP_RETAIL_MODELS)
    // Not in the original: FlashWOW Two Forms; a new character starts in its own form with no other look.
    s_viewingAlteredForm = false;
    s_parkedForms.clear();
#endif

    if (CCharacterCreation::m_charCustomizeFrame->m_model) {
        CCharacterCreation::m_charCustomizeFrame->m_model->DetachAllChildrenById(0);
    }

    ComponentData data;
    CCharacterCreation::GetRandomRaceAndSex(&data);
    CCharacterCreation::CalcClasses(data.m_preferences.m_raceID);
    CCharacterCreation::InitCharacterComponents(&data, true);
    CCharacterCreation::SetSelectedClass(CCharacterCreation::GetRandomClassID());
    data.m_preferences.m_classId = CCharacterCreation::m_selectedClassID;

    CCharacterCreation::m_raceIndex = -1;

    for (uint32_t i = 0; i < CCharacterCreation::m_races.Count(); i++) {
        if (CCharacterCreation::m_races.m_data[i] == static_cast<int32_t>(CCharacterCreation::m_character->m_data.m_preferences.m_raceID)) {
            CCharacterCreation::m_raceIndex = i;
            break;
        }
    }

    CCharacterCreation::m_nameGen.LoadNames(CCharacterCreation::m_character->m_data.m_preferences.m_raceID, CCharacterCreation::m_character->m_data.m_preferences.m_sexID);
    CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
}

// VERIFY: 06f4239a manual
// OFFSET: 0x004E20B0
void CCharacterCreation::SetSelectedRace(uint32_t raceIndex) {
    if (raceIndex >= CCharacterCreation::m_races.Count() || raceIndex == CCharacterCreation::m_raceIndex) {
        return;
    }

#if defined(WOWDECOMP_RETAIL_MODELS)
    ShowForm(false);    // Not in the original: FlashWOW Two Forms; the race's own look is the one saved for it
#endif

    CharacterPreferences* preferences = CCharacterCreation::m_charPreferences[CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex]][CCharacterCreation::m_character->m_data.m_preferences.m_sexID];

    if (!preferences) {
        preferences = static_cast<CharacterPreferences*>(SMemAlloc(sizeof(CharacterPreferences), __FILE__, __LINE__, 0));
        CCharacterCreation::m_charPreferences[CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex]][CCharacterCreation::m_character->m_data.m_preferences.m_sexID] = preferences;
    }

    CCharacterCreation::m_character->GetPreferences(preferences);
    CCharacterCreation::m_raceIndex = raceIndex;

    ComponentData data;
    CharacterSelectionDisplay* display = nullptr;

    if (CCharacterCreation::m_existingCharacterIndex >= 0) {
        display = CCharacterSelection::GetCharacterDisplay(CCharacterCreation::m_existingCharacterIndex);
    }

    if (display && display->m_characterInfo.m_raceID == CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex] && (display->m_characterInfo.m_customizeFlags & 0x110000)) {
        data.m_preferences.m_raceID = display->m_characterInfo.m_raceID;
        data.m_preferences.m_sexID = display->m_characterInfo.m_sexID;
        data.m_preferences.m_classId = display->m_characterInfo.m_classID;
        data.m_preferences.m_skinID = display->m_characterInfo.m_skinColorID;
        data.m_preferences.m_hairStyleId = display->m_characterInfo.m_hairStyleID;
        data.m_preferences.m_hairColorId = display->m_characterInfo.m_hairColorID;
        data.m_preferences.m_facialHairId = display->m_characterInfo.m_facialHairStyleID;
        data.m_preferences.m_faceId = display->m_characterInfo.m_faceID;
        CCharacterCreation::InitCharacterComponents(&data, false);
        CCharacterCreation::SetSelectedSex(display->m_characterInfo.m_sexID);
    } else {
        CharacterPreferences* saved = CCharacterCreation::m_charPreferences[CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex]][CCharacterCreation::m_character->m_data.m_preferences.m_sexID];

        if (saved) {
            data.SetPreferences(saved);
            CCharacterCreation::CalcClasses(data.m_preferences.m_raceID);

            int32_t classID = CCharacterCreation::m_selectedClassID;

            if (!IsRaceClassValid(data.m_preferences.m_raceID, CCharacterCreation::m_selectedClassID)) {
                classID = CCharacterCreation::GetRandomClassID();
                CCharacterCreation::m_selectedClassID = classID;
            }

            data.m_preferences.m_classId = classID;
            CCharacterComponent::ValidateComponentData(&data, COMPONENT_CONTEXT_0);
            CCharacterCreation::InitCharacterComponents(&data, false);
        } else {
            data.m_preferences.m_sexID = CCharacterCreation::m_character->m_data.m_preferences.m_sexID;
            data.m_preferences.m_raceID = CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex];
            CCharacterCreation::CalcClasses(data.m_preferences.m_raceID);

            if (!IsRaceClassValid(data.m_preferences.m_raceID, CCharacterCreation::m_selectedClassID)) {
                CCharacterCreation::SetSelectedClass(CCharacterCreation::GetRandomClassID());
            }

            data.m_preferences.m_classId = CCharacterCreation::m_selectedClassID;
            CCharacterCreation::InitCharacterComponents(&data, true);
        }
    }

    CCharacterCreation::m_nameGen.LoadNames(CCharacterCreation::m_races.m_data[CCharacterCreation::m_raceIndex], CCharacterCreation::m_character->m_data.m_preferences.m_sexID);
    CCharacterCreation::Dress();
    CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
}

// VERIFY: abfffae0 auto
// OFFSET: 0x004E2330
void CCharacterCreation::SetToExistingCharacter(uint32_t index) {
    CharacterSelectionDisplay* display = CCharacterSelection::GetCharacterDisplay(index);

    if (!display) {
        return;
    }

#if defined(WOWDECOMP_RETAIL_MODELS)
    ShowForm(false);    // Not in the original: FlashWOW Two Forms; paid changes edit the race's own form
#endif
    CCharacterCreation::m_existingCharacterIndex = index;

    ComponentData data;
    data.m_preferences.m_raceID = display->m_characterInfo.m_raceID;
    data.m_preferences.m_sexID = display->m_characterInfo.m_sexID;
    data.m_preferences.m_classId = display->m_characterInfo.m_classID;
    data.m_preferences.m_skinID = display->m_characterInfo.m_skinColorID;
    data.m_preferences.m_hairStyleId = display->m_characterInfo.m_hairStyleID;
    data.m_preferences.m_hairColorId = display->m_characterInfo.m_hairColorID;
    data.m_preferences.m_facialHairId = display->m_characterInfo.m_facialHairStyleID;
    data.m_preferences.m_faceId = display->m_characterInfo.m_faceID;
    CCharacterCreation::InitCharacterComponents(&data, false);
    CCharacterCreation::CalcClasses(data.m_preferences.m_raceID);
    CCharacterCreation::InitCharacterComponents(&data, false);

    CCharacterCreation::m_selectedClassID = display->m_characterInfo.m_classID;

    for (uint32_t i = 0; i < CCharacterCreation::m_races.Count(); i++) {
        if (CCharacterCreation::m_races.m_data[i] == display->m_characterInfo.m_raceID) {
            CCharacterCreation::m_raceIndex = i;
            break;
        }
    }

    CCharacterCreation::m_nameGen.LoadNames(CCharacterCreation::m_character->m_data.m_preferences.m_raceID, CCharacterCreation::m_character->m_data.m_preferences.m_sexID);
    CGlueLoading::StartLoad(CCharacterCreation::m_character, 1);
}
