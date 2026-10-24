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
#include "Engine/Source/Retail/RetailModel.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Model2/M2Data.h"
#include "WoW/Common/ComponentCore/Types.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Common/TextureCache.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/ItemDisplayInfoRec.h"
#include "WoW/Source/DB/Rec/HelmetGeosetVisDataRec.h"
#include <deque>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <memory>
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

// A client texture made from a composed image, shared by every look that paints the same layers.
struct OwnedTexture {
    CTexture* texture = nullptr;
    explicit OwnedTexture(CTexture* made) : texture(made) {}
    OwnedTexture(const OwnedTexture&) = delete;
    OwnedTexture& operator=(const OwnedTexture&) = delete;
    ~OwnedTexture() { if (texture) HandleClose(reinterpret_cast<HOBJECT>(texture)); }
};

struct Look {
    uint32_t chrModel = 0;
    std::string key;                         // names its textures: race, sex and choices
    std::vector<uint16_t> geosets;           // chosen, then the anatomy defaults
    std::vector<Retail::Appearance::TextureSection> sections;
    std::shared_ptr<const Retail::Image> body;                      // the composed skin sheet (type 1)
    std::map<uint32_t, std::shared_ptr<OwnedTexture>> textures;     // every other type (hair, eyes, ...)
};

bool FetchBlp(uint32_t fileDataId, std::vector<uint8_t>& bytes, void*) {
    std::string error;
    if (Retail::Source().ReadFileDataId(fileDataId, bytes, error)) return true;
    Blizzard::Log::Printf(1, kLog, "Texture %u: %s\n", static_cast<unsigned>(fileDataId), error.c_str());
    return false;
}

using Clock = std::chrono::steady_clock;
double Since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

// Time spent turning composed images into client textures: building the BLP (pixels plus every mip
// level) and creating the texture from it.
struct MakeStats {
    double build = 0, upload = 0;
    uint32_t count = 0;
    uint64_t bytes = 0;
};

CTexture* MakeTexture(const std::string& name, const Retail::Image& image, MakeStats* stats = nullptr) {
    auto start = Clock::now();
    std::vector<uint8_t> blp;
    Retail::BuildBlp(image, blp);
    if (stats) { stats->build += Since(start); stats->bytes += blp.size(); ++stats->count; }
    start = Clock::now();
    CGxTexFlags flags(GxTex_LinearMipNearest, 0, 0, 0, 0, 0, 1);
    CTexture* texture = TextureCreateFromBlpData(name.c_str(), flags, blp.data());
    if (stats) stats->upload += Since(start);
    return texture;
}

void LogBuildType() {
    static bool logged = false;
    if (logged) return;
    logged = true;
#if defined(_DEBUG)
    Blizzard::Log::Printf(1, kLog, "Timing: Debug build (image work runs several times slower than in Release)\n");
#else
    Blizzard::Log::Printf(1, kLog, "Timing: Release build\n");
#endif
}

// The Retail option each 3.3.5 selector drives, by name (the lists follow Esteria's StockOptionName):
// the first name the model has wins. Women's "facial hair" slot drives a female-only option.
enum class Stock { Skin, Face, HairStyle, HairColor, Facial };

const char* const* StockNames(Stock stock, uint32_t sex) {
    static const char* const skin[] = {"Skin Color", "Fur Color", "Primary Color", nullptr};
    static const char* const face[] = {"Face", nullptr};
    static const char* const hairStyle[] = {"Hair Style", "Horn Style", "Hair", nullptr};
    static const char* const hairColor[] = {"Hair Color", "Horn Color", nullptr};
    static const char* const male[] = {"Beard", "Facial Hair", "Beard Style", "Jaw Features", "Tusks", "Chin Decoration", nullptr};
    static const char* const female[] = {"Makeup", "Earrings", "Piercings", "Horns", "Face Features", "Ears", nullptr};
    switch (stock) {
        case Stock::Skin: return skin;
        case Stock::Face: return face;
        case Stock::HairStyle: return hairStyle;
        case Stock::HairColor: return hairColor;
        default: return sex ? female : male;
    }
}

uint32_t StockValue(const CharacterPreferences& prefs, Stock stock) {
    switch (stock) {
        case Stock::Skin: return prefs.m_skinID;
        case Stock::Face: return prefs.m_faceId;
        case Stock::HairStyle: return prefs.m_hairStyleId;
        case Stock::HairColor: return prefs.m_hairColorId;
        default: return prefs.m_facialHairId;
    }
}

