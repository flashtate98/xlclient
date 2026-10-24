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
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Model2/M2Data.h"
#include "WoW/Common/ComponentCore/Types.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Common/TextureCache.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/ItemDisplayInfoRec.h"
#include "WoW/Source/DB/Rec/HelmetGeosetVisDataRec.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/Object/Types.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include <deque>
#include <cctype>
#include <cstring>
#include <cmath>
#include <sstream>
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
    std::vector<Retail::Appearance::SkinnedModel> parts;            // collection models (horns, ...)
    std::vector<Retail::Appearance::BoneSet> boneSets;              // face shapes (.bone files)
    uint32_t bodyFileDataId = 0;                                     // a conditional body (Orc Upright), else 0
    std::shared_ptr<const Retail::Image> body;                      // the composed skin sheet (type 1)
    std::string bodyKey;                                             // its layers (shared by looks)
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
    Retail::Appearance::SkinnedModels(chrModel, choices, look->parts);
    Retail::Appearance::BoneSets(chrModel, choices, look->boneSets);
    uint32_t ownBody = 0;
    look->bodyFileDataId = Retail::Appearance::BodyModel(chrModel, choices, ownBody);
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
            look->bodyKey = typeKey;
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

// Collection models: separately modelled parts a choice adds (Demon Hunter horns, blindfolds). Each is
// a Retail model attached to the body's root and drawn with the body's skeleton (SetSkinParent),
// showing the geosets the choices pick and the body's textures by type. One model holds every variant
// of its parts (all six horn styles), so a model is created per file and its geosets switch.
constexpr uint32_t kCollectionSocket = 0xFFFFFFFEu;

struct Collection {
    CM2Model* parent = nullptr;
    std::vector<uint16_t> geosets;
    // The look's composed textures (hair, eyes, blindfold (9), ...). Bound from here rather than from
    // the body's slots, which still hold placeholders while the body loads (character select, world).
    std::map<uint32_t, std::shared_ptr<OwnedTexture>> textures;
    bool reported = false;
    bool described = false;
    std::string lastDescribed;
};
std::unordered_map<CM2Model*, Collection> s_collections;    // by collection model

