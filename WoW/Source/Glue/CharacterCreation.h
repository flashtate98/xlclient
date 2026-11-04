#ifndef WOW_SOURCE_GLUE_CHARACTERCREATION_H
#define WOW_SOURCE_GLUE_CHARACTERCREATION_H

#include <cstdint>
#include "Engine/Source/Storm/H/stpl.h"
#include "WoW/Source/Game/NameGen.h"
#include "WoW/Source/Glue/Types.h"

class CCharacterComponent;
class ChrClassesRec;
class ComponentData;
class CSimpleModelFFX;
struct CharacterPreferences;

class CCharacterCreation {
    public:
    static CharacterPreferences* m_charPreferences[128][2]; // FlashWOW: was [22][2] (races 0-21)
    static CCharacterComponent* m_character;
    static CNameGen m_nameGen;
    static CSimpleModelFFX* m_charCustomizeFrame;
    static float m_charFacing;
    static TSGrowableArray<int32_t> m_races;
    static TSFixedArray<ChrClassesRec*> m_classes;
    static int32_t m_existingCharacterIndex;
    static int32_t m_selectedClassID;
    static uint32_t m_raceIndex;
    static uint32_t m_prevSkinIndex;
    static uint32_t m_prevFaceIndex;
    static uint32_t m_prevHairColorIndex;
    static uint32_t m_prevHairStyleIndex;
    static uint32_t m_prevFacialHairIndex;

    static void CalcClasses(int32_t raceID);
    static void CreateCharacter(const char* name);
    static void CycleCharCustomization(CHAR_CUSTOMIZATION_TYPE type, int32_t delta);
    static void Dress();
    static int32_t GetExistingClassIndex();
    static int32_t GetExistingRaceIndex();
    static int32_t GetRandomClassID();
    static void GetRandomRaceAndSex(ComponentData* data);
    static int32_t Idle(const void* data, void* param);
    static void InitCharacterComponents(ComponentData* data, bool randomize);
    static void Initialize();
    static bool IsClassAvailable(int32_t classID);
    static void OnHide();
    static void RandomizeCharCustomization();
    static void ResetCharCustomizeInfo();
    static void SetCharacterFacing(float facing);
    static void SetCharCustomizeModel(const char* modelName);
    static void SetSelectedClass(int32_t classID);
    static void SetSelectedRace(uint32_t raceIndex);
    static void SetSelectedSex(uint32_t sexID);
    static void SetToExistingCharacter(uint32_t index);
    static void Shutdown();
};

bool IsRaceClassValid(int32_t raceID, int32_t classID);
void CharacterCreateRegisterScriptFunctions();
void CharacterCreateUnregisterScriptFunctions();
#if defined(WOWDECOMP_RETAIL_MODELS)
#include <utility>
#include <vector>
// Not in the original: FlashWOW Two Forms on the create screen (CharacterCreation.cpp).
bool CharacterCreationHasAlteredForm();
bool CharacterCreationViewingAlteredForm();
void CharacterCreationSetViewingAlteredForm(bool altered);
// Not in the original: FlashWOW "show clothing" toggle. While clothing is hidden the character on the create
// screen wears nothing (underwear only), so skin markings and tattoos show; Dress() honours it.
bool CharacterCreationClothingHidden();
void CharacterCreationSetClothingHidden(bool hidden);
// Not in the original: FlashWOW create screen zoom. 0 is the scene's own camera, 1 the character's face close
// up; the camera eases there (instant: jumps). Works on any race, from the size of its model.
float CharacterCreationZoom();
void CharacterCreationSetZoom(float zoom, bool instant);
// Per-race tuning of the zoom: the aim moved up and right by these shares of the character's height, and the
// closest the camera may come to it (world units; never under 0.45).
void CharacterCreationSetZoomTuning(float up, float right, float minDistance);
// The altered form's look (skin, face, hair style, hair colour, facial hair) and Retail rows for the character
// about to be created; false when it has none (the player never showed that form).
bool CharacterCreationAlteredFormLook(uint8_t look[5], std::vector<std::pair<uint32_t, uint32_t>>& extras);
// Not in the original: console command "retailCustomize" (lists or steps the Retail customization rows).
int32_t CCommand_RetailCustomize(const char* command, const char* arguments);
#endif

#endif
