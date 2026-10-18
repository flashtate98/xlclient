#ifndef WOW_COMMON_COMPONENTCORE_SHELLRACES_H
#define WOW_COMMON_COMPONENTCORE_SHELLRACES_H

#include <cstdint>
#include "Engine/Source/Gx/Types.h"

class CCharacterComponent;
class ItemDisplayInfoRec;

// FlashWOW: races whose model wears armor as "shell" geosets (20001-20028) instead of the stock
// body geosets. Native port of the drakonid-armor WXL extension (v3); see the
// "Drakonid Armor Module - Design Reference" doc for the reasoning behind each rule.
namespace ShellRaces {

// True for races that use shell armor (today: Drakonid, races 20 and 21).
bool IsShellRace(uint32_t raceID);

// Body regions an item display covers, one bit per region in compositor order:
// 0 arm upper, 1 arm lower, 2 hand, 3 torso upper, 4 torso lower, 5 leg upper, 6 leg lower, 7 foot.
// Bit i is set when the display's Texture_(i+1) is non-empty.
uint32_t DisplayCoverage(const ItemDisplayInfoRec& rec);

// True when a helmet should hide the horns (geoset group 32). Every helmet model does, except the
// small, open ones (circlets, goggles, monocles, eyepatches...) listed in ShellRaces.cpp.
bool HelmetHidesHorns(const ItemDisplayInfoRec& rec);

// Shows the customization pieces (groups 31, 32, 35) and the covered armor shells.
// Called at the end of CCharacterComponent::GeosRenderPrep, before OptimizeVisibleGeometry.
void ApplyGeosets(CCharacterComponent* component);

// Clothing texture (model texture slot 14) that the armor shells sample. It holds only the
// equipped items' body textures, with real alpha, on a transparent background, so the scales
// show through wherever an item leaves a gap. Always ARGB8888, even when componentCompress
// makes the body texture DXT1 (DXT1 would drop the alpha).
//
// Refresh is called by the compositor each time it finishes a body texture update; it creates
// the texture on first use, binds it to slot 14 and asks for an upload, which builds it.
void RefreshClothing(CCharacterComponent* component);
void DestroyClothing(CCharacterComponent* component);

// Texture callback: GxTex_Lock builds the composite, GxTex_Latch hands each mip level over.
void UpdateClothingTexture(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels);

} // namespace ShellRaces

#endif