// Model names compare without regard to case or slash direction (the cache may normalise them).
std::string NameKey(const char* name) {
    std::string key = name ? name : "";
    for (char& c : key) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

bool IsCollection(const CM2Model* child, const CM2Model* parent) {
    return child && child->m_attachParent == parent && child->m_attachmentId == kCollectionSocket;
}

void ApplyCollection(CM2Model* child);
void OnCollectionLoadedThunk(CM2Model* child, void*) { ApplyCollection(child); }

// Binds a loaded collection to its body and shows its geosets and the body's textures.
void ApplyCollection(CM2Model* child) {
    const auto found = s_collections.find(child);
    if (found == s_collections.end()) return;
    Collection& state = found->second;
    CM2Model* parent = state.parent;
    if (!IsCollection(child, parent)) { s_collections.erase(found); return; }
    if (!(child->m_fFlags & 1) || !(parent->m_fFlags & 1)) {
        // Not loaded yet: come back when it is (the body, texture and geometry passes also retry).
        if (!child->m_loadedCallback) {
            child->m_loadedCallback = OnCollectionLoadedThunk;
            child->m_loadedArg = nullptr;
        }
        return;
    }
    if (!child->HasSkinParent(parent) && !child->SetSkinParent(parent)) {
        child->SetGeometryVisible(0, 65535, 0);
        if (!state.reported) {
            state.reported = true;
            Blizzard::Log::Printf(1, kLog, "Collection %s does not match the skeleton of %s; not shown\n",
                child->m_shared->m_fileName, parent->m_shared->m_fileName);
        }
        return;
    }
    child->SetGeometryVisible(0, 65535, 0);
    for (const uint16_t geoset : state.geosets) child->SetGeometryVisible(geoset, geoset, 1);

    // The collection's replaceable textures take the body's of the same type (the body's slots hold
    // the composed hair, skin, blindfold (9), ... textures).
    std::string described;
    const auto& wants = child->m_shared->m_data->m_textures;
    const auto& has = parent->m_shared->m_data->m_textures;
    for (uint32_t i = 0; i < wants.m_count; ++i) {
        const uint32_t type = wants[i].m_type;
        CTexture* texture = nullptr;
        const char* from = "body";
        const auto composed = type ? state.textures.find(type) : state.textures.end();
        if (composed != state.textures.end() && composed->second && composed->second->texture) {
            texture = composed->second->texture;
            from = "look";
        }
        for (uint32_t j = 0; !texture && type && parent->m_textures && j < has.m_count; ++j)
            if (has[j].m_type == type) texture = parent->m_textures[j];
        if (texture) child->ReplaceTexture(type, texture);
        described += " " + std::to_string(type);
        if (!type) described += "(file)";
        else if (!texture) described += "(none on the body)";
        else described += "(" + std::string(from) + ": " + texture->m_filename + ")";
    }
    if (!state.described || described != state.lastDescribed) {
        state.described = true;
        state.lastDescribed = described;
        Blizzard::Log::Printf(1, kLog, "Collection %s bound to %s, geosets %u, textures:%s\n", child->m_shared->m_fileName,
            parent->m_shared->m_fileName, static_cast<unsigned>(state.geosets.size()), described.c_str());
    }
}

void ApplyCollections(CM2Model* parent) {
    for (CM2Model* child = parent ? parent->m_attachList : nullptr; child; child = child->m_attachNext)
        if (child->m_attachmentId == kCollectionSocket) ApplyCollection(child);
}

// Makes the body's collection models match the look: drops models no choice wants, creates the missing
// ones (they bind once loaded) and updates the geosets of the rest.
void SyncCollections(CCharacterComponent* character, const Look& look) {
    CM2Model* parent = character->m_data.m_model;
    std::map<std::string, std::vector<uint16_t>> wanted;
    for (const auto& part : look.parts)
        if (part.geoset % 100) wanted[NameKey(Retail::FileName(part.fileDataId, ".m2").c_str())].push_back(static_cast<uint16_t>(part.geoset));

    for (CM2Model* child = parent->m_attachList; child;) {
        CM2Model* next = child->m_attachNext;
        if (child->m_attachmentId == kCollectionSocket) {
            const auto want = child->m_shared ? wanted.find(NameKey(child->m_shared->m_fileName)) : wanted.end();
            if (want == wanted.end()) {
                s_collections.erase(child);
                child->DetachFromParent();
            } else {
                Collection& state = s_collections[child];
                state.parent = parent;
                state.geosets = want->second;
                state.textures = look.textures;
                ApplyCollection(child);
                wanted.erase(want);
            }
        }
        child = next;
    }
    for (const auto& want : wanted) {
        CM2Model* child = parent->m_scene->CreateModel(want.first.c_str(), 0);
        if (!child) {
            Blizzard::Log::Printf(1, kLog, "Collection %s could not be created\n", want.first.c_str());
            continue;
        }
        child->SetGeometryVisible(0, 65535, 0);   // nothing shows until it follows the body's skeleton
        child->AttachToParent(parent, kCollectionSocket, nullptr, 1);
        Collection& state = s_collections[child];
        state.parent = parent;
        state.geosets = want.second;
        state.textures = look.textures;
        child->SetLoadedCallback(OnCollectionLoadedThunk, nullptr);
        child->Release();   // the attachment holds the body's reference
    }
}

// A .bone file: version, then chunks BIDA (uint16 bone indices) and BOMT (one 4x4 matrix per bone, the
// engine's own row-vector layout). Parsed files are kept by FileDataID.
using BoneShape = std::vector<std::pair<uint16_t, C44Matrix>>;

const BoneShape* BoneFile(uint32_t fileDataId) {
    static std::map<uint32_t, BoneShape> files;
    const auto cached = files.find(fileDataId);
    if (cached != files.end()) return &cached->second;
    BoneShape& shape = files[fileDataId];
    std::vector<uint8_t> bytes;
    std::string error;
    if (!Retail::Source().ReadFileDataId(fileDataId, bytes, error)) {
        Blizzard::Log::Printf(1, kLog, "Bone file %u: %s\n", static_cast<unsigned>(fileDataId), error.c_str());
        return &shape;
    }
    std::vector<uint16_t> ids;
    std::vector<C44Matrix> matrices;
    size_t at = 4;
    while (at + 8 <= bytes.size()) {
        uint32_t tag = 0, size = 0;
        std::memcpy(&tag, bytes.data() + at, 4);
        std::memcpy(&size, bytes.data() + at + 4, 4);
        at += 8;
        if (size > bytes.size() - at) break;
        if (tag == 0x41444942u) {          // "BIDA"
            ids.resize(size / 2);
            std::memcpy(ids.data(), bytes.data() + at, ids.size() * 2);
        } else if (tag == 0x544D4F42u) {   // "BOMT"
            matrices.resize(size / sizeof(C44Matrix));
            std::memcpy(static_cast<void*>(matrices.data()), bytes.data() + at, matrices.size() * sizeof(C44Matrix));
        }
        at += size;
    }
    if (ids.size() != matrices.size()) {
        Blizzard::Log::Printf(1, kLog, "Bone file %u: %u bones but %u matrices; not used\n", static_cast<unsigned>(fileDataId),
            static_cast<unsigned>(ids.size()), static_cast<unsigned>(matrices.size()));
        return &shape;
    }
    for (size_t i = 0; i < ids.size(); ++i) shape.emplace_back(ids[i], matrices[i]);
    return &shape;
}

// The face shape for the body model in use: the bone set made for this model's file (by path), or the
// only one offered.
void ApplyBoneShape(CM2Model* model, const Look& look, bool bodyMatches) {
    if (!model || !(model->m_fFlags & 1) || !model->m_shared) return;
    // A set is made for one skeleton: none applies while the body is not (yet) the one the look wants.
    if (!bodyMatches) {
        model->SetBoneAdjustments(BoneShape{});
        return;
    }
    const Retail::Appearance::BoneSet* chosen = look.boneSets.size() == 1 ? &look.boneSets.front() : nullptr;
    const std::string modelName = NameKey(model->m_shared->m_fileName);
    for (const auto& set : look.boneSets) {
        std::string path;
        if (NameKey(Retail::FileName(set.modelFileDataId, ".m2").c_str()) == modelName ||
            (Retail::Source().PathOf(set.modelFileDataId, path) && NameKey(path.c_str()) == modelName)) {
            chosen = &set;
            break;
        }
    }
    const BoneShape* shape = chosen ? BoneFile(chosen->boneFileDataId) : nullptr;
    model->SetBoneAdjustments(shape ? *shape : BoneShape{});
}

// Conditional bodies. Some choices replace the whole body model: the Orc male Upright posture is
// orcmaleupright.m2 (ChrCustomizationCondModel -> CreatureModelData), a different skeleton from the
// hunched orcmale_hd.m2. The body is swapped in place, following Esteria's SyncBodyModel: the new model
// takes the old one's transform, animation, callbacks, parent and attachments (weapons, effects), and
// every holder of the old model moves to it: the component, and the unit or model frame showing it.
// Glue screens (create, select) read the component's model each time, so they need nothing else.
static std::unordered_map<const CCharacterComponent*, std::string> s_originalBodies;   // before a swap
static std::unordered_map<const CCharacterComponent*, std::pair<RetailBodyOwner, void*>> s_bodyOwners;
static std::set<std::string> s_failedBodies;
static bool s_swappingBody = false;

// The body file a look calls for: its conditional model, else the body the component had before it
// swapped (empty when it never swapped: its own body is the right one).
static std::string WantedBody(const CCharacterComponent* character, const Look& look) {
    if (look.bodyFileDataId) return Retail::FileName(look.bodyFileDataId, ".m2");
    const auto original = s_originalBodies.find(character);
    return original != s_originalBodies.end() ? original->second : std::string();
}

static bool BodyIs(const CM2Model* model, const std::string& path) {
    return path.empty() || (model && model->m_shared && NameKey(model->m_shared->m_fileName) == NameKey(path.c_str()));
}

// Swaps the component's body for the one its look wants. False when nothing changed: the body is
// already right, is not loaded, or its holder is not known (a component in the world that no unit or
// frame shows is left alone, since something else may hold its model).
static bool SyncBodyModel(CCharacterComponent* character, const Look& look) {
    if (s_swappingBody) return false;
    CM2Model* old = character->m_data.m_model;
    const std::string path = WantedBody(character, look);
    if (BodyIs(old, path) || !old || !(old->m_fFlags & 1) || !old->m_scene || !old->m_shared) return false;
    if (s_failedBodies.count(NameKey(path.c_str()))) return false;

    CGUnit_C* unit = nullptr;
    const auto bound = s_boundGuids.find(character);
    if (bound != s_boundGuids.end() && ClntObjMgrGetActivePlayer()) {
        WOWGUID guid;
        guid.m_guid = bound->second;
        auto* found = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(guid, TYPEMASK_UNIT, __FILE__, __LINE__));
        if (found && found->m_characterComponent == character) unit = found;
    }
    // A unit and its component share one body: never move just one of them.
    if (unit && unit->m_worldModel != old) return false;
    const auto frame = s_bodyOwners.find(character);
    if (!unit && frame == s_bodyOwners.end() && ClntObjMgrGetActivePlayer()) return false;

    const auto start = Clock::now();
    CM2Model* replacement = old->m_scene->CreateModel(path.c_str(), old->m_flags);
    if (!replacement) {
        s_failedBodies.insert(NameKey(path.c_str()));
        Blizzard::Log::Printf(1, kLog, "Body %s could not be created; the body stays %s\n", path.c_str(), old->m_shared->m_fileName);
        return false;
    }
    replacement->WaitForLoad("Retail conditional body");
    if (!(replacement->m_fFlags & 1) || !replacement->m_shared || !replacement->m_shared->m_data) {
        s_failedBodies.insert(NameKey(path.c_str()));
        Blizzard::Log::Printf(1, kLog, "Body %s did not load (missing .m2, .skin or .skel files?); the body stays %s\n",
            path.c_str(), old->m_shared->m_fileName);
        replacement->Release();
        return false;
    }
    // Reattaching detaches first, so make sure every equipment socket exists before changing any links.
    for (CM2Model* child = old->m_attachList; child; child = child->m_attachNext) {
        if (child->m_attachmentId != kCollectionSocket && !(child->m_fFlags & 0x40000) &&
            !replacement->HasAttachment(child->m_attachmentId)) {
            s_failedBodies.insert(NameKey(path.c_str()));
            Blizzard::Log::Printf(1, kLog, "Body %s has no socket %u; the body stays %s\n", path.c_str(),
                static_cast<unsigned>(child->m_attachmentId), old->m_shared->m_fileName);
            replacement->Release();
            return false;
        }
    }

    s_swappingBody = true;
    replacement->m_worldTransform = old->m_worldTransform;
    replacement->m_matrixF4 = old->m_matrixF4;
    replacement->m_scaleSq = old->m_scaleSq;
    replacement->m_frameOfReferenceMatrix = old->m_frameOfReferenceMatrix;
    replacement->m_frameOfReference = old->m_frameOfReference == &old->m_frameOfReferenceMatrix
        ? &replacement->m_frameOfReferenceMatrix : old->m_frameOfReference;
    replacement->m_fFlags = (replacement->m_fFlags & ~0x8004u) | (old->m_fFlags & 0x8004u);
    replacement->m_lighting = old->m_lighting;
    replacement->m_lightingCallback = old->m_lightingCallback;
    replacement->m_lightingArg = old->m_lightingArg;
    replacement->m_drawCallback = old->m_drawCallback;
    replacement->m_drawArg = old->m_drawArg;
    replacement->m_eventCallback = old->m_eventCallback;
    replacement->m_eventCallbackParam = old->m_eventCallbackParam;
    replacement->m_sequenceCallback = old->m_sequenceCallback;
    replacement->m_sequenceCallbackParam = old->m_sequenceCallbackParam;
    replacement->m_loadedCallback = old->m_loadedCallback;
    replacement->m_loadedArg = old->m_loadedArg;
    replacement->m_fadeAlpha = old->m_fadeAlpha;
    replacement->m_baseAlpha = old->m_baseAlpha;
    replacement->m_diffuseTintR = old->m_diffuseTintR;
    replacement->m_diffuseTintG = old->m_diffuseTintG;
    replacement->m_diffuseTintB = old->m_diffuseTintB;
    replacement->m_emissiveTintR = old->m_emissiveTintR;
    replacement->m_emissiveTintG = old->m_emissiveTintG;
    replacement->m_emissiveTintB = old->m_emissiveTintB;
    replacement->m_effectiveAlpha = old->m_effectiveAlpha;
    replacement->m_alpha = old->m_alpha;
    replacement->m_diffuseColor = old->m_diffuseColor;
    replacement->m_emissiveColor = old->m_emissiveColor;

    M2BoneSequenceInfo sequence{};
    if (old->GetBoneSequenceInfo(UINT32_MAX, sequence))
        replacement->SetBoneSequence(UINT32_MAX, sequence.m_sequenceId, sequence.m_variationIndex,
            sequence.m_time, sequence.m_speed, 0, 1);
    else
        replacement->SetBoneSequence(UINT32_MAX, 0, UINT32_MAX, 0, 1.0f, 0, 1);

    if (old->m_attachParent) {
        replacement->AttachToParent(old->m_attachParent, old->m_attachmentId, nullptr, (old->m_fFlags & 0x40000) != 0);
        old->DetachFromParent();   // the component's reference keeps it alive until the end
    }
    for (CM2Model* child = old->m_attachList; child;) {
        CM2Model* next = child->m_attachNext;
        if (child->m_attachmentId == kCollectionSocket) {
            s_collections.erase(child);   // made again for the new body (SyncCollections)
            child->DetachFromParent();
        } else {
            const uint32_t socket = child->m_attachmentId;
            const bool root = (child->m_fFlags & 0x40000) != 0;
            ++child->m_refCount;          // AttachToParent releases the old parent's attachment reference
            child->AttachToParent(replacement, socket, nullptr, root);
            child->Release();             // only the new parent's attachment reference remains
        }
        child = next;
    }

    const std::string from = old->m_shared->m_fileName;
    if (look.bodyFileDataId) s_originalBodies.emplace(character, from);   // keeps the first (own) body
    else s_originalBodies.erase(character);
    character->m_data.m_model = replacement;   // CreateModel's reference now belongs to the component
    if (unit) {
        unit->m_torsoKeyBone = -1;             // found again on the new skeleton
        unit->SetCharacterModel(replacement);  // takes its own reference and releases the old one
    } else if (frame != s_bodyOwners.end()) {
        frame->second.first(frame->second.second, replacement);
    }
    character->m_flags |= 0x4;
    character->SetDirtySections(0xFFFFFFFF);
    s_swappingBody = false;
    Blizzard::Log::Printf(1, kLog, "Body swapped for %s: %s -> %s (%.1f ms)\n",
        unit ? "a unit" : frame != s_bodyOwners.end() ? "a model frame" : "a glue screen", from.c_str(), path.c_str(), Since(start));
    old->Release();   // the component's former reference, last
    return true;
}

