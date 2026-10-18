#include "WoW/Common/ComponentCore/ShellRaces.h"

#include <cctype>
#include <cstddef>
#include <cstring>

#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Common/ComponentCore/CharacterComponent.h"
#include "WoW/Common/TextureCache.h"
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

// Scratch image the clothing composite is built in. Shared by every component, like the stock
// compositor's own buffer: it is filled on GxTex_Lock and read on GxTex_Latch of the same upload.
MipBits* s_clothingBuffer;

uint32_t TextureSize() {
    return 1u << CCharacterComponent::s_mipLevels;
}

// The equipment layers each body section can hold (CharacterSection::m_layerTex), as the stock
// RenderPrep functions read them. Robes skip the pants layer (0) on both leg sections.
void LayerRange(int32_t section, bool robe, int32_t& first, int32_t& last) {
    static const int32_t s_layerCounts[8] = { 2, 7, 1, 5, 7, 3, 6, 1 };

    first = 0;
    last = s_layerCounts[section];

    if (robe && section == SECTION_LEG_UPPER) {
        first = 1;
        last = 2;
    } else if (robe && section == SECTION_LEG_LOWER) {
        first = 1;
        last = 4;
    }
}

// Straight-alpha "over": the source pixel (b, g, r, a) on top of whatever is already there.
// Unlike the stock paste routines, a transparent source pixel leaves the destination
// transparent instead of turning it opaque black.
void BlendOver(uint8_t* dst, uint32_t b, uint32_t g, uint32_t r, uint32_t a) {
    if (!a) {
        return;
    }

    uint32_t prior = dst[3];
    uint32_t total = a * 255 + prior * (255 - a);   // output alpha, scaled by 255

    dst[0] = static_cast<uint8_t>((b * a * 255 + dst[0] * prior * (255 - a) + total / 2) / total);
    dst[1] = static_cast<uint8_t>((g * a * 255 + dst[1] * prior * (255 - a) + total / 2) / total);
    dst[2] = static_cast<uint8_t>((r * a * 255 + dst[2] * prior * (255 - a) + total / 2) / total);
    dst[3] = static_cast<uint8_t>((total + 127) / 255);
}

// Draws one item layer into its section of the clothing image (mip 0 only), scaled to the
// section with nearest-neighbour sampling, keeping the layer's own alpha (0, 1, 4 or 8 bit).
void PasteLayer(void* texture, int32_t section, uint32_t size) {
    TCTEXTUREINFO info = {};
    info.m_opaque = 1;

    if (!TextureCacheHasMips(texture) || !TextureCacheGetInfo(texture, info, 1)) {
        return;
    }

    const BlpPalPixel* pal = TextureCacheGetPal(texture);
    const uint8_t* src = static_cast<const uint8_t*>(TextureCacheGetMip(texture, 0));
    uint32_t srcWidth = info.m_width;
    uint32_t srcHeight = info.m_height;

    if (!pal || !src || !srcWidth || !srcHeight) {
        return;
    }

    const uint8_t* alpha = src + srcWidth * srcHeight;
    uint32_t alphaSize = info.m_alphaSize;
    uint32_t shift = 9 - CCharacterComponent::s_mipLevels;
    const CompSectionInfo& raw = CCharacterComponent::s_sectionInfoRaw[section];
    uint32_t left = raw.m_pos.x >> shift;
    uint32_t top = raw.m_pos.y >> shift;
    uint32_t width = raw.m_size.x >> shift;
    uint32_t height = raw.m_size.y >> shift;
    uint8_t* image = reinterpret_cast<uint8_t*>(s_clothingBuffer->m_mip[0]);

    for (uint32_t y = 0; y < height; y++) {
        uint32_t sy = y * srcHeight / height;

        for (uint32_t x = 0; x < width; x++) {
            uint32_t sx = x * srcWidth / width;
            uint32_t i = sy * srcWidth + sx;
            uint32_t a = 255;

            if (alphaSize == 1) {
                a = ((alpha[i >> 3] >> (i & 7)) & 0x1) ? 255 : 0;
            } else if (alphaSize == 4) {
                a = ((alpha[i >> 1] >> (4 * (i & 1))) & 0xF) * 17;
            } else if (alphaSize == 8) {
                a = alpha[i];
            }

            const BlpPalPixel& color = pal[src[i]];
            BlendOver(image + ((top + y) * size + left + x) * 4, color.m_b, color.m_g, color.m_r, a);
        }
    }
}

