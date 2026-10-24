#include "BarberShop.h"
#include <cstdint>
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/FrameScript/FrameScript_Object.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include "Engine/Source/lua-5.1/src/lauxlib.h"
#include "Engine/Source/FrameXML/LoadXML.h"
#include "WoW/Source/DB/Rec/BarberShopStyleRec.h"
#include "WoW/Source/Magic/MagicClient/Spell_C.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/Unit.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#if defined(WOWDECOMP_RETAIL_MODELS)
#include <vector>
#include "WoW/Common/ComponentCore/CharacterComponent.h"
#include "WoW/Common/ComponentCore/RetailCharacter.h"
#include "Engine/Source/Model2/M2Model.h"
#endif

// VERIFY: 350bfeb0 auto
// OFFSET: 0x0052E4F0
static int32_t Script_CanAlterSkin(lua_State* L) {
    lua_pushboolean(L, CGBarberShop::m_currentSkinColor != 0);
    return 1;
}

// VERIFY: edda00c4 manual
// OFFSET: 0x0052E850
static int32_t Script_GetBarberShopStyleInfo(lua_State* L) {
    if (!CGBarberShop::m_barberShopEnabled) {
        return 0;
    }

    if (!lua_isnumber(L, 1)) {
        luaL_error(L, "Usage: GetBarberShopInfo(type)");
        return 0;
    }

    uint32_t index = static_cast<uint32_t>(lua_tonumber(L, 1)) - 1;

    if (index > 3) {
        return 0;
    }

    if (index == 3 && !CGBarberShop::m_currentSkinColor) {
        return 0;
    }

    int32_t cost;

    if (index == 1) {
        lua_pushnil(L);
        lua_pushnil(L);
        float value = GetGameTableValue(0, ClntObjMgrGetActivePlayerObj()->m_unit->m_unitFieldLevel - 1, 0) * 0.5f;
        cost = static_cast<int32_t>(value);
    } else {
        const BarberShopStyleRec* style = CGBarberShop::GetCurrentStyle(index);
        lua_pushstring(L, style->m_displayName);
        lua_pushstring(L, style->m_description);
        float value = GetGameTableValue(0, ClntObjMgrGetActivePlayerObj()->m_unit->m_unitFieldLevel - 1, 0) * style->m_costModifier;
        cost = static_cast<int32_t>(value);
    }

    lua_pushinteger(L, cost);

    if (CGBarberShop::IsCurrentStyle(index)) {
        lua_pushnumber(L, 1.0);
    } else {
        lua_pushnil(L);
    }

    return 4;
}

// VERIFY: 1b6918c6 auto
// OFFSET: 0x0052ED60
static int32_t Script_GetBarberShopTotalCost(lua_State* L) {
    lua_pushinteger(L, CGBarberShop::GetTotalCost());
    return 1;
}

// VERIFY: ed3c2281 auto
// OFFSET: 0x0052ED80
static int32_t Script_CancelBarberShop(lua_State* L) {
    CGBarberShop::DisableBarberShop();
    return 0;
}

// VERIFY: 8d6f7910 auto
// OFFSET: 0x0052EEF0
static int32_t Script_ApplyBarberShopStyle(lua_State* L) {
    CGBarberShop::ApplyAppearanceChange();
    return 0;
}

// VERIFY: 600e509f auto
// OFFSET: 0x0052F900
static int32_t Script_BarberShopReset(lua_State* L) {
    CGBarberShop::Reset();
    return 1;
}

// VERIFY: 969003f7 manual
// OFFSET: 0x0052FDD0
static int32_t Script_SetNextBarberShopStyle(lua_State* L) {
    if (!lua_isnumber(L, 1)) {
        luaL_error(L, "Usage: SetNextBarberShopStyle(type[, backward])");
        return 0;
    }

    uint32_t index = static_cast<uint32_t>(lua_tonumber(L, 1)) - 1;

    if (index <= 3 && (index != 3 || CGBarberShop::m_currentSkinColor)) {
        bool backward = StringToBOOL(L, 2, 0) != 0;
        CGBarberShop::SetNextBarberShopStyle(index, backward);
    }

    return 0;
}

// VERIFY: 270ee40e auto
// OFFSET: 0x0052E9B0
static int32_t Script_GetHairCustomization(lua_State* L) {
    lua_pushstring(L, CGPlayer_C::GetHairCustomizationName());
    return 1;
}

// VERIFY: eceedfeb auto
// OFFSET: 0x0052E9D0
static int32_t Script_GetFacialHairCustomization(lua_State* L) {
    lua_pushstring(L, CGPlayer_C::GetFacialHairCustomizationName());
    return 1;
}

#if defined(WOWDECOMP_RETAIL_MODELS)
// Not in the original: the barber shop's Retail customization rows, the same functions the create
// screen has, acting on the player's own character while the barber shop is open. Rows changed here
// cost nothing; ApplyBarberShopStyle sends them.
static CCharacterComponent* BarberCharacter() {
    if (!CGBarberShop::m_barberShopEnabled) return nullptr;
    CGPlayer_C* player = ClntObjMgrGetActivePlayerObj();
    return player ? player->m_characterComponent : nullptr;
}