// Composed looks and body sheets. They hold textures, so they are released at shutdown before the
// texture system and the graphics device go (RetailCharacterShutdown), not by static destruction.
std::map<std::string, std::shared_ptr<Look>> s_looks;
std::deque<std::string> s_lookOrder;
std::mutex s_lookMutex;
std::map<std::string, CTexture*> s_bodies;
std::deque<std::string> s_bodyOrder;

// Each texture type is composed from its own layers only, so a change that leaves a type's layers
// alone (a new hair colour leaves the skin; Ears changes no texture at all) reuses what was composed.
// Keyed by the model, the type and its layers in paint order.
std::map<std::string, std::shared_ptr<const Retail::Image>> s_composedBodies;
std::deque<std::string> s_composedBodyOrder;
std::map<std::string, std::shared_ptr<OwnedTexture>> s_composedTextures;
std::deque<std::string> s_composedTextureOrder;

template <typename Map>
void Trim(Map& map, std::deque<std::string>& order, size_t keep) {
    while (order.size() > keep) {
        map.erase(order.front());   // a look still showing it keeps its own reference
        order.pop_front();
    }
}

// Decoded equipment textures for the body sheet, by display, section and layer.
std::map<std::string, std::shared_ptr<const Retail::Image>> s_itemImages;
std::deque<std::string> s_itemImageOrder;

// A composed look per Retail race, sex and choices. The most recent ones are kept; each holds a full
// skin sheet, so the cache is bounded.
std::shared_ptr<Look> LookFor(uint32_t retailRace, uint32_t sex, uint32_t chrModel, const std::vector<uint32_t>& choices) {
    auto& looks = s_looks;
    auto& order = s_lookOrder;
    std::lock_guard<std::mutex> lock(s_lookMutex);

    std::string key = std::to_string(retailRace) + "-" + std::to_string(sex);
    for (const uint32_t id : choices) key += "-" + std::to_string(id);
    const auto found = looks.find(key);
    if (found != looks.end()) return found->second;

    LogBuildType();
    const auto lookStart = Clock::now();
    auto look = std::make_shared<Look>();
    look->chrModel = chrModel;
    look->key = key;
    std::vector<uint16_t> defaults;
    std::vector<Retail::Appearance::TextureLayer> layers;
    std::vector<Retail::ComposedTexture> composed;
    std::string warnings;
    Retail::Appearance::Geosets(chrModel, choices, look->geosets);
    Retail::Appearance::DefaultGeosets(look->geosets, defaults);
    look->geosets.insert(look->geosets.end(), defaults.begin(), defaults.end());
    Retail::Appearance::Sections(Retail::Appearance::LayoutFor(chrModel), look->sections);
    Retail::Appearance::TextureLayers(chrModel, choices, layers);
    const double tables = Since(lookStart);
    Retail::ComposeStats compose;
    double composeTotal = 0;
    MakeStats make;
    uint32_t reused = 0;
    std::map<uint32_t, std::vector<Retail::Appearance::TextureLayer>> byType;
    for (const auto& layer : layers) byType[layer.textureType].push_back(layer);
    for (const auto& type : byType) {
        std::string typeKey = std::to_string(chrModel) + "/" + std::to_string(type.first);
        for (const auto& layer : type.second)
            typeKey += ":" + std::to_string(layer.fileDataId) + "." + std::to_string(layer.blendMode) + "." + std::to_string(layer.sectionMask);
        if (type.first == 1) {
            const auto cached = s_composedBodies.find(typeKey);
            if (cached != s_composedBodies.end()) { look->body = cached->second; ++reused; continue; }
        } else {
            const auto cached = s_composedTextures.find(typeKey);
            if (cached != s_composedTextures.end()) { look->textures[type.first] = cached->second; ++reused; continue; }
        }
        auto start = Clock::now();
        composed.clear();
        Retail::ComposeTextures(chrModel, type.second, FetchBlp, nullptr, composed, warnings, &compose);
        composeTotal += Since(start);
        for (auto& texture : composed) {
            if (texture.textureType == 1) {
                look->body = std::make_shared<const Retail::Image>(std::move(texture.image));
                s_composedBodies[typeKey] = look->body;
                s_composedBodyOrder.push_back(typeKey);
                Trim(s_composedBodies, s_composedBodyOrder, 6);
                continue;
            }
            CTexture* made = MakeTexture("Retail\\Character\\" + std::to_string(std::hash<std::string>{}(typeKey)), texture.image, &make);
            if (!made) continue;
            auto owned = std::make_shared<OwnedTexture>(made);
            look->textures[texture.textureType] = owned;
            s_composedTextures[typeKey] = owned;
            s_composedTextureOrder.push_back(typeKey);
            Trim(s_composedTextures, s_composedTextureOrder, 48);
        }
    }
    Blizzard::Log::Printf(1, kLog,
        "Timing look: total %.1f ms | tables %.1f | %u texture types reused | compose %.1f (read %.1f for %u files, decode %.1f, paint %.1f) | "
        "textures %u: build %.1f, create %.1f, %.1f MB\n",
        Since(lookStart), tables, reused, composeTotal, compose.fetch, compose.files, compose.decode, compose.paint,
        make.count, make.build, make.upload, make.bytes / 1048576.0);

    std::string text;
    for (const uint32_t id : choices) text += " " + std::to_string(id);
    text += " | geosets";
    for (const uint16_t id : look->geosets) text += " " + std::to_string(id);
    text += " | textures";
    if (look->body && look->body->width) text += " 1 (" + std::to_string(look->body->width) + "x" + std::to_string(look->body->height) + ")";
    for (const auto& texture : look->textures) text += " " + std::to_string(texture.first);
    Blizzard::Log::Printf(1, kLog, "Retail race %u sex %u: ChrModel %u, choices%s\n%s",
        static_cast<unsigned>(retailRace), static_cast<unsigned>(sex), static_cast<unsigned>(chrModel), text.c_str(), warnings.c_str());

    looks.emplace(key, look);
    order.push_back(key);
    while (order.size() > 24) {
        const auto old = looks.find(order.front());
        if (old != looks.end()) looks.erase(old);   // its textures close with the last reference
        order.pop_front();
    }
    return look;
}