// Fills mip levels 1..n from level 0 with a 2x2 box filter, weighting colour by alpha so the
// transparent (black) pixels around an item don't darken its edges at a distance.
void BuildMips(uint32_t size) {
    for (uint32_t level = 1, width = size >> 1; width; level++, width >>= 1) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(s_clothingBuffer->m_mip[level - 1]);
        uint8_t* dst = reinterpret_cast<uint8_t*>(s_clothingBuffer->m_mip[level]);
        uint32_t srcWidth = width * 2;

        for (uint32_t y = 0; y < width; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t sum[3] = { 0, 0, 0 };
                uint32_t alphaSum = 0;

                for (uint32_t dy = 0; dy < 2; dy++) {
                    for (uint32_t dx = 0; dx < 2; dx++) {
                        const uint8_t* p = src + ((y * 2 + dy) * srcWidth + x * 2 + dx) * 4;
                        sum[0] += p[0] * p[3];
                        sum[1] += p[1] * p[3];
                        sum[2] += p[2] * p[3];
                        alphaSum += p[3];
                    }
                }

                uint8_t* out = dst + (y * width + x) * 4;

                for (uint32_t c = 0; c < 3; c++) {
                    out[c] = static_cast<uint8_t>(alphaSum ? (sum[c] + alphaSum / 2) / alphaSum : 0);
                }

                out[3] = static_cast<uint8_t>((alphaSum + 2) / 4);
            }
        }
    }
}

void BuildClothing(CCharacterComponent* component) {
    uint32_t size = TextureSize();

    if (!s_clothingBuffer) {
        s_clothingBuffer = TextureAllocMippedImg(PIXEL_ARGB8888, size, size);
    }

    // Start fully transparent, every mip level.
    for (uint32_t level = 0, width = size; ; level++, width >>= 1) {
        memset(s_clothingBuffer->m_mip[level], 0, width * width * 4);

        if (width == 1) {
            break;
        }
    }

    // Make sure every body section's item textures are loaded, not just the dirty ones the stock
    // compositor needed this time. Remember which ones this call loaded, to free them after.
    void* before[8][7];

    for (int32_t section = 0; section < 8; section++) {
        for (int32_t layer = 0; layer < 7; layer++) {
            before[section][layer] = component->m_section[section].m_layerTex[layer];
        }
    }

    uint32_t dirty = component->m_dirtySections;
    component->m_dirtySections = 0xFF;
    component->ItemsLoaded(1);
    component->m_dirtySections = dirty;

    // Paste the items, in stock order, with real alpha. (The stock paste routines can't be used
    // here: their 4- and 8-bit paths make every pixel of the section opaque, so a transparent
    // part of an item came out solid black.)
    bool robe = (component->m_flags & 0x20) != 0;

    for (int32_t section = 0; section < 8; section++) {
        CharacterSection& data = component->m_section[section];
        int32_t first;
        int32_t last;
        LayerRange(section, robe, first, last);

        for (int32_t layer = first; layer < last; layer++) {
            if (((1 << layer) & data.m_layerMask) && data.m_layerTex[layer]) {
                PasteLayer(data.m_layerTex[layer], section, size);
            }
        }
    }

    BuildMips(size);

    for (int32_t section = 0; section < 8; section++) {
        CharacterSection& data = component->m_section[section];

        for (int32_t layer = 0; layer < 7; layer++) {
            if (!before[section][layer] && data.m_layerTex[layer] && data.m_layerItemDisplayId[layer]) {
                TextureCacheDestroyTexture(data.m_layerTex[layer]);
                data.m_layerTex[layer] = nullptr;
            }
        }
    }
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

void ShellRaces::UpdateClothingTexture(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    CCharacterComponent* component = static_cast<CCharacterComponent*>(userArg);

    switch (command) {
        case GxTex_Lock:
            BuildClothing(component);
            break;

        case GxTex_Latch:
            texelStrideInBytes = 4 * width;
            texels = s_clothingBuffer ? s_clothingBuffer->m_mip[mipLevel] : nullptr;
            break;

        default:
            break;
    }
}

void ShellRaces::RefreshClothing(CCharacterComponent* component) {
    CM2Model* model = component->m_data.m_model;

    if (!model || !ShellRaces::IsShellRace(component->m_data.m_preferences.m_raceID)) {
        return;
    }

    uint32_t size = TextureSize();

    if (!component->m_shellClothingTexture) {
        CGxTexFlags flags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1);

        if (g_theGxDevicePtr->DeviceApi() == GxApi_GLL) {
            flags.m_bit14 = 1;
        }

        component->m_shellClothingTexture = TextureCreate(size, size, GxTex_Argb8888, GxTex_Argb8888, flags, component, &ShellRaces::UpdateClothingTexture, "ShellClothing", 1);

        if (!component->m_shellClothingTexture) {
            return;
        }
    }

    model->ReplaceTexture(14, component->m_shellClothingTexture);
    GxTexUpdate(TextureGetGxTex(component->m_shellClothingTexture, 1, nullptr), 0, 0, size, size, 1);
}

void ShellRaces::DestroyClothing(CCharacterComponent* component) {
    if (!component->m_shellClothingTexture) {
        return;
    }

    GxTexSetCannotUpdate(TextureGetGxTex(component->m_shellClothingTexture, 1, nullptr));
    HandleClose(reinterpret_cast<HOBJECT>(component->m_shellClothingTexture));
    component->m_shellClothingTexture = nullptr;
}
