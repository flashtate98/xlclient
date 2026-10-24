#if defined(WOWDECOMP_RETAIL_MODELS)

#include "WoW/Common/ComponentCore/RetailCharacter.h"
#include "WoW/Common/ComponentCore/CharacterComponent.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Retail/Appearance.h"
#include "Engine/Source/Retail/Compose.h"
#include "Engine/Source/Retail/LocalSource.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Common/TextureCache.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/ItemDisplayInfoRec.h"
#include <deque>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr const char* kLog = "Logs/RetailCharacter.log";

std::filesystem::path RaceMapPath() {
#ifdef _WIN32
    char exe[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, exe, sizeof(exe));
    if (length && length < sizeof(exe)) {
        const auto beside = std::filesystem::path(exe).parent_path() / "Data" / "Retail" / "races.csv";
        std::error_code code;
        if (std::filesystem::exists(beside, code)) return beside;
    }
#endif
    return std::filesystem::path("Data") / "Retail" / "races.csv";
}

bool Number(const std::string& text, uint32_t& value) {
    size_t begin = text.find_first_not_of(" \t"), end = text.find_last_not_of(" \t\r");
    if (begin == std::string::npos) return false;
    const auto parsed = std::from_chars(text.data() + begin, text.data() + end + 1, value);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + end + 1;
}

// Client ChrRaces ID -> Retail ChrRaces ID.
const std::map<uint32_t, uint32_t>& RaceMap() {
    static std::map<uint32_t, uint32_t> map;
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t race = 1; race <= 11; ++race) map[race] = race;
        const auto path = RaceMapPath();
        std::ifstream input(path);
        if (!input) return;
        std::string line;
        size_t rows = 0;
        while (std::getline(input, line)) {
            const auto separator = line.find_first_of(";,");
            uint32_t client = 0, retail = 0;
            if (separator == std::string::npos || !Number(line.substr(0, separator), client) ||
                !Number(line.substr(separator + 1), retail)) continue;
            if (retail) map[client] = retail; else map.erase(client);
            ++rows;
        }
        Blizzard::Log::Printf(1, kLog, "%s: %u race mappings\n", path.string().c_str(), static_cast<unsigned>(rows));
    });
    return map;
}

struct Look {
    uint32_t chrModel = 0;
    std::string key;                         // names its textures: race, sex and choices
    std::vector<uint16_t> geosets;           // chosen, then the anatomy defaults
    std::vector<Retail::Appearance::TextureSection> sections;
    Retail::Image body;                      // the composed skin sheet (texture type 1)
    std::map<uint32_t, CTexture*> textures;  // every other composed type (hair, eyes, ...)
};

bool FetchBlp(uint32_t fileDataId, std::vector<uint8_t>& bytes, void*) {
    std::string error;
    if (Retail::Source().ReadFileDataId(fileDataId, bytes, error)) return true;
    Blizzard::Log::Printf(1, kLog, "Texture %u: %s\n", static_cast<unsigned>(fileDataId), error.c_str());
    return false;
}

CTexture* MakeTexture(const std::string& name, const Retail::Image& image) {
    std::vector<uint8_t> blp;
    Retail::BuildBlp(image, blp);
    CGxTexFlags flags(GxTex_LinearMipNearest, 0, 0, 0, 0, 0, 1);
    return TextureCreateFromBlpData(name.c_str(), flags, blp.data());
}