// Choices of the options the 3.3.5 selectors do not drive, per character component, by option ID:
// set by the character creation rows (RetailCustomizationCycle). An option without an entry, or whose
// stored choice is not offered any more, takes its default choice.
std::unordered_map<const CCharacterComponent*, std::map<uint32_t, uint32_t>> s_extraChoices;

// The server's choices per character GUID (RetailCustomizationStore), the character each component
// shows (RetailCustomizationBind), and the GUIDs already asked for.
std::unordered_map<uint64_t, std::map<uint32_t, uint32_t>> s_storedChoices;
std::unordered_map<const CCharacterComponent*, uint64_t> s_boundGuids;
std::set<uint64_t> s_askedGuids;

// The choices a component takes for options the 3.3.5 selectors do not drive: the create screen's,
// else those stored for the character it is bound to; null when there are none.
const std::map<uint32_t, uint32_t>* ExtraChoicesOf(const CCharacterComponent* character) {
    const auto extras = s_extraChoices.find(character);
    if (extras != s_extraChoices.end()) return &extras->second;
    const auto bound = s_boundGuids.find(character);
    if (bound == s_boundGuids.end()) return nullptr;
    const auto stored = s_storedChoices.find(bound->second);
    return stored != s_storedChoices.end() ? &stored->second : nullptr;
}

// One option as a character sees it: the choices offered given the class and the choices taken before
// it, and the one taken.
struct ResolvedOption {
    uint32_t id = 0;
    std::string name;
    bool driven = false;            // by a 3.3.5 selector
    std::vector<uint32_t> offered;
    uint32_t index = 0;             // into offered
};