void RetailCharacterSetBodyOwner(const CCharacterComponent* character, RetailBodyOwner swap, void* owner) {
    if (swap) s_bodyOwners[character] = { swap, owner };
    else s_bodyOwners.erase(character);
}

bool RetailCharacterGeometry(CCharacterComponent* character) {
    const auto found = RetailLook(character);
    if (!found) return false;
    const Look& look = *found;
    if (SyncBodyModel(character, look)) {
        // The stock reset ran on the old body.
        character->m_data.m_model->SetGeometryVisible(0, 2000, 0);
        character->m_data.m_model->SetGeometryVisible(0, 0, 1);
    }
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
    ApplyBoneShape(model, look, BodyIs(model, WantedBody(character, look)));
    SyncCollections(character, look);
    ApplyCollections(model);
    return true;
}

bool RetailCharacterTextures(CCharacterComponent* character) {
    const auto look = RetailLook(character);
    if (!look) return false;
    for (const auto& texture : look->textures) character->m_data.m_model->ReplaceTexture(texture.first, texture.second->texture);
    for (auto& collection : s_collections)
        if (collection.second.parent == character->m_data.m_model) collection.second.textures = look->textures;
    ApplyCollections(character->m_data.m_model);
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
    for (auto it = s_collections.begin(); it != s_collections.end();)
        it = it->second.parent == character->m_data.m_model ? s_collections.erase(it) : std::next(it);
    s_extraChoices.erase(character);
    s_boundGuids.erase(character);
    s_originalBodies.erase(character);
    s_bodyOwners.erase(character);
}

