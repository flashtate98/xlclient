#ifndef WOW_COMMON_COMPONENTCORE_RETAILCHARACTER_H
#define WOW_COMMON_COMPONENTCORE_RETAILCHARACTER_H

class CCharacterComponent;

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

// Releases every composed texture. Called at shutdown before TextureDestroy: textures still held at
// process exit would be freed after the graphics device is gone.
void RetailCharacterShutdown();

#endif