// The default look per Retail race and sex, composed once and logged the first time it is built.
Look& LookFor(uint32_t retailRace, uint32_t sex) {
    static std::map<uint64_t, Look> looks;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const uint64_t key = (static_cast<uint64_t>(retailRace) << 32) | sex;
    const auto found = looks.find(key);
    if (found != looks.end()) return found->second;

    Look& look = looks[key];
    look.chrModel = Retail::Appearance::ChrModelFor(retailRace, sex);
    std::vector<uint32_t> choices;
    std::vector<uint16_t> defaults;
    std::vector<Retail::Appearance::TextureLayer> layers;
    std::vector<Retail::ComposedTexture> composed;
    std::string warnings;
    if (look.chrModel) {
        Retail::Appearance::DefaultChoices(look.chrModel, choices);
        Retail::Appearance::Geosets(look.chrModel, choices, look.geosets);
        Retail::Appearance::DefaultGeosets(look.geosets, defaults);
        look.geosets.insert(look.geosets.end(), defaults.begin(), defaults.end());
        Retail::Appearance::Sections(Retail::Appearance::LayoutFor(look.chrModel), look.sections);
        Retail::Appearance::TextureLayers(look.chrModel, choices, layers);
        Retail::ComposeTextures(look.chrModel, layers, FetchBlp, nullptr, composed, warnings);
    }

    look.key = std::to_string(retailRace) + "-" + std::to_string(sex);
    for (const uint32_t id : choices) look.key += "-" + std::to_string(id);
    for (auto& texture : composed) {
        if (texture.textureType == 1) {
            look.body = std::move(texture.image);
            continue;
        }
        CTexture* made = MakeTexture("Retail\\Character\\" + look.key + "\\" + std::to_string(texture.textureType), texture.image);
        if (made) look.textures[texture.textureType] = made;
    }

    std::string text;
    for (const uint32_t id : choices) text += " " + std::to_string(id);
    text += " | geosets";
    for (const uint16_t id : look.geosets) text += " " + std::to_string(id);
    text += " | textures";
    if (look.body.width) text += " 1 (" + std::to_string(look.body.width) + "x" + std::to_string(look.body.height) + ")";
    for (const auto& texture : look.textures) text += " " + std::to_string(texture.first);
    Blizzard::Log::Printf(1, kLog, "Retail race %u sex %u: ChrModel %u, default choices%s\n%s",
        static_cast<unsigned>(retailRace), static_cast<unsigned>(sex), static_cast<unsigned>(look.chrModel), text.c_str(), warnings.c_str());
    return look;
}

// The Retail look of a character with a Retail body, or null for stock bodies and unmapped races.
Look* RetailLook(CCharacterComponent* character) {
    CM2Model* model = character->m_data.m_model;
    if (!model || !model->m_shared || !Retail::Source().Handles(model->m_shared->m_fileName)) return nullptr;
    const auto& map = RaceMap();
    const auto race = map.find(character->m_data.m_preferences.m_raceID);
    if (race == map.end()) return nullptr;
    Look& look = LookFor(race->second, character->m_data.m_preferences.m_sexID);
    return look.chrModel ? &look : nullptr;
}

// One equipped item texture in a body section, in the stock compositor's order (Esteria's
// Equipment.h). Robes skip the leg sections' lower layers, as the stock compositor does.
struct EquipmentLayer {
    uint32_t section, layer, displayId;
    void* texture;
};

std::vector<EquipmentLayer> CollectEquipmentLayers(const CCharacterComponent* character, bool robe) {
    constexpr uint32_t counts[] = {2, 7, 1, 5, 7, 3, 6, 1};
    std::vector<EquipmentLayer> layers;
    for (uint32_t section = 0; section < 8; ++section) {
        const uint32_t first = robe && (section == 5 || section == 6) ? 1 : 0;
        const uint32_t last = robe && section == 5 ? 2 : robe && section == 6 ? 4 : counts[section];
        const auto& data = character->m_section[section];
        for (uint32_t layer = first; layer < last; ++layer)
            if ((data.m_layerMask & (1u << layer)) && (data.m_layerItemDisplayId[layer] || data.m_layerTex[layer]))
                layers.push_back({section, layer, data.m_layerItemDisplayId[layer], data.m_layerTex[layer]});
    }
    return layers;
}

}