// The character's options in OrderIndex order (Face before Skin Color). The five the 3.3.5 selectors
// drive take the Nth offered choice for the character's value (wrapping); the others take the stored
// extra choice, or their first. A choice that asks for a choice of an earlier option is offered only
// when that choice was taken.
uint32_t Resolve(const CCharacterComponent* character, uint32_t chrModel, std::vector<ResolvedOption>& resolved,
    std::vector<uint32_t>& choices) {
    const auto& prefs = character->m_data.m_preferences;
    std::vector<Retail::Appearance::Option> options;
    Retail::Appearance::Options(chrModel, prefs.m_classId, options);
    std::vector<int32_t> driven(options.size(), -1);
    for (const Stock stock : {Stock::Skin, Stock::Face, Stock::HairStyle, Stock::HairColor, Stock::Facial}) {
        bool found = false;
        for (const char* const* name = StockNames(stock, prefs.m_sexID); *name && !found; ++name)
            for (size_t i = 0; i < options.size() && !found; ++i)
                if (driven[i] < 0 && options[i].name == *name) {
                    driven[i] = static_cast<int32_t>(stock);
                    found = true;
                }
    }
    const auto* extras = ExtraChoicesOf(character);
    resolved.clear();
    choices.clear();
    for (size_t i = 0; i < options.size(); ++i) {
        ResolvedOption option;
        option.id = options[i].id;
        option.name = options[i].name;
        option.driven = driven[i] >= 0;
        for (const uint32_t id : options[i].choices)
            if (Retail::Appearance::RequirementsMet(id, choices)) option.offered.push_back(id);
        if (option.offered.empty()) option.offered = options[i].choices;
        if (option.driven) {
            option.index = StockValue(prefs, static_cast<Stock>(driven[i])) % option.offered.size();
        } else if (extras) {
            const auto stored = extras->find(option.id);
            if (stored != extras->end()) {
                const auto at = std::find(option.offered.begin(), option.offered.end(), stored->second);
                if (at != option.offered.end()) option.index = static_cast<uint32_t>(at - option.offered.begin());
            }
        }
        choices.push_back(option.offered[option.index]);
        resolved.push_back(std::move(option));
    }
    return chrModel;
}

// The character's Retail race and ChrModel, or 0 for stock bodies and unmapped races.
uint32_t RetailModelOf(const CCharacterComponent* character, uint32_t& retailRace) {
    CM2Model* model = character->m_data.m_model;
    if (!model || !model->m_shared || !Retail::Source().Handles(model->m_shared->m_fileName)) return 0;
    const auto& prefs = character->m_data.m_preferences;
    const auto& map = RaceMap();
    const auto race = map.find(prefs.m_raceID);
    if (race == map.end()) return 0;
    retailRace = race->second;
    return Retail::Appearance::ChrModelFor(race->second, prefs.m_sexID);
}

// The Retail look of a character with a Retail body, or null for stock bodies and unmapped races.
std::shared_ptr<Look> RetailLook(CCharacterComponent* character) {
    uint32_t retailRace = 0;
    const uint32_t chrModel = RetailModelOf(character, retailRace);
    if (!chrModel) return nullptr;
    std::vector<ResolvedOption> resolved;
    std::vector<uint32_t> choices;
    Resolve(character, chrModel, resolved, choices);
    return LookFor(retailRace, character->m_data.m_preferences.m_sexID, chrModel, choices);
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
    const auto found = RetailLook(character);
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
    for (const auto& texture : look.textures) model->ReplaceTexture(texture.first, texture.second->texture);
    return true;
}

bool RetailCharacterTextures(CCharacterComponent* character) {
    const auto look = RetailLook(character);
    if (!look) return false;
    for (const auto& texture : look->textures) character->m_data.m_model->ReplaceTexture(texture.first, texture.second->texture);
    return true;
}

// Geosets, then the hair/eye textures, then the body sheet on the next section pass.
static void Refresh(CCharacterComponent* character) {
    character->m_flags |= 0x4;
    character->SetDirtySections(0xFFFFFFFF);
    RetailCharacterTextures(character);
}

bool RetailCustomizationOptions(const CCharacterComponent* character, std::vector<RetailCustomizationOption>& options) {
    options.clear();
    uint32_t retailRace = 0;
    const uint32_t chrModel = RetailModelOf(character, retailRace);
    if (!chrModel) return false;
    std::vector<ResolvedOption> resolved;
    std::vector<uint32_t> choices;
    Resolve(character, chrModel, resolved, choices);
    for (const auto& option : resolved)
        if (!option.driven) options.push_back({option.name, option.index, static_cast<uint32_t>(option.offered.size())});
    return true;
}

