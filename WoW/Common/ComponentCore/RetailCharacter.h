#ifndef WOW_COMMON_COMPONENTCORE_RETAILCHARACTER_H
#define WOW_COMMON_COMPONENTCORE_RETAILCHARACTER_H

class CCharacterComponent;

// Not in the original: characters whose body is a Retail (MD21) model take their geosets from the
// Retail customization tables instead of the 3.3.5 hair and facial-hair rules.
//
// The client race is mapped to a Retail ChrRaces ID (1-11 are the same in both; others come from
// Data\Retail\races.csv, rows "clientRace;retailRace"), then to its ChrModel for the character's sex.
// Until choices are stored per character, every option uses its default choice.
//
// Called from CCharacterComponent::GeosRenderPrep after the stock reset. Returns false, changing
// nothing, for stock models and unmapped races.
bool RetailCharacterGeometry(CCharacterComponent* character);

// Called after the stock pass has shown its one geoset per group (only when RetailCharacterGeometry
// returned true). Group 17 is the 3.3.5 eye-glow slot; on Retail bodies 1701-1705 are eye-area
// effect meshes (masks, glows) that the stock facial-hair and Death Knight rules would switch on.
void RetailCharacterAfterStockGeometry(CCharacterComponent* character);

#endif