bool RetailCharacterGeometry(CCharacterComponent* character) {
    Look* found = RetailLook(character);
    if (!found) return false;
    const Look& look = *found;
    CM2Model* model = character->m_data.m_model;

    // The stock reset only reaches geoset 2000; Retail bodies carry groups above it (feet 20,
    // torso 22, hands 23, customization 24-51), which would otherwise all draw at once.
    model->SetGeometryVisible(2001, 9999, 0);

    for (const uint16_t geoset : look.geosets) {
        model->SetGeometryVisible(geoset, geoset, 1);
        // The stock pass shows one geoset per group below 19 from m_geosets; a Retail choice replaces
        // the 3.3.5 hair and facial-hair pick for its group.
        if (geoset / 100 < 19) character->m_data.m_geosets[geoset / 100] = geoset;
    }

    // Hair, eyes and the other single-layer types replace the 3.3.5 CharSections textures.
    for (const auto& texture : look.textures) model->ReplaceTexture(texture.first, texture.second);
    return true;
}

void RetailCharacterBody(CCharacterComponent* character) {
    Look* found = RetailLook(character);
    if (!found || !found->body.width) return;
    const Look& look = *found;

    // The 3.3.5 item section grid is the left half of the Retail sheet at twice the size, in the same
    // section order, so each equipped item texture is stretched over its section's rectangle.
    const auto layers = CollectEquipmentLayers(character, (character->m_flags & 0x20) != 0);
    std::string name = "Retail\\Character\\" + look.key + "\\body";
    for (const auto& layer : layers) {
        name += "/" + std::to_string(layer.section) + "." + std::to_string(layer.layer) + "." + std::to_string(layer.displayId);
        // Guild tabard layers have display ID zero; their file names tell the emblems and colours apart.
        if (!layer.displayId && layer.texture) name += ":" + std::string(static_cast<const CACHEENTRY*>(layer.texture)->m_fileName);
    }

    static std::map<std::string, CTexture*> bodies;
    static std::deque<std::string> order;
    auto cached = bodies.find(name);
    if (cached == bodies.end()) {
        Retail::Image sheet = look.body;
        for (const auto& layer : layers) {
            void* handle = layer.texture;
            // Only dirty sections reload their item textures; the others were released by the last pass.
            const bool reload = !handle;
            if (reload) {
                ItemDisplayInfoRec display;
                if (g_itemDisplayInfoDB.GetRecord(layer.displayId, &display))
                    handle = character->LoadItemComponentTexture(&display, static_cast<COMPONENT_SECTIONS>(layer.section));
            }
            TCTEXTUREINFO info{};
            TextureCacheGetInfo(handle, info, 1);
            const CACHEENTRY* entry = static_cast<const CACHEENTRY*>(handle);
            std::vector<uint8_t> bytes;
            if (entry && !entry->m_loadFailed && entry->m_data && info.m_width) {
                const auto* begin = reinterpret_cast<const uint8_t*>(entry->m_data);
                bytes.assign(begin, begin + entry->m_size);
            }
            if (reload && handle) TextureCacheDestroyTexture(handle);
            Retail::Image item;
            std::string error;
            if (!Retail::DecodeBlp(bytes, item, error)) {
                Blizzard::Log::Printf(1, kLog, "Item texture for section %u layer %u (display %u): %s\n",
                    static_cast<unsigned>(layer.section), static_cast<unsigned>(layer.layer), static_cast<unsigned>(layer.displayId), error.c_str());
                continue;
            }
            for (const auto& section : look.sections)
                if (section.sectionType == layer.section)
                    Retail::PaintImage(sheet, item, section.x, section.y, section.width, section.height, 2);
        }
        CTexture* texture = MakeTexture(name, sheet);
        if (!texture) return;
        cached = bodies.emplace(name, texture).first;
        order.push_back(name);
        // Each state is a full-size sheet; keep a handful.
        while (order.size() > 8) {
            const auto old = bodies.find(order.front());
            if (old != bodies.end()) {
                HandleClose(reinterpret_cast<HOBJECT>(old->second));
                bodies.erase(old);
            }
            order.pop_front();
        }
    }
    character->m_data.m_model->ReplaceTexture(1, cached->second);
}

void RetailCharacterAfterStockGeometry(CCharacterComponent* character) {
    CM2Model* model = character->m_data.m_model;
    if (model) model->SetGeometryVisible(1701, 1799, 0);
}

#endif