bool RetailCustomizationCycle(CCharacterComponent* character, uint32_t index, int32_t delta, bool randomize) {
    uint32_t retailRace = 0;
    const uint32_t chrModel = RetailModelOf(character, retailRace);
    if (!chrModel) return false;
    // A bound character (the barber shop) starts its edits from its stored choices, so the options it
    // does not touch keep them.
    if (!s_extraChoices.count(character)) {
        if (const auto* stored = ExtraChoicesOf(character)) s_extraChoices[character] = *stored;
    }
    std::vector<ResolvedOption> resolved;
    std::vector<uint32_t> choices;
    Resolve(character, chrModel, resolved, choices);
    uint32_t extra = 0;
    bool changed = false;
    for (const auto& option : resolved) {
        if (option.driven) continue;
        const uint32_t count = static_cast<uint32_t>(option.offered.size());
        if (randomize) {
            s_extraChoices[character][option.id] = option.offered[static_cast<uint32_t>(std::rand()) % count];
            changed = true;
        } else if (extra == index) {
            const int32_t next = ((static_cast<int32_t>(option.index) + delta) % static_cast<int32_t>(count) + static_cast<int32_t>(count)) % static_cast<int32_t>(count);
            s_extraChoices[character][option.id] = option.offered[static_cast<uint32_t>(next)];
            changed = true;
        }
        ++extra;
    }
    if (!changed) return false;
    Refresh(character);
    return true;
}

bool RetailCustomizationEdited(const CCharacterComponent* character) {
    const auto extras = s_extraChoices.find(character);
    if (extras == s_extraChoices.end()) return false;
    RetailChoiceList edited, stored;
    RetailCustomizationChoices(character, edited);
    const auto saved = std::move(extras->second);
    s_extraChoices.erase(extras);
    RetailCustomizationChoices(character, stored);
    s_extraChoices[character] = saved;
    return edited != stored;
}

void RetailCustomizationDiscard(CCharacterComponent* character) {
    if (!s_extraChoices.erase(character)) return;
    uint32_t retailRace = 0;
    if (RetailModelOf(character, retailRace)) Refresh(character);
}

void RetailCustomizationTakeExtras(const CCharacterComponent* character, RetailCustomizationExtras& extras) {
    extras.clear();
    const auto found = s_extraChoices.find(character);
    if (found != s_extraChoices.end()) extras.assign(found->second.begin(), found->second.end());
}

void RetailCustomizationGiveExtras(const CCharacterComponent* character, const RetailCustomizationExtras& extras) {
    if (!character || extras.empty()) return;
    s_extraChoices[character] = std::map<uint32_t, uint32_t>(extras.begin(), extras.end());
}

void RetailCustomizationForget(const CCharacterComponent* character) {
    s_extraChoices.erase(character);
    s_boundGuids.erase(character);
}

bool RetailCustomizationChoices(const CCharacterComponent* character, RetailChoiceList& choices) {
    choices.clear();
    uint32_t retailRace = 0;
    const uint32_t chrModel = RetailModelOf(character, retailRace);
    if (!chrModel) {
        Blizzard::Log::Printf(1, kLog, "Create: stock body, no Retail choices sent\n");
        return false;
    }
    std::vector<ResolvedOption> resolved;
    std::vector<uint32_t> taken;
    Resolve(character, chrModel, resolved, taken);
    for (const auto& option : resolved)
        if (!option.driven) choices.emplace_back(option.id, option.offered[option.index]);
    std::string text;
    for (const auto& choice : choices) text += " " + std::to_string(choice.first) + "=" + std::to_string(choice.second);
    Blizzard::Log::Printf(1, kLog, "Create: sending%s\n", text.c_str());
    return true;
}

bool RetailCustomizationBind(const CCharacterComponent* character, uint64_t guid) {
    if (!character || !guid) return false;
    s_boundGuids[character] = guid;
    const bool known = s_storedChoices.count(guid) != 0;
    Blizzard::Log::Printf(1, kLog, "Bind %p to character %llu: %s\n", static_cast<const void*>(character),
        static_cast<unsigned long long>(guid), known ? "choices known" : "choices not known");
    if (known) return false;
    return s_askedGuids.insert(guid).second;
}

void RetailCustomizationStore(uint64_t guid, const RetailChoiceList& choices) {
    std::map<uint32_t, uint32_t> next(choices.begin(), choices.end());
    std::string text;
    for (const auto& choice : next) text += " " + std::to_string(choice.first) + "=" + std::to_string(choice.second);
    Blizzard::Log::Printf(1, kLog, "Server choices for character %llu:%s\n", static_cast<unsigned long long>(guid), text.empty() ? " none" : text.c_str());
    const auto known = s_storedChoices.find(guid);
    if (known != s_storedChoices.end() && known->second == next) return;
    s_storedChoices[guid] = std::move(next);
    for (const auto& bound : s_boundGuids) {
        if (bound.second != guid || s_extraChoices.count(bound.first)) continue;
        auto* character = const_cast<CCharacterComponent*>(bound.first);
        uint32_t retailRace = 0;
        if (RetailModelOf(character, retailRace)) Refresh(character);
    }
}