// GetRetailCustomizationCount() -> number of rows (0 outside the barber shop and for stock bodies)
static int32_t Script_GetRetailCustomizationCount(lua_State* L) {
    std::vector<RetailCustomizationOption> options;
    if (CCharacterComponent* character = BarberCharacter()) RetailCustomizationOptions(character, options);
    lua_pushnumber(L, static_cast<double>(options.size()));
    return 1;
}

// GetRetailCustomizationInfo(index) -> name, choice (1-based), choiceCount
static int32_t Script_GetRetailCustomizationInfo(lua_State* L) {
    if (!lua_isnumber(L, 1)) {
        luaL_error(L, "Usage: GetRetailCustomizationInfo(index)");
        return 0;
    }
    std::vector<RetailCustomizationOption> options;
    CCharacterComponent* character = BarberCharacter();
    if (character) RetailCustomizationOptions(character, options);
    const int32_t index = static_cast<int32_t>(lua_tonumber(L, 1)) - 1;
    if (index < 0 || index >= static_cast<int32_t>(options.size())) {
        return 0;
    }
    lua_pushstring(L, options[index].name.c_str());
    lua_pushnumber(L, static_cast<double>(options[index].index + 1));
    lua_pushnumber(L, static_cast<double>(options[index].count));
    return 3;
}

// CycleRetailCustomization(index, delta)
static int32_t Script_CycleRetailCustomization(lua_State* L) {
    if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2)) {
        luaL_error(L, "Usage: CycleRetailCustomization(index, delta)");
        return 0;
    }
    const int32_t index = static_cast<int32_t>(lua_tonumber(L, 1)) - 1;
    const int32_t delta = static_cast<int32_t>(lua_tonumber(L, 2));
    CCharacterComponent* character = BarberCharacter();
    if (character && index >= 0 && delta && RetailCustomizationCycle(character, static_cast<uint32_t>(index), delta)) {
        character->m_data.m_model->IsDrawable(1, 1);
        character->RenderPrep(1);
    }
    return 0;
}

// IsRetailCustomizationChanged() -> 1 when the rows differ from the saved look
static int32_t Script_IsRetailCustomizationChanged(lua_State* L) {
    CCharacterComponent* character = BarberCharacter();
    if (character && RetailCustomizationEdited(character)) {
        lua_pushnumber(L, 1.0);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static FrameScript_Method s_retailScriptFunctions[] = {
    { "GetRetailCustomizationCount", &Script_GetRetailCustomizationCount },
    { "GetRetailCustomizationInfo", &Script_GetRetailCustomizationInfo },
    { "CycleRetailCustomization", &Script_CycleRetailCustomization },
    { "IsRetailCustomizationChanged", &Script_IsRetailCustomizationChanged },
};
#endif

// STATIC: 0x00ACC790
static FrameScript_Method s_ScriptFunctions[] = {
    { "GetBarberShopStyleInfo", &Script_GetBarberShopStyleInfo },
    { "SetNextBarberShopStyle", &Script_SetNextBarberShopStyle },
    { "GetBarberShopTotalCost", &Script_GetBarberShopTotalCost },
    { "ApplyBarberShopStyle", &Script_ApplyBarberShopStyle },
    { "CancelBarberShop", &Script_CancelBarberShop },
    { "GetHairCustomization", &Script_GetHairCustomization },
    { "GetFacialHairCustomization", &Script_GetFacialHairCustomization },
    { "BarberShopReset", &Script_BarberShopReset },
    { "CanAlterSkin", &Script_CanAlterSkin },
};

// VERIFY: 5397546f auto
// OFFSET: 0x0052E520
void CGBarberShop::RegisterScriptFunctions() {
    for (uint32_t i = 0; i < sizeof(s_ScriptFunctions) / sizeof(s_ScriptFunctions[0]); i++) {
        FrameScript_RegisterFunction(s_ScriptFunctions[i].m_name, s_ScriptFunctions[i].m_method);
    }
#if defined(WOWDECOMP_RETAIL_MODELS)
    for (const auto& function : s_retailScriptFunctions) {
        FrameScript_RegisterFunction(function.m_name, function.m_method);
    }
#endif
}

// VERIFY: 86a5e76c auto
// OFFSET: 0x0052E550
void CGBarberShop::UnregisterScriptFunctions() {
    for (uint32_t i = 0; i < sizeof(s_ScriptFunctions) / sizeof(s_ScriptFunctions[0]); i++) {
        FrameScript_UnregisterFunction(s_ScriptFunctions[i].m_name);
    }
#if defined(WOWDECOMP_RETAIL_MODELS)
    for (const auto& function : s_retailScriptFunctions) {
        FrameScript_UnregisterFunction(function.m_name);
    }
#endif
}