bool RetailCustomizationChoices(const CCharacterComponent* character, RetailChoiceList& choices) {
    choices.clear();
    uint32_t retailRace = 0;
    const uint32_t chrModel = RetailModelOf(character, retailRace);
    if (!chrModel) return false;
    std::vector<ResolvedOption> resolved;
    std::vector<uint32_t> taken;
    Resolve(character, chrModel, resolved, taken);
    for (const auto& option : resolved)
        if (!option.driven) choices.emplace_back(option.id, option.offered[option.index]);
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
    s_collections.clear();
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
    // Named by the skin layers rather than every choice: a new hairstyle or ears keep the same equipped sheet.
    std::string name = "Retail\\Character\\" + std::to_string(look.chrModel) + "\\" +
        std::to_string(std::hash<std::string>{}(look.bodyKey)) + "\\body";
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
    ApplyCollections(character->m_data.m_model);
}

// Hand tuning for helmet placement per client race and sex, from Data\Retail\helmets.csv beside the
// executable: rows "race;sex;scale;x;y;z" (scale 0 keeps the measured scale; x y z are added to the
// offset, in yards, z up). Read once.
struct HelmetTuning {
    float scale = 0;
    float offset[3] = {0, 0, 0};
};

const HelmetTuning* HelmetTuningFor(uint32_t race, uint32_t sex) {
    static std::map<uint32_t, HelmetTuning> tunings;
    static std::once_flag once;
    std::call_once(once, [] {
        std::filesystem::path path = RaceMapPath().parent_path() / "helmets.csv";
        std::ifstream file(path);
        std::string line;
        uint32_t rows = 0;
        while (std::getline(file, line)) {
            for (char& c : line) if (c == ';' || c == ',') c = ' ';
            std::istringstream fields(line);
            uint32_t r = 0, s = 0;
            HelmetTuning tuning;
            if (!(fields >> r >> s >> tuning.scale >> tuning.offset[0] >> tuning.offset[1] >> tuning.offset[2])) continue;
            tunings[(r << 1) | (s & 1)] = tuning;
            ++rows;
        }
        if (rows) Blizzard::Log::Printf(1, kLog, "%s: %u helmet tunings\n", path.string().c_str(), static_cast<unsigned>(rows));
    });
    const auto found = tunings.find((race << 1) | (sex & 1));
    return found == tunings.end() ? nullptr : &found->second;
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

    // Size: the head-to-jaw distance on both skeletons. Retail heads that grew a lot (Gnomes) scale the
    // helmet up; within 10% of the 3.3.5 head the helmet keeps its size (Human, Elves and Dwarves fit
    // unscaled).
    float scale = 1.0f;
    float measured = 1.0f;
    if (donor.anchor.hasJaw && data->m_boneIndicesById.m_count > 7) {
        const uint16_t jaw = data->m_boneIndicesById[7];
        if (jaw < data->m_bones.m_count) {
            const auto& jawPivot = data->m_bones[jaw].m_pivot;
            const auto distance = [](const float* a, float x, float y, float z) {
                return std::sqrt((a[0] - x) * (a[0] - x) + (a[1] - y) * (a[1] - y) + (a[2] - z) * (a[2] - z));
            };
            const float legacy = distance(donor.anchor.head.data(), donor.anchor.jaw[0], donor.anchor.jaw[1], donor.anchor.jaw[2]);
            const float retail = distance(target.head.data(), jawPivot.x, jawPivot.y, jawPivot.z);
            if (legacy > 0.001f && retail > 0.001f && std::isfinite(retail / legacy)) {
                measured = retail / legacy;
                if (std::fabs(measured - 1.0f) > 0.10f) scale = std::min(1.5f, std::max(0.8f, measured));
            }
        }
    }
    // Hand tuning from Data\Retail\helmets.csv, rows "race;sex;scale;x;y;z" (scale 0 keeps the measured one).
    float nudge[3] = {0, 0, 0};
    if (const HelmetTuning* tuning = HelmetTuningFor(prefs.m_raceID, prefs.m_sexID)) {
        if (tuning->scale > 0) scale = tuning->scale;
        for (int axis = 0; axis < 3; ++axis) nudge[axis] = tuning->offset[axis];
    }

    // The helmet keeps its 3.3.5 crown height above the head, scaled with the head, plus any nudge.
    std::array<float, 3> offset{};
    for (size_t axis = 0; axis < 3; ++axis)
        offset[axis] = target.head[axis] + scale * (donor.anchor.socket[axis] - donor.anchor.head[axis]) - target.socket[axis] + nudge[axis];

    // The helmet's own transform is applied in the socket's frame (CM2Model::Animate): scale about the
    // helmet's origin, then move it relative to the Retail socket.
    C44Matrix& local = helmet->m_worldTransform;
    if ((helmet->m_fFlags & 0x8000) && local.a0 == scale && local.d0 == offset[0] && local.d1 == offset[1] && local.d2 == offset[2]) return;
    local.Identity();
    local.a0 = scale;
    local.b1 = scale;
    local.c2 = scale;
    local.d0 = offset[0];
    local.d1 = offset[1];
    local.d2 = offset[2];
    helmet->m_fFlags |= 0x8000;

    static std::set<std::string> logged;
    if (logged.insert(std::string(body->m_shared->m_fileName)).second) {
        Blizzard::Log::Printf(1, kLog, "Helmet fit on %s: head bone %u, socket bone %u at %.3f %.3f %.3f, head size %.3f of the 3.3.5 head, scale %.3f, offset %.3f %.3f %.3f%s\n",
            body->m_shared->m_fileName, static_cast<unsigned>(head), static_cast<unsigned>(attachment.m_bone),
            target.socket[0], target.socket[1], target.socket[2], measured, scale, offset[0], offset[1], offset[2],
            attachment.m_bone == head ? "" : " (socket is not on the head bone)");
    }
}

void RetailCharacterAfterStockGeometry(CCharacterComponent* character) {
    CM2Model* model = character->m_data.m_model;
    if (!model) return;
    model->SetGeometryVisible(1701, 1799, 0);
    ApplyCollections(model);

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