void RetailCharacterShutdown() {
    {
        std::lock_guard<std::mutex> lock(s_lookMutex);
        s_looks.clear();
        s_lookOrder.clear();
    }
    s_composedBodies.clear();
    s_composedBodyOrder.clear();
    s_composedTextures.clear();
    s_composedTextureOrder.clear();
    s_itemImages.clear();
    s_itemImageOrder.clear();
    for (auto& body : s_bodies) HandleClose(reinterpret_cast<HOBJECT>(body.second));
    s_bodies.clear();
    s_bodyOrder.clear();
    s_storedChoices.clear();
    s_boundGuids.clear();
    s_askedGuids.clear();
}

void RetailCharacterBody(CCharacterComponent* character) {
    RetailCharacterFitHelmet(character);
    const auto found = RetailLook(character);
    if (!found || !found->body || !found->body->width) return;
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

    auto& bodies = s_bodies;
    auto& order = s_bodyOrder;
    auto cached = bodies.find(name);
    if (cached == bodies.end()) {
        LogBuildType();
        const auto bodyStart = Clock::now();
        double itemRead = 0, itemDecode = 0, itemPaint = 0;
        uint32_t itemsReused = 0;
        auto start = Clock::now();
        Retail::Image sheet = *look.body;
        const double copy = Since(start);
        const auto& prefs = character->m_data.m_preferences;
        for (const auto& layer : layers) {
            void* handle = layer.texture;
            // Only dirty sections reload their item textures; the others were released by the last pass.
            const bool reload = !handle;
            // An item's section texture depends on the display, section and the wearer's race and sex
            // (its _M/_F/_U file). Guild tabards (display zero) are not kept: their emblems vary.
            std::string itemKey;
            if (layer.displayId) {
                itemKey = std::to_string(layer.displayId) + "/" + std::to_string(layer.section) + "/" + std::to_string(layer.layer)
                    + "/" + std::to_string(prefs.m_raceID) + "/" + std::to_string(prefs.m_sexID);
                const auto cached = s_itemImages.find(itemKey);
                if (cached != s_itemImages.end()) {
                    start = Clock::now();
                    for (const auto& section : look.sections)
                        if (section.sectionType == layer.section)
                            Retail::PaintImage(sheet, *cached->second, section.x, section.y, section.width, section.height, 2);
                    itemPaint += Since(start);
                    ++itemsReused;
                    continue;
                }
            }
            if (reload) {
                ItemDisplayInfoRec display;
                if (g_itemDisplayInfoDB.GetRecord(layer.displayId, &display))
                    handle = character->LoadItemComponentTexture(&display, static_cast<COMPONENT_SECTIONS>(layer.section));
            }
            start = Clock::now();
            TCTEXTUREINFO info{};
            TextureCacheGetInfo(handle, info, 1);
            const CACHEENTRY* entry = static_cast<const CACHEENTRY*>(handle);
            std::vector<uint8_t> bytes;
            if (entry && !entry->m_loadFailed && entry->m_data && info.m_width) {
                const auto* begin = reinterpret_cast<const uint8_t*>(entry->m_data);
                bytes.assign(begin, begin + entry->m_size);
            }
            if (reload && handle) TextureCacheDestroyTexture(handle);
            itemRead += Since(start);
            Retail::Image item;
            std::string error;
            start = Clock::now();
            const bool decoded = Retail::DecodeBlp(bytes, item, error);
            itemDecode += Since(start);
            if (!decoded) {
                Blizzard::Log::Printf(1, kLog, "Item texture for section %u layer %u (display %u): %s\n",
                    static_cast<unsigned>(layer.section), static_cast<unsigned>(layer.layer), static_cast<unsigned>(layer.displayId), error.c_str());
                continue;
            }
            start = Clock::now();
            for (const auto& section : look.sections)
                if (section.sectionType == layer.section)
                    Retail::PaintImage(sheet, item, section.x, section.y, section.width, section.height, 2);
            itemPaint += Since(start);
            if (!itemKey.empty()) {
                s_itemImages[itemKey] = std::make_shared<const Retail::Image>(std::move(item));
                s_itemImageOrder.push_back(itemKey);
                Trim(s_itemImages, s_itemImageOrder, 64);
            }
        }
        MakeStats make;
        CTexture* texture = MakeTexture(name, sheet, &make);
        Blizzard::Log::Printf(1, kLog,
            "Timing body: total %.1f ms | copy %.1f | %u item layers (%u reused): read %.1f, decode %.1f, paint %.1f | build %.1f, create %.1f, %.1f MB\n",
            Since(bodyStart), copy, static_cast<unsigned>(layers.size()), itemsReused, itemRead, itemDecode, itemPaint,
            make.build, make.upload, make.bytes / 1048576.0);
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

void RetailCharacterFitHelmet(CCharacterComponent* character) {
    CM2Model* body = character ? character->m_data.m_model : nullptr;
    if (!body || !(body->m_fFlags & 1) || !body->m_shared || !body->m_shared->m_data) return;
    uint32_t retailRace = 0;
    if (!RetailModelOf(character, retailRace)) return;
    CM2Model* helmet = body->m_attachList;
    while (helmet && helmet->m_attachmentId != ATTACH_HELM) helmet = helmet->m_attachNext;
    if (!helmet || !helmet->m_shared || Retail::Source().Handles(helmet->m_shared->m_fileName)) return;

    const M2Data* data = body->m_shared->m_data;
    if (data->m_boneIndicesById.m_count <= 6 || data->m_attachmentIndicesById.m_count <= ATTACH_HELM) return;
    const uint16_t head = data->m_boneIndicesById[6];
    const uint16_t socket = data->m_attachmentIndicesById[ATTACH_HELM];
    if (head >= data->m_bones.m_count || socket >= data->m_attachments.m_count) return;
    const auto& attachment = data->m_attachments[socket];

    // The 3.3.5 body the race's helmets were made for: Character\<Race>\<Sex>\<Race><Sex>.m2.
    static const char* const kStock[] = {nullptr, "Human", "Orc", "Dwarf", "NightElf", "Scourge", "Tauren", "Gnome",
        "Troll", nullptr, "BloodElf", "Draenei"};
    const auto& prefs = character->m_data.m_preferences;
    if (prefs.m_raceID >= sizeof(kStock) / sizeof(kStock[0]) || !kStock[prefs.m_raceID] || prefs.m_sexID > 1) return;
    const std::string race = kStock[prefs.m_raceID], sex = prefs.m_sexID ? "Female" : "Male";
    const std::string donorPath = "Character\\" + race + "\\" + sex + "\\" + race + sex + ".m2";

    struct Donor { bool tried = false, valid = false; Retail::HelmetAnchor anchor; };
    static std::map<std::string, Donor> donors;
    Donor& donor = donors[donorPath];
    if (!donor.tried) {
        donor.tried = true;
        void* bytes = nullptr;
        uint32_t size = 0;
        std::string error = "could not read the file";
        if (SFile::LoadFile(donorPath.c_str(), &bytes, &size, 0, nullptr)) {
            const auto* begin = static_cast<const uint8_t*>(bytes);
            donor.valid = Retail::ReadHelmetAnchor(std::vector<uint8_t>(begin, begin + size), donor.anchor, error);
            SFile::Unload(bytes);
        }
        if (donor.valid) {
            Blizzard::Log::Printf(1, kLog, "Helmet donor %s: head %.3f %.3f %.3f, socket %.3f %.3f %.3f\n", donorPath.c_str(),
                donor.anchor.head[0], donor.anchor.head[1], donor.anchor.head[2],
                donor.anchor.socket[0], donor.anchor.socket[1], donor.anchor.socket[2]);
        } else {
            Blizzard::Log::Printf(1, kLog, "Helmet donor %s: %s\n", donorPath.c_str(), error.c_str());
        }
    }
    if (!donor.valid) return;

    const auto& pivot = data->m_bones[head].m_pivot;
    const Retail::HelmetAnchor target{{pivot.x, pivot.y, pivot.z},
        {attachment.m_position.x, attachment.m_position.y, attachment.m_position.z}};
    const auto offset = Retail::HelmetTranslation(donor.anchor, target);

    // The helmet's own transform is applied in the socket's frame (CM2Model::Animate), so a translation
    // there moves it relative to the Retail socket.
    C44Matrix& local = helmet->m_worldTransform;
    if ((helmet->m_fFlags & 0x8000) && local.d0 == offset[0] && local.d1 == offset[1] && local.d2 == offset[2]) return;
    local.Identity();
    local.d0 = offset[0];
    local.d1 = offset[1];
    local.d2 = offset[2];
    helmet->m_fFlags |= 0x8000;

    static std::set<std::string> logged;
    if (logged.insert(std::string(body->m_shared->m_fileName)).second) {
        Blizzard::Log::Printf(1, kLog, "Helmet fit on %s: head bone %u, socket bone %u at %.3f %.3f %.3f, offset %.3f %.3f %.3f%s\n",
            body->m_shared->m_fileName, static_cast<unsigned>(head), static_cast<unsigned>(attachment.m_bone),
            target.socket[0], target.socket[1], target.socket[2], offset[0], offset[1], offset[2],
            attachment.m_bone == head ? "" : " (socket is not on the head bone)");
    }
}

void RetailCharacterAfterStockGeometry(CCharacterComponent* character) {
    CM2Model* model = character->m_data.m_model;
    if (!model) return;
    model->SetGeometryVisible(1701, 1799, 0);

    // Feet. Retail bodies have a bare foot with toes (2001) and a closed shoe (2002) as separate meshes;
    // the 3.3.5 boot texture painted on the bare foot showed the toes. With boots on, only the shoe shows
    // (Esteria's ApplyLegacyFootGeometry). Hoofed races (Tauren, Draenei) keep their feet.
    const uint32_t race = character->m_data.m_preferences.m_raceID;
    if (race != 6 && race != 11) {
        bool hasShoe = false;
        if ((model->m_fFlags & 1) && model->m_shared && model->m_shared->m_skinData) {
            const auto& sections = model->m_shared->m_skinData->m_skinSections;
            for (uint32_t i = 0; i < sections.m_count && !hasShoe; ++i)
                hasShoe = (sections[i].m_skinSectionId & 0xFFFF) == 2002;
        }
        const bool shoe = hasShoe && character->m_itemDisplayID[ITEMSLOT_BOOTS] != 0;
        model->SetGeometryVisible(2001, 2099, 0);
        model->SetGeometryVisible(shoe ? 2002 : 2001, shoe ? 2002 : 2001, 1);
    }

    // Helmet hides. AddHelm writes them into the 3.3.5 selector values, which the Retail geometry does
    // not read, so the chosen hair and beard showed through. HelmetGeosetVisData slots 0-6 cover geoset
    // groups 0 (hair), 1-3 (facial hair), 7 (ears), 16 and 17, per race bit, as in the stock AddHelm. On
    // Retail bodies group 0's 1-99 are the hairstyles (0 itself is the scalp), so those go.
    const auto& prefs = character->m_data.m_preferences;
    const uint32_t displayId = character->m_itemDisplayID[ITEMSLOT_HEAD];
    if (!displayId || prefs.m_sexID > 1 || prefs.m_raceID >= 32) return;
    CM2Model* helmet = model->m_attachList;
    while (helmet && helmet->m_attachmentId != ATTACH_HELM) helmet = helmet->m_attachNext;
    if (!helmet) return;    // hidden helm, barber shop
    ItemDisplayInfoRec display;
    if (!g_itemDisplayInfoDB.GetRecord(displayId, &display)) return;
    const auto* visibility = g_helmetGeosetVisDataDB.GetRecord(display.m_helmetGeosetVisID[prefs.m_sexID]);
    if (!visibility) return;
    static const uint32_t kGroups[7] = {0, 1, 2, 3, 7, 16, 17};
    const uint32_t raceBit = uint32_t{1} << prefs.m_raceID;
    const auto hides = [&](uint32_t slot) { return (static_cast<uint32_t>(visibility->m_hideGeoset[slot]) & raceBit) != 0; };
    // Facial hair goes only under helmets that hide all of it (full-face helms). Hoods and half helms
    // that hid one piece of the small 3.3.5 facial hair would take a whole Retail mustache or sideburns.
    const bool wholeFace = hides(1) && hides(2) && hides(3);
    for (uint32_t slot = 0; slot < 7; ++slot) {
        if (!hides(slot) || (slot >= 1 && slot <= 3 && !wholeFace)) continue;
        const uint32_t group = kGroups[slot];
        model->SetGeometryVisible(group ? group * 100 : 1, group * 100 + 99, 0);
    }
}

#endif
