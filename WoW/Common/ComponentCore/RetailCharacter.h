#ifndef WOW_COMMON_COMPONENTCORE_RETAILCHARACTER_H
#define WOW_COMMON_COMPONENTCORE_RETAILCHARACTER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class CCharacterComponent;
class CM2Model;

// Not in the original: characters whose body is a Retail (MD21) model take their geosets from the
// Retail customization tables instead of the 3.3.5 hair and facial-hair rules.
//
// The client race is mapped to a Retail ChrRaces ID (1-11 are the same in both; others come from
// Data\Retail\races.csv, rows "clientRace;retailRace"), then to its ChrModel for the character's sex.
// The 3.3.5 skin, face, hair style, hair colour and facial hair values pick the Nth choice of the
// matching Retail options; every other option uses its default choice until full Retail choices are
// stored per character. The skin, hair, eye
// and other textures are composed from the choices' materials (Compose.h).
//
// Called from CCharacterComponent::GeosRenderPrep after the stock reset. Returns false, changing
// nothing, for stock models and unmapped races.
bool RetailCharacterGeometry(CCharacterComponent* character);

// Called after the stock pass has shown its one geoset per group (only when RetailCharacterGeometry
// returned true). Group 17 is the 3.3.5 eye-glow slot; on Retail bodies 1701-1705 are eye-area
// effect meshes (masks, glows) that the stock facial-hair and Death Knight rules would switch on.
void RetailCharacterAfterStockGeometry(CCharacterComponent* character);

// Binds the composed non-body textures (hair, eyes, ...) for the character's current values. Called in
// place of the 3.3.5 hair and extra-skin texture replacement, which a hair colour change triggers
// without a geometry pass. Returns false, changing nothing, for stock bodies.
bool RetailCharacterTextures(CCharacterComponent* character);

// Binds the composed Retail skin sheet (texture type 1) with the equipped items' 3.3.5 section
// textures stretched over the matching Retail sections. Called after the stock section pass, before
// its temporary item textures are released. Does nothing for stock bodies.
void RetailCharacterBody(CCharacterComponent* character);

// Places an installed 3.3.5 helmet on a Retail body. Stock helmet meshes are built around the 3.3.5
// body's helmet socket; a Retail skeleton puts socket 11 somewhere else relative to the head, so the
// helmet sinks into the neck. The helmet keeps the offset from the head it had on the 3.3.5 model of
// the race (Retail::HelmetTranslation). Called after the helmet is linked and after each section
// pass; does nothing for stock bodies or Retail helmets.
void RetailCharacterFitHelmet(CCharacterComponent* character);

// Live helmet tuning: replaces the helmets.csv row for the character's race and sex for this session and
// refits its helmet now. Returns the row ("race;sex;scale;x;y;z") to paste into helmets.csv to keep it.
std::string RetailCharacterTuneHelmet(CCharacterComponent* character, float scale, float x, float y, float z);

// The Retail options of a character that the 3.3.5 selectors do not drive (Eyebrows, Ears, Eye Color,
// ...), in OrderIndex order, with the choices its class and other choices allow. False for stock bodies.
struct RetailCustomizationOption {
    std::string name;
    uint32_t index;   // the choice taken, 0-based
    uint32_t count;   // the choices offered
};
bool RetailCustomizationOptions(const CCharacterComponent* character, std::vector<RetailCustomizationOption>& options);

// Steps option `index` (0-based, as listed above) by `delta` choices, wrapping, or with `randomize` picks
// a random choice for every listed option; then refreshes geosets and textures. False for stock bodies.
bool RetailCustomizationCycle(CCharacterComponent* character, uint32_t index, int32_t delta, bool randomize = false);

// Barber shop: whether the rows on a bound component differ from the character's stored choices, and
// dropping those edits (back to the stored look).
bool RetailCustomizationEdited(const CCharacterComponent* character);
void RetailCustomizationDiscard(CCharacterComponent* character);

// The create screen's row choices, carried from a component that is about to be freed to the one that
// replaces it. Choices the new character is not offered fall back to defaults as usual.
using RetailCustomizationExtras = std::vector<std::pair<uint32_t, uint32_t>>;
void RetailCustomizationTakeExtras(const CCharacterComponent* character, RetailCustomizationExtras& extras);
void RetailCustomizationGiveExtras(const CCharacterComponent* character, const RetailCustomizationExtras& extras);

// Conditional bodies (the Orc male Upright posture) replace the component's model during the geometry
// pass. A unit showing the component is found from the character it is bound to, and glue screens read
// the component's model each time; any other holder of the model (a model frame) registers here to be
// handed the new body. A null `swap` unregisters.
// The size a Retail body is drawn at on the glue screens (create, select): the world's size for the race,
// CreatureDisplayInfo scale x CreatureModelData scale of its native display. The stock screens draw at 1.0,
// which suited the 3.3.5 bodies; some Retail bodies are modelled larger (Tauren male 1.35x) and are sized
// down through those scales, so the screens follow them. 1.0 for stock bodies.
float RetailCharacterGlueScale(const CCharacterComponent* character);

using RetailBodyOwner = void (*)(void* owner, CM2Model* replacement);
void RetailCharacterSetBodyOwner(const CCharacterComponent* character, RetailBodyOwner swap, void* owner);

// The stock client race (1-11) whose helmets a race wears: itself, or for a new race the stock race whose
// ChrRaces client prefix it shares (Nightborne "Ni": Night Elf). 0 if none.
uint32_t RetailCharacterStockRace(uint32_t raceId);

// The race whose HelmetGeosetVisData bit a character uses: for a Retail body of a new race the stock race above,
// otherwise its own race.
uint32_t RetailCharacterHelmetRace(const CCharacterComponent* character);

// How many values the facial hair selector cycles through on a Retail body (the choices of the option it
// drives), 0 for stock bodies. Used when the race has no CharacterFacialHairStyles rows.
uint32_t RetailCharacterFacialCount(const CCharacterComponent* character);

// Drops the stored choices of a component that is going away.
void RetailCustomizationForget(const CCharacterComponent* character);

// Saving. A character's choices of the options above live on the server (mod-retail-customization)
// as (ChrCustomizationOption ID, ChrCustomizationChoice ID) pairs; RetailCustomizationNet.cpp carries
// them. A component bound to a character takes these stored choices wherever the create screen has
// not set any.
using RetailChoiceList = std::vector<std::pair<uint32_t, uint32_t>>;

// The choices the component shows for the options listed above, for a new character. False for
// stock bodies.
bool RetailCustomizationChoices(const CCharacterComponent* character, RetailChoiceList& choices);

// Binds a component to the character (GUID) it shows. True when that character's choices are not
// known yet and have not been asked for, so the caller should ask the server.
bool RetailCustomizationBind(const CCharacterComponent* character, uint64_t guid);

// A character's choices as the server stores them (empty: none stored). Bound components showing
// that character are refreshed.
void RetailCustomizationStore(uint64_t guid, const RetailChoiceList& choices);

// Releases every composed texture. Called at shutdown before TextureDestroy: textures still held at
// process exit would be freed after the graphics device is gone.
void RetailCharacterShutdown();

#endif
