#if defined(WOWDECOMP_RETAIL_MODELS)

#include "WoW/Common/ComponentCore/RetailCharacter.h"
#include "WoW/Source/Object/ObjectClient/AlteredForm.h"
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
#include <list>
#include <cstdio>
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
#include <condition_variable>
#include <thread>
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
    uint32_t eyeTint = 0;                                            // FlashWOW Death Knight eye colour (0xRRGGBB), 0 for Frost
    bool placeholder = false;    // the tables only, no textures: shown (hidden) while a new character's look is built
};

bool FetchBlp(uint32_t fileDataId, std::vector<uint8_t>& bytes, void*) {
    std::string error;
    if (Retail::Source().ReadFileDataId(fileDataId, bytes, error)) return true;
    Blizzard::Log::Printf(1, kLog, "Texture %u: %s\n", static_cast<unsigned>(fileDataId), error.c_str());
    return false;
}

// FlashWOW Death Knight eye colours. Recolours a glow texture: each texel keeps its grey part (the white
// core, the grey flames) and its coloured part takes the target colour, so Blizzard's blue becomes it.
void TintImage(Retail::Image& image, uint32_t rgb) {
    const int target[3] = { int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF) };
    for (size_t p = 0; p + 3 < image.rgba.size(); p += 4) {
        uint8_t* c = &image.rgba[p];
        const int high = std::max({ int(c[0]), int(c[1]), int(c[2]) });
        const int low = std::min({ int(c[0]), int(c[1]), int(c[2]) });
        for (int k = 0; k < 3; ++k) c[k] = static_cast<uint8_t>(target[k] * (high - low) / 255 + low);
    }
}

// The Death Knight eye choice's own textures, to be composed tinted (FetchImage).
struct TintFetch {
    uint32_t rgb = 0;
    std::set<uint32_t> fileDataIds;
};

using Clock = std::chrono::steady_clock;
double Since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

// Decoded layer textures, kept between looks: a randomize or a new race reads the same base skin, underwear, eye and
// hair files for every look it builds, and reading and decoding them was over a third of each build. Keyed by
// FileDataID and, for the Death Knight eye textures, the tint drawn into them. The least recently used go first once
// the images pass kImageCacheBytes.
constexpr size_t kImageCacheBytes = 256u << 20;
struct ImageCache {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::pair<std::shared_ptr<const Retail::Image>, std::list<uint64_t>::iterator>> images;
    std::list<uint64_t> order;    // most recently used first
    size_t bytes = 0;
};
ImageCache s_imageCache;

std::shared_ptr<const Retail::Image> FetchImage(uint32_t fileDataId, void* context, Retail::ComposeStats* stats) {
    const auto* tint = static_cast<const TintFetch*>(context);
    const uint32_t rgb = tint && tint->rgb && tint->fileDataIds.count(fileDataId) ? tint->rgb : 0;
    const uint64_t key = uint64_t(fileDataId) | uint64_t(rgb) << 32;
    {
        std::lock_guard<std::mutex> lock(s_imageCache.mutex);
        const auto found = s_imageCache.images.find(key);
        if (found != s_imageCache.images.end()) {
            s_imageCache.order.splice(s_imageCache.order.begin(), s_imageCache.order, found->second.second);
            if (stats) ++stats->cached;
            return found->second.first;
        }
    }
    std::vector<uint8_t> bytes;
    auto start = Clock::now();
    const bool read = FetchBlp(fileDataId, bytes, nullptr);
    if (stats) { stats->fetch += Since(start); ++stats->files; }
    if (!read) return nullptr;
    start = Clock::now();
    auto image = std::make_shared<Retail::Image>();
    std::string error;
    const bool decoded = Retail::DecodeBlp(bytes, *image, error);
    if (decoded && rgb) TintImage(*image, rgb);
    if (stats) stats->decode += Since(start);
    if (!decoded) {
        Blizzard::Log::Printf(1, kLog, "Texture %u: %s\n", static_cast<unsigned>(fileDataId), error.c_str());
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(s_imageCache.mutex);
    if (s_imageCache.images.count(key)) return image;
    s_imageCache.order.push_front(key);
    s_imageCache.images.emplace(key, std::make_pair(image, s_imageCache.order.begin()));
    s_imageCache.bytes += image->rgba.size();
    while (s_imageCache.bytes > kImageCacheBytes && s_imageCache.order.size() > 1) {
        const auto oldest = s_imageCache.images.find(s_imageCache.order.back());
        s_imageCache.bytes -= oldest->second.first->rgba.size();
        s_imageCache.images.erase(oldest);
        s_imageCache.order.pop_back();
    }
    return image;
}

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
    // The filtering the player chose (trilinear by default, anisotropic if set), as the stock character skin has;
    // a fixed bilinear one left the Retail sheets grainy at a distance, with visible steps between mip levels.
    CGxTexFlags flags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1);
    CTexture* texture = TextureCreateFromBlpData(name.c_str(), flags, blp.data(), 1);
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

// A look in the making. BuildLook does everything that needs no graphics device (the tables, composing,
// the BLP files with their mips) and may run on the look worker; FinishLook creates the textures and keeps
// the look, on the main thread. The main thread owns every job: the worker only works on one it was given,
// so a job, and the textures its look shares, are never released on the worker.
struct LookJob {
    std::string key;
    uint32_t retailRace = 0, sex = 0, chrModel = 0;
    std::vector<uint32_t> picked;
    enum State { Queued, Running, Done } state = Queued;     // under s_jobMutex
    // Results.
    std::shared_ptr<Look> look;
    struct Pending {
        uint32_t textureType;
        std::string typeKey;
        std::vector<uint8_t> blp;
    };
    std::vector<Pending> pending;                            // composed textures to create
    std::string warnings, choicesText;
    double total = 0, tables = 0, composeTotal = 0;
    Retail::ComposeStats compose;
    MakeStats make;
    uint32_t reused = 0;
};

// Guards the composed bodies and textures, which the worker reads and the main thread trims.
std::mutex s_composedMutex;

std::string LookKey(uint32_t retailRace, uint32_t sex, const std::vector<uint32_t>& picked) {
    std::string key = std::to_string(retailRace) + "-" + std::to_string(sex);
    for (const uint32_t id : picked) key += "-" + std::to_string(id);
    return key;
}

// The parts of a look that come from the tables alone: geosets, sections, collection models, face shapes, body.
void LookTables(Look& look, const std::vector<uint32_t>& choices) {
    const uint32_t chrModel = look.chrModel;
    std::vector<uint16_t> defaults;
    Retail::Appearance::Geosets(chrModel, choices, look.geosets);
    Retail::Appearance::DefaultGeosets(look.geosets, defaults);
    look.geosets.insert(look.geosets.end(), defaults.begin(), defaults.end());
    Retail::Appearance::Sections(Retail::Appearance::LayoutFor(chrModel), look.sections);
    Retail::Appearance::SkinnedModels(chrModel, choices, look.parts);
    Retail::Appearance::BoneSets(chrModel, choices, look.boneSets);
    uint32_t ownBody = 0;
    look.bodyFileDataId = Retail::Appearance::BodyModel(chrModel, choices, ownBody);
}

void BuildLook(LookJob& job) {
    const auto lookStart = Clock::now();
    const uint32_t chrModel = job.chrModel;
    // A Death Knight eye colour draws as the Death Knight eye choice with its textures tinted.
    std::vector<uint32_t> choices;
    Retail::Appearance::BaseChoices(job.picked, choices);
    TintFetch tint;
    for (const uint32_t id : job.picked) {
        uint32_t base = 0, rgb = 0;
        if (!Retail::Appearance::EyeTint(id, base, rgb)) continue;
        tint.rgb = rgb;
        std::vector<Retail::Appearance::TextureLayer> own;
        Retail::Appearance::TextureLayers(chrModel, { base }, own);
        for (const auto& layer : own) tint.fileDataIds.insert(layer.fileDataId);
    }

    auto look = std::make_shared<Look>();
    look->chrModel = chrModel;
    look->key = job.key;
    look->eyeTint = tint.rgb;
    std::vector<Retail::Appearance::TextureLayer> layers;
    std::vector<Retail::ComposedTexture> composed;
    LookTables(*look, choices);
    Retail::Appearance::TextureLayers(chrModel, choices, layers);
    job.tables = Since(lookStart);
    std::map<uint32_t, std::vector<Retail::Appearance::TextureLayer>> byType;
    for (const auto& layer : layers) byType[layer.textureType].push_back(layer);
    for (const auto& type : byType) {
        std::string typeKey = std::to_string(chrModel) + "/" + std::to_string(type.first);
        for (const auto& layer : type.second)
            typeKey += ":" + std::to_string(layer.fileDataId) + "." + std::to_string(layer.blendMode) + "." + std::to_string(layer.sectionMask) +
                (tint.fileDataIds.count(layer.fileDataId) ? "~" + std::to_string(tint.rgb) : "");
        {
            std::lock_guard<std::mutex> lock(s_composedMutex);
            if (type.first == 1) {
                look->bodyKey = typeKey;
                const auto cached = s_composedBodies.find(typeKey);
                if (cached != s_composedBodies.end()) { look->body = cached->second; ++job.reused; continue; }
            } else {
                const auto cached = s_composedTextures.find(typeKey);
                if (cached != s_composedTextures.end()) { look->textures[type.first] = cached->second; ++job.reused; continue; }
            }
        }
        auto start = Clock::now();
        composed.clear();
        Retail::ComposeTextures(chrModel, type.second, &FetchImage, &tint, composed, job.warnings, &job.compose);
        job.composeTotal += Since(start);
        for (auto& texture : composed) {
            if (texture.textureType == 1) {
                look->body = std::make_shared<const Retail::Image>(std::move(texture.image));
                std::lock_guard<std::mutex> lock(s_composedMutex);
                s_composedBodies[typeKey] = look->body;
                s_composedBodyOrder.push_back(typeKey);
                Trim(s_composedBodies, s_composedBodyOrder, 6);
                continue;
            }
            start = Clock::now();
            LookJob::Pending made{texture.textureType, typeKey, {}};
            Retail::BuildBlp(texture.image, made.blp);
            job.make.build += Since(start);
            job.make.bytes += made.blp.size();
            ++job.make.count;
            job.pending.push_back(std::move(made));
        }
    }

    for (const uint32_t id : choices) job.choicesText += " " + std::to_string(id);
    job.look = std::move(look);
    job.total = Since(lookStart);
}

// Creates the job's textures and keeps its look (main thread).
std::shared_ptr<Look> FinishLook(LookJob& job) {
    Look& look = *job.look;
    for (auto& made : job.pending) {
        const auto start = Clock::now();
        CGxTexFlags flags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1);    // with the player's filtering (see MakeTexture)
        const std::string name = "Retail\\Character\\" + std::to_string(std::hash<std::string>{}(made.typeKey));
        CTexture* texture = TextureCreateFromBlpData(name.c_str(), flags, made.blp.data(), 1);
        job.make.upload += Since(start);
        if (!texture) continue;
        auto owned = std::make_shared<OwnedTexture>(texture);
        look.textures[made.textureType] = owned;
        std::lock_guard<std::mutex> lock(s_composedMutex);
        s_composedTextures[made.typeKey] = owned;
        s_composedTextureOrder.push_back(made.typeKey);
        Trim(s_composedTextures, s_composedTextureOrder, 48);
    }
    job.pending.clear();

    LogBuildType();
    Blizzard::Log::Printf(1, kLog,
        "Timing look: total %.1f ms | tables %.1f | %u texture types reused | compose %.1f (read %.1f for %u files, %u cached, decode %.1f, paint %.1f) | "
        "textures %u: build %.1f, create %.1f, %.1f MB | painted %u layers, %u scaled, %.1f Mpx drawn from %.1f Mpx\n",
        job.total + job.make.upload, job.tables, job.reused, job.composeTotal, job.compose.fetch, job.compose.files, job.compose.cached,
        job.compose.decode, job.compose.paint, job.make.count, job.make.build, job.make.upload, job.make.bytes / 1048576.0,
        job.compose.layers, job.compose.scaled, job.compose.drawn, job.compose.source);
    std::string text = job.choicesText + " | geosets";
    for (const uint16_t id : look.geosets) text += " " + std::to_string(id);
    text += " | textures";
    if (look.body && look.body->width) text += " 1 (" + std::to_string(look.body->width) + "x" + std::to_string(look.body->height) + ")";
    for (const auto& texture : look.textures) text += " " + std::to_string(texture.first);
    Blizzard::Log::Printf(1, kLog, "Retail race %u sex %u: ChrModel %u, choices%s\n%s", static_cast<unsigned>(job.retailRace),
        static_cast<unsigned>(job.sex), static_cast<unsigned>(job.chrModel), text.c_str(), job.warnings.c_str());

    std::lock_guard<std::mutex> lock(s_lookMutex);
    s_looks[job.key] = job.look;
    s_lookOrder.push_back(job.key);
    while (s_lookOrder.size() > 24) {
        const auto old = s_looks.find(s_lookOrder.front());
        if (old != s_looks.end()) s_looks.erase(old);   // its textures close with the last reference
        s_lookOrder.pop_front();
    }
    return job.look;
}

// FlashWOW: the look worker. Building a look took up to a fifth of a second on the main thread (a Dracthyr
// randomize), freezing the screen. A character that already shows a look keeps it while the new one is built
// here, and takes the new one when it is ready (RetailCharacterPump); a new character still waits for its
// first look, so it never shows without its textures.
std::mutex s_jobMutex;
std::condition_variable s_jobWake, s_jobDone;
std::map<std::string, std::unique_ptr<LookJob>> s_jobs;    // by look key; queued, running and done
std::deque<LookJob*> s_jobQueue;
bool s_jobStop = false;
std::thread* s_jobWorker = nullptr;                        // never destroyed: joined at shutdown

// A body sheet in the making: the look's composed skin with the equipped item textures painted over their
// sections. The main thread gathers the items (decoded ones it kept, else their bytes from the texture
// cache, which only it may use); the worker decodes, paints and builds the BLP; the main thread creates the
// texture and gives it to every character waiting for it (FinishBody).
struct BodyJob {
    std::string name;
    std::shared_ptr<const Retail::Image> skin;
    std::vector<Retail::Appearance::TextureSection> sections;
    struct Item {
        uint32_t section = 0, layer = 0, displayId = 0;
        std::string key;                                    // for s_itemImages; empty for guild tabards
        std::shared_ptr<const Retail::Image> decoded;       // kept from before, or
        std::vector<uint8_t> bytes;                         // the texture cache's BLP
    };
    std::vector<Item> items;
    enum State { Queued, Running, Done } state = Queued;    // under s_jobMutex
    // Results.
    std::vector<uint8_t> blp;
    std::vector<std::pair<std::string, std::shared_ptr<const Retail::Image>>> decoded;    // newly decoded, to keep
    std::string errors;
    uint32_t reused = 0;
    double gather = 0, copy = 0, decode = 0, paint = 0, build = 0, total = 0;
    Clock::time_point queued;
};
std::map<std::string, std::unique_ptr<BodyJob>> s_bodyJobs;    // by sheet name; queued, running and done
std::deque<BodyJob*> s_bodyQueue;
std::unordered_map<CCharacterComponent*, std::string> s_bodyWaiting;    // characters waiting for a sheet, by name

void BuildBody(BodyJob& job) {
    const auto bodyStart = Clock::now();
    auto start = Clock::now();
    Retail::Image sheet = *job.skin;
    job.copy = Since(start);
    for (auto& item : job.items) {
        std::shared_ptr<const Retail::Image> image = item.decoded;
        if (!image) {
            start = Clock::now();
            auto made = std::make_shared<Retail::Image>();
            std::string error;
            const bool decoded = Retail::DecodeBlp(item.bytes, *made, error);
            job.decode += Since(start);
            if (!decoded) {
                job.errors += "Item texture for section " + std::to_string(item.section) + " layer " + std::to_string(item.layer) +
                    " (display " + std::to_string(item.displayId) + "): " + error + "\n";
                continue;
            }
            image = made;
            if (!item.key.empty()) job.decoded.emplace_back(item.key, image);
        }
        start = Clock::now();
        for (const auto& section : job.sections)
            if (section.sectionType == item.section)
                Retail::PaintImage(sheet, *image, section.x, section.y, section.width, section.height, 2);
        job.paint += Since(start);
    }
    start = Clock::now();
    Retail::BuildBlp(sheet, job.blp);
    job.build = Since(start);
    job.total = Since(bodyStart);
}

// The worker and its painting helpers run below the game's main thread, on at most half the cores: at the
// same priority on every core they starved it, and a burst of randomize clicks froze the screen for a second.
void BelowNormalPriority() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

void LookWorker() {
    BelowNormalPriority();
    Retail::SetHelperThreads(std::max(1u, std::thread::hardware_concurrency() / 2), &BelowNormalPriority);
    std::unique_lock<std::mutex> lock(s_jobMutex);
    for (;;) {
        s_jobWake.wait(lock, [] { return s_jobStop || !s_jobQueue.empty() || !s_bodyQueue.empty(); });
        if (s_jobStop) return;
        // Bodies first: a body is the last step before a character is drawn.
        if (!s_bodyQueue.empty()) {
            BodyJob* body = s_bodyQueue.front();
            s_bodyQueue.pop_front();
            body->state = BodyJob::Running;
            lock.unlock();
            BuildBody(*body);
            lock.lock();
            body->state = BodyJob::Done;
            continue;
        }
        LookJob* job = s_jobQueue.front();
        s_jobQueue.pop_front();
        job->state = LookJob::Running;
        lock.unlock();
        BuildLook(*job);
        lock.lock();
        job->state = LookJob::Done;
        s_jobDone.notify_all();
    }
}

// Under s_jobMutex.
void EnsureLookWorker() {
    if (!s_jobWorker) s_jobWorker = new std::thread(LookWorker);
}

// The look for these choices: kept, or built now on this thread (waiting for the worker if it is on it).
std::shared_ptr<Look> LookFor(uint32_t retailRace, uint32_t sex, uint32_t chrModel, const std::vector<uint32_t>& picked) {
    const std::string key = LookKey(retailRace, sex, picked);
    {
        std::lock_guard<std::mutex> lock(s_lookMutex);
        const auto found = s_looks.find(key);
        if (found != s_looks.end()) return found->second;
    }
    std::unique_ptr<LookJob> job;
    {
        std::unique_lock<std::mutex> lock(s_jobMutex);
        const auto queued = s_jobs.find(key);
        if (queued != s_jobs.end()) {
            if (queued->second->state == LookJob::Queued)
                s_jobQueue.erase(std::find(s_jobQueue.begin(), s_jobQueue.end(), queued->second.get()));
            else
                s_jobDone.wait(lock, [&] { return queued->second->state == LookJob::Done; });
            job = std::move(queued->second);
            s_jobs.erase(queued);
        }
    }
    if (!job) {
        job = std::make_unique<LookJob>();
        job->key = key;
        job->retailRace = retailRace;
        job->sex = sex;
        job->chrModel = chrModel;
        job->picked = picked;
    }
    if (!job->look) BuildLook(*job);
    return FinishLook(*job);
}

// The look for these choices if it is ready; otherwise it is handed to the worker (once) and null comes back.
std::shared_ptr<Look> RequestLook(uint32_t retailRace, uint32_t sex, uint32_t chrModel, const std::vector<uint32_t>& picked,
    std::string& key) {
    key = LookKey(retailRace, sex, picked);
    {
        std::lock_guard<std::mutex> lock(s_lookMutex);
        const auto found = s_looks.find(key);
        if (found != s_looks.end()) return found->second;
    }
    std::unique_ptr<LookJob> done;
    {
        std::lock_guard<std::mutex> lock(s_jobMutex);
        const auto queued = s_jobs.find(key);
        if (queued != s_jobs.end()) {
            if (queued->second->state != LookJob::Done) return nullptr;
            done = std::move(queued->second);
            s_jobs.erase(queued);
        } else {
            EnsureLookWorker();
            auto job = std::make_unique<LookJob>();
            job->key = key;
            job->retailRace = retailRace;
            job->sex = sex;
            job->chrModel = chrModel;
            job->picked = picked;
            s_jobQueue.push_back(job.get());
            s_jobs.emplace(key, std::move(job));
            s_jobWake.notify_one();
            return nullptr;
        }
    }
    return FinishLook(*done);
}

// The look each character shows, and the characters waiting for a look the worker is building (by its key).
std::unordered_map<const CCharacterComponent*, std::shared_ptr<Look>> s_shownLooks;
std::unordered_map<CCharacterComponent*, std::string> s_waitingLooks;

// A character stops waiting for a look (it wants another, or none): the look's job is dropped if it has not
// started and nobody else waits for it. Right away, not at the next pump: clicks that pile up during a slow
// frame are all handled in one frame, and each would otherwise be built.
void StopWaiting(CCharacterComponent* character) {
    const auto waiting = s_waitingLooks.find(character);
    if (waiting == s_waitingLooks.end()) return;
    const std::string key = std::move(waiting->second);
    s_waitingLooks.erase(waiting);
    for (const auto& other : s_waitingLooks)
        if (other.second == key) return;
    std::lock_guard<std::mutex> lock(s_jobMutex);
    const auto job = s_jobs.find(key);
    if (job == s_jobs.end() || job->second->state != LookJob::Queued) return;
    s_jobQueue.erase(std::find(s_jobQueue.begin(), s_jobQueue.end(), job->second.get()));
    s_jobs.erase(job);
}

// A character stops waiting for a body sheet: its job is dropped if it has not started and nobody else waits.
void StopWaitingBody(CCharacterComponent* character) {
    const auto waiting = s_bodyWaiting.find(character);
    if (waiting == s_bodyWaiting.end()) return;
    const std::string name = std::move(waiting->second);
    s_bodyWaiting.erase(waiting);
    for (const auto& other : s_bodyWaiting)
        if (other.second == name) return;
    std::lock_guard<std::mutex> lock(s_jobMutex);
    const auto job = s_bodyJobs.find(name);
    if (job == s_bodyJobs.end() || job->second->state != BodyJob::Queued) return;
    s_bodyQueue.erase(std::find(s_bodyQueue.begin(), s_bodyQueue.end(), job->second.get()));
    s_bodyJobs.erase(job);
}

// Whether something is being built for the character: its look or its body sheet is queued or under way.
bool BeingBuilt(const CCharacterComponent* character) {
    auto* key = const_cast<CCharacterComponent*>(character);
    const auto look = s_waitingLooks.find(key);
    if (look != s_waitingLooks.end() && s_jobs.count(look->second)) return true;
    const auto body = s_bodyWaiting.find(key);
    return body != s_bodyWaiting.end() && s_bodyJobs.count(body->second);
}

// FlashWOW: a new character (a race picked on the create screen, a player coming into view) is not drawn until
// its first look is ready, instead of the game waiting for it: its body, and through it everything attached
// (equipment, collection models), draws at base alpha 0. It shows again once its body sheet is on
// (FinishBody), or when kRevealAfterMs pass with nothing being built for it, or after kRevealAlwaysMs whatever
// happened: in a crowd, waiting for the worker's turn is not a failure.
constexpr double kRevealAfterMs = 2000;
constexpr double kRevealAlwaysMs = 10000;
struct HiddenCharacter {
    Clock::time_point since;
    uint32_t retailRace = 0, sex = 0;
};
std::unordered_map<const CCharacterComponent*, HiddenCharacter> s_hiddenCharacters;
uint32_t s_shownCount = 0;
double s_shownTotal = 0, s_shownWorst = 0;

void HideCharacter(CCharacterComponent* character, uint32_t retailRace, uint32_t sex) {
    if (CM2Model* model = character->m_data.m_model) model->m_baseAlpha = 0.0f;
    if (!s_hiddenCharacters.count(character)) s_hiddenCharacters[character] = {Clock::now(), retailRace, sex};
}

void RevealCharacter(CCharacterComponent* character, const char* why) {
    const auto hidden = s_hiddenCharacters.find(character);
    if (hidden == s_hiddenCharacters.end()) return;
    if (CM2Model* model = character->m_data.m_model) model->m_baseAlpha = 1.0f;
    const double waited = Since(hidden->second.since);
    ++s_shownCount;
    s_shownTotal += waited;
    s_shownWorst = std::max(s_shownWorst, waited);
    Blizzard::Log::Printf(1, kLog, "Timing appear: race %u sex %u shown %.1f ms after its first look was asked for (%s) | "
        "%zu still hidden, %u shown so far, average %.1f ms, slowest %.1f ms\n", static_cast<unsigned>(hidden->second.retailRace),
        static_cast<unsigned>(hidden->second.sex), waited, why, s_hiddenCharacters.size() - 1, s_shownCount,
        s_shownTotal / s_shownCount, s_shownWorst);
    s_hiddenCharacters.erase(hidden);
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
    bool implied = false;           // set from the other choices (Retail::Appearance::ClassImplied); no row
    int32_t stock = -1;             // the Stock selector that drives it, when driven
    // Unnamed options (the Dracthyr dragon's three single-choice ones) are Retail's hidden ones: no row.
    bool Row() const { return !driven && !implied && !name.empty(); }
    std::vector<uint32_t> offered;
    uint32_t index = 0;             // into offered
};

// The character's options in OrderIndex order (Face before Skin Color). The five the 3.3.5 selectors
// drive take the Nth offered choice for the character's value (wrapping); the others take the stored
// extra choice, or their first. A choice that asks for a choice of an earlier option is offered only
// when that choice was taken.
constexpr uint32_t kListClass = 6;    // Death Knight: the one class with choices of its own (skin colours)

uint32_t ResolveFor(const CharacterPreferences& prefs, const std::map<uint32_t, uint32_t>* extras, uint32_t chrModel,
    std::vector<ResolvedOption>& resolved, std::vector<uint32_t>& choices) {
    // The options are the class's own (a Death Knight has its own Eye Color, with the glowing eyes). The five the
    // 3.3.5 selectors drive list their choices as a Death Knight sees them, whatever the class, so a 3.3.5 value
    // means the same choice for every class: the Death Knight skin colours keep their place (and CharSections keeps
    // them for Death Knights) instead of every later colour moving up for other classes. RetailCustomizationPanel
    // leaves out what the class may not take.
    std::vector<Retail::Appearance::Option> options, stable;
    Retail::Appearance::Options(chrModel, prefs.m_classId, options);
    Retail::Appearance::Options(chrModel, kListClass, stable);
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
    for (size_t i = 0; i < options.size(); ++i) {
        if (driven[i] < 0) continue;
        for (const auto& same : stable)
            if (same.id == options[i].id && same.choices.size() > options[i].choices.size()) options[i].choices = same.choices;
    }
    // A choice may ask for a choice of another option (the Death Knight skin colours ask for a Death Knight eye
    // colour, which comes later in the list). Requirements are checked against a first pass that takes every
    // option's choice unfiltered.
    std::vector<uint32_t> first;
    for (size_t i = 0; i < options.size(); ++i) {
        const auto& list = options[i].choices;
        if (list.empty()) { first.push_back(0); continue; }
        uint32_t index = 0;
        if (driven[i] >= 0) {
            index = StockValue(prefs, static_cast<Stock>(driven[i])) % list.size();
        } else if (extras) {
            const auto stored = extras->find(options[i].id);
            if (stored != extras->end()) {
                const auto at = std::find(list.begin(), list.end(), stored->second);
                if (at != list.end()) index = static_cast<uint32_t>(at - list.begin());
            }
        }
        first.push_back(list[index]);
    }
    resolved.clear();
    choices.clear();
    for (size_t i = 0; i < options.size(); ++i) {
        ResolvedOption option;
        option.id = options[i].id;
        option.name = options[i].name;
        option.driven = driven[i] >= 0;
        option.stock = driven[i];
        // A driven option keeps every choice in place (its value picks by position); the panel hides the ones whose
        // requirements are not met. The others offer only what is met.
        for (const uint32_t id : options[i].choices)
            if (option.driven || Retail::Appearance::RequirementsMet(id, first)) option.offered.push_back(id);
        if (option.offered.empty()) option.offered = options[i].choices;
        if (option.offered.empty()) continue;
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
    // Class-implied options follow the look: their Demon Hunter choice when any visible choice is a Demon
    // Hunter one (horns, blindfold, tattoo), else their other choice. FlashWOW opens Demon Hunter choices
    // to every class, so a class cannot decide it.
    bool demonHunter = false;
    for (size_t i = 0; i < resolved.size(); ++i)
        if (!resolved[i].name.empty() && Retail::Appearance::DemonHunterOnly(choices[i])) demonHunter = true;
    for (size_t i = 0; i < resolved.size(); ++i) {
        ResolvedOption& option = resolved[i];
        if (option.driven || !Retail::Appearance::ClassImplied(option.id)) continue;
        option.implied = true;
        for (uint32_t k = 0; k < option.offered.size(); ++k)
            if (Retail::Appearance::DemonHunterOnly(option.offered[k]) == demonHunter) {
                option.index = k;
                choices[i] = option.offered[k];
                break;
            }
    }
    return chrModel;
}

uint32_t Resolve(const CCharacterComponent* character, uint32_t chrModel, std::vector<ResolvedOption>& resolved,
    std::vector<uint32_t>& choices) {
    return ResolveFor(character->m_data.m_preferences, ExtraChoicesOf(character), chrModel, resolved, choices);
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

// The Retail look of a character with a Retail body, or null for stock bodies and unmapped races. A new look
// is built on the look worker while the character keeps the one it shows (geosets, textures and body sheet
// together, so it never shows half of each); a character showing nothing yet, or another body, waits for it.
std::shared_ptr<Look> RetailLook(CCharacterComponent* character) {
    uint32_t retailRace = 0;
    const uint32_t chrModel = RetailModelOf(character, retailRace);
    if (!chrModel) return nullptr;
    std::vector<ResolvedOption> resolved;
    std::vector<uint32_t> choices;
    Resolve(character, chrModel, resolved, choices);
    const uint32_t sex = character->m_data.m_preferences.m_sexID;
    const std::string key = LookKey(retailRace, sex, choices);
    if (const auto waiting = s_waitingLooks.find(character); waiting != s_waitingLooks.end() && waiting->second != key)
        StopWaiting(character);
    std::string requested;
    auto look = RequestLook(retailRace, sex, chrModel, choices, requested);
    if (!look) {
        const auto shown = s_shownLooks.find(character);
        if (shown != s_shownLooks.end() && shown->second->chrModel == chrModel) {
            s_waitingLooks[character] = key;
            return shown->second;
        }
        // A new character: hidden, with the look's geometry from the tables, until its textures are built.
        auto placeholder = std::make_shared<Look>();
        placeholder->chrModel = chrModel;
        placeholder->key = key;
        placeholder->placeholder = true;
        std::vector<uint32_t> base;
        Retail::Appearance::BaseChoices(choices, base);
        LookTables(*placeholder, base);
        s_shownLooks[character] = placeholder;
        s_waitingLooks[character] = key;
        HideCharacter(character, retailRace, sex);
        return placeholder;
    }
    s_waitingLooks.erase(character);
    s_shownLooks[character] = look;
    return look;
}

// One equipped item texture in a body section, in the stock compositor's order (Esteria's
// Equipment.h). Robes skip the leg sections' lower layers, as the stock compositor does.
struct EquipmentLayer {
    uint32_t section, layer, displayId;
    void* texture;
};

// Retail bodies that show no item textures (ChrModel 89: the Dracthyr dragon).
bool WearsNoArmorTextures(uint32_t chrModel) { return chrModel == 89; }

}   // namespace

bool RetailCharacterWearsNoHelmet(const CCharacterComponent* character) {
    uint32_t retailRace = 0;
    return RetailModelOf(character, retailRace) == 89;
}

namespace {

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
// Geoset ranges the worn helmet hides on a body (hair, facial hair, ...), by body model. Collections on
// that body hide the same ranges: some races keep their hair and beards there (Earthen).
std::unordered_map<CM2Model*, std::vector<std::pair<uint16_t, uint16_t>>> s_helmetHides;

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

// FlashWOW Death Knight eye colours on the eye-glow wisps (group 17). Their textures are fixed in the
// model file, not replaceable, so each tinted copy goes straight into the model's texture slots; Frost
// puts back an untinted copy where a tinted one was. Copies by file and colour; a few are kept.
constexpr const char* kTintPrefix = "Retail\\EyeTint\\";
std::map<std::string, std::shared_ptr<OwnedTexture>> s_tintedTextures;
std::deque<std::string> s_tintedTextureOrder;

CTexture* TintedModelTexture(uint32_t fileDataId, uint32_t rgb) {
    const std::string name = std::string(kTintPrefix) + std::to_string(fileDataId) + "\\" + std::to_string(rgb) + ".blp";
    const auto cached = s_tintedTextures.find(name);
    if (cached != s_tintedTextures.end()) return cached->second->texture;
    std::vector<uint8_t> bytes;
    if (!FetchBlp(fileDataId, bytes, nullptr)) return nullptr;
    Retail::Image image;
    std::string error;
    if (!Retail::DecodeBlp(bytes, image, error)) {
        Blizzard::Log::Printf(1, kLog, "Eye colour: texture %u: %s\n", static_cast<unsigned>(fileDataId), error.c_str());
        return nullptr;
    }
    if (rgb) TintImage(image, rgb);
    std::vector<uint8_t> blp;
    Retail::BuildBlp(image, blp);
    CGxTexFlags flags(GxTex_LinearMipLinear, 1, 1);   // as the model loads its own (the flames scroll)
    CTexture* texture = TextureCreateFromBlpData(name.c_str(), flags, blp.data(), 1);
    if (!texture) return nullptr;
    s_tintedTextures[name] = std::make_shared<OwnedTexture>(texture);
    s_tintedTextureOrder.push_back(name);
    Trim(s_tintedTextures, s_tintedTextureOrder, 32);
    return texture;
}

uint32_t RetailFileDataId(const char* file) {
    const size_t prefix = std::strlen(Retail::SourcePrefix());
    if (!file || std::strncmp(file, Retail::SourcePrefix(), prefix) != 0) return 0;
    uint32_t id = 0;
    const char* end = file + std::strlen(file);
    const auto result = std::from_chars(file + prefix, end, id);
    return result.ec == std::errc() ? id : 0;
}

void ApplyEyeTint(CM2Model* model, const Look& look) {
    if (!model || !(model->m_fFlags & 1) || !model->m_shared || !model->m_textures) return;
    M2Data* data = model->m_shared->m_data;
    M2SkinProfile* skin = model->m_shared->m_skinData;
    if (!data || !skin) return;
    const size_t prefix = std::strlen(kTintPrefix);
    for (uint32_t b = 0; b < skin->m_batches.m_count; ++b) {
        const M2Batch& batch = skin->m_batches[b];
        if (batch.m_skinSectionIndex >= skin->m_skinSections.m_count) continue;
        // Retail models keep the index start's upper word in the high half of the ID.
        const uint32_t geoset = skin->m_skinSections[batch.m_skinSectionIndex].m_skinSectionId & 0xFFFF;
        if (geoset < 1701 || geoset > 1799) continue;
        for (uint32_t k = 0; k < batch.m_textureCount; ++k) {
            const uint32_t combo = batch.m_textureComboIndex + k;
            if (combo >= data->m_textureCombos.m_count) break;
            const uint16_t index = data->m_textureCombos[combo];
            if (index >= data->m_textures.m_count || data->m_textures[index].m_type != 0) continue;
            const M2Texture& file = data->m_textures[index];
            const uint32_t fileDataId = RetailFileDataId(file.m_filename.m_count > 1 ? file.m_filename.Data() : nullptr);
            if (!fileDataId) continue;
            CTexture* current = model->m_textures[index];
            const bool ours = current && std::strncmp(current->m_filename, kTintPrefix, prefix) == 0;
            if (!look.eyeTint && !ours) continue;   // Frost on the model's own texture
            CTexture* wanted = TintedModelTexture(fileDataId, look.eyeTint);
            if (!wanted || wanted == current) continue;
            if (current) HandleClose(reinterpret_cast<HOBJECT>(current));
            model->m_textures[index] = reinterpret_cast<CTexture*>(HandleDuplicate(reinterpret_cast<HOBJECT>(wanted)));
            if (!TextureGetGxTex(model->m_textures[index], 0, nullptr)) model->m_fFlags &= ~2u;
        }
    }
}

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
    if (const auto hides = s_helmetHides.find(parent); hides != s_helmetHides.end())
        for (const auto& range : hides->second) child->SetGeometryVisible(range.first, range.second, 0);

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
    // Key bones playing their own sequence carry over too: the closed fingers holding a weapon
    // (ComponentCloseFingers, key bones 8-17), torso animations, ...
    const uint32_t keyBones = std::min(old->m_shared->m_data->m_boneIndicesById.m_count,
        replacement->m_shared->m_data->m_boneIndicesById.m_count);
    for (uint32_t bone = 0; bone < keyBones; ++bone) {
        M2BoneSequenceInfo own{};
        if (!old->GetBoneSequenceInfo(bone, own)) continue;
        if (own.m_sequenceId == sequence.m_sequenceId && own.m_variationIndex == sequence.m_variationIndex) continue;
        replacement->SetBoneSequence(bone, own.m_sequenceId, own.m_variationIndex, own.m_time, own.m_speed, 0, 1);
    }

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
    // The textures the component bound to the old body (skin sheet, hair, cape, ...) carry over by type,
    // so the new body draws right before the next section pass. That pass is asked for directly: the swap
    // can run inside a texture request (ProcessFinishedRequest), and SetDirtySections would cancel it
    // under that pass's feet.
    if (old->m_textures && replacement->m_shared->m_data) {
        const auto& had = old->m_shared->m_data->m_textures;
        const auto& wants = replacement->m_shared->m_data->m_textures;
        for (uint32_t i = 0; i < wants.m_count; ++i) {
            const uint32_t type = wants[i].m_type;
            for (uint32_t j = 0; type && j < had.m_count; ++j)
                if (had[j].m_type == type && old->m_textures[j]) {
                    replacement->ReplaceTexture(type, old->m_textures[j]);
                    break;
                }
        }
    }
    character->m_flags |= 0x4;
    character->m_dirtySections = 0xFFFFFFFF;
    s_swappingBody = false;
    Blizzard::Log::Printf(1, kLog, "Body swapped for %s: %s -> %s (%.1f ms)\n",
        unit ? "a unit" : frame != s_bodyOwners.end() ? "a model frame" : "a glue screen", from.c_str(), path.c_str(), Since(start));
    old->Release();   // the component's former reference, last
    return true;
}

const Retail::HelmetAnchor* StockAnchor(uint32_t raceId, uint32_t sexId);

// The glue screens' cameras were placed for the 3.3.5 bodies. A Retail body modelled much larger than its
// 3.3.5 body (the Tauren: heads 1.35x and 1.25x as high) is drawn at the 3.3.5 body's height, measured by the
// head bone. Bodies within 20% of it (every race but the Tauren; Gnomes are 15% taller) keep scale 1.
float RetailCharacterGlueScale(const CCharacterComponent* character) {
    if (!character) return 1.0f;
    const CM2Model* model = character->m_data.m_model;
    uint32_t retailRace = 0;
    if (!RetailModelOf(character, retailRace) || !(model->m_fFlags & 1) || !model->m_shared->m_data) return 1.0f;
    const auto& prefs = character->m_data.m_preferences;
    const Retail::HelmetAnchor* stock = StockAnchor(prefs.m_raceID, prefs.m_sexID);
    const M2Data* data = model->m_shared->m_data;
    if (!stock || data->m_boneIndicesById.m_count <= 6) return 1.0f;
    const uint16_t head = data->m_boneIndicesById[6];
    if (head >= data->m_bones.m_count) return 1.0f;
    const float retail = data->m_bones[head].m_pivot.z, legacy = stock->head[2];
    const float ratio = retail > 0.01f && legacy > 0.01f ? legacy / retail : 1.0f;
    const float scale = std::isfinite(ratio) && std::fabs(ratio - 1.0f) > 0.2f ? ratio : 1.0f;

    static std::map<std::string, float> reported;
    const std::string name = model->m_shared->m_fileName;
    const auto seen = reported.find(name);
    if (seen == reported.end() || seen->second != scale) {
        reported[name] = scale;
        Blizzard::Log::Printf(1, kLog, "Glue scale for %s: head %.3f high, 3.3.5 head %.3f, scale %.3f\n",
            name.c_str(), retail, legacy, scale);
    }
    return scale;
}

// Puts a glue screen body at its glue scale, keeping its facing and position (the screens set the transform
// before the body has loaded, when its head is not known yet).
static void ApplyGlueScale(CCharacterComponent* character) {
    CM2Model* model = character->m_data.m_model;
    if (ClntObjMgrGetActivePlayer() || !model || !(model->m_fFlags & 0x8000)) return;
    C44Matrix& m = model->m_worldTransform;
    const float current = std::sqrt(m.a0 * m.a0 + m.a1 * m.a1 + m.a2 * m.a2);
    const float wanted = RetailCharacterGlueScale(character);
    if (current < 0.001f || std::fabs(current - wanted) < 0.0005f) return;
    const float k = wanted / current;
    m.a0 *= k; m.a1 *= k; m.a2 *= k;
    m.b0 *= k; m.b1 *= k; m.b2 *= k;
    m.c0 *= k; m.c1 *= k; m.c2 *= k;
}

uint32_t StockRaceFor(uint32_t raceId, uint32_t sexId);
uint32_t RetailCharacterStockRace(uint32_t raceId, uint32_t sexId) { return StockRaceFor(raceId, sexId); }
uint32_t RetailCharacterHelmetRace(const CCharacterComponent* character) {
    const uint32_t race = character->m_data.m_preferences.m_raceID;
    uint32_t retailRace = 0;
    const uint32_t stock = RetailModelOf(character, retailRace) ? StockRaceFor(race, character->m_data.m_preferences.m_sexID) : 0;
    if (stock) return stock;
    // A race past the 32-bit masks that copies a stock race (the NPC races 34-43: Human, Orc, Dwarf, ... by
    // their ChrRaces client prefix) hides as that race.
    if (race >= 32)
        if (const uint32_t copied = StockRaceFor(race, character->m_data.m_preferences.m_sexID)) return copied;
    // FlashWOW moved its NPC races above 32 to free 23, 24 and 27-32 for new player races. The helmet hides
    // are 32-bit race masks, so the moved races keep the bit of their old number (which the new player races,
    // hiding by a stock race, do not read). Any other race past the masks hides nothing.
    switch (race) {
        case 46: return 23;     // Vrykul
        case 47: return 24;     // Fel Orc
        case 48: return 27;     // Skeleton
        case 49: return 28;     // Tuskarr
        case 50: return 29;     // Forest Troll
        case 51: return 30;     // Taunka
        case 52: return 31;     // Northrend Skeleton
        case 53: return 32;     // Ice Troll
        default: return race;
    }
}

namespace {
// How many choices the Retail option a 3.3.5 selector drives has, 0 for stock bodies or when it drives none.
uint32_t DrivenCount(const CCharacterComponent* character, Stock stock) {
    uint32_t retailRace = 0;
    const uint32_t chrModel = RetailModelOf(character, retailRace);
    if (!chrModel) return 0;
    std::vector<ResolvedOption> resolved;
    std::vector<uint32_t> choices;
    Resolve(character, chrModel, resolved, choices);
    for (const auto& option : resolved)
        if (option.stock == static_cast<int32_t>(stock)) return static_cast<uint32_t>(option.offered.size());
    return 0;
}
}

// How many values the facial hair selector has on a Retail body: the choices of the option it drives
// (Goblin female Earrings), 0 for stock bodies or when it drives none.
uint32_t RetailCharacterFacialCount(const CCharacterComponent* character) { return DrivenCount(character, Stock::Facial); }

// How many values the skin selector has on a Retail body: the choices of Skin Color (Fur Color, ...).
uint32_t RetailCharacterSkinCount(const CCharacterComponent* character) { return DrivenCount(character, Stock::Skin); }

void RetailCharacterSetBodyOwner(const CCharacterComponent* character, RetailBodyOwner swap, void* owner) {
    if (swap) s_bodyOwners[character] = { swap, owner };
    else s_bodyOwners.erase(character);
}

bool RetailCharacterGeometry(CCharacterComponent* character) {
    const auto geometryStart = Clock::now();
    struct Report {
        Clock::time_point start;
        ~Report() {
            const double spent = Since(start);
            if (spent > 5) Blizzard::Log::Printf(1, kLog, "Timing geometry: %.1f ms\n", spent);
        }
    } report{geometryStart};
    const auto found = RetailLook(character);
    if (!found) return false;
    const Look& look = *found;
    if (SyncBodyModel(character, look)) {
        // The stock reset ran on the old body.
        character->m_data.m_model->SetGeometryVisible(0, 2000, 0);
        character->m_data.m_model->SetGeometryVisible(0, 0, 1);
    }
    ApplyGlueScale(character);
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
    ApplyEyeTint(model, look);
    ApplyBoneShape(model, look, BodyIs(model, WantedBody(character, look)));
    SyncCollections(character, look);
    ApplyCollections(model);
    return true;
}

// The stock skin, face and hair setters ask for their textures here, each as soon as its value changes. The look is
// not built now: a randomize changes a dozen values one after another, and building each in-between look cost up to
// a quarter of a second apiece (a Dracthyr randomize built four looks, half a second, to show the last). The geometry
// pass marked here (GeosRenderPrep, once per frame) builds the look once from the final values and binds the same
// textures, eye tint and collections as this did.
bool RetailCharacterTextures(CCharacterComponent* character) {
    uint32_t retailRace = 0;
    if (!RetailModelOf(character, retailRace)) return false;
    character->m_flags |= 0x4;
    return true;
}

// Geosets and the hair/eye textures on the next geometry pass, then the body sheet on the next section pass.
static void Refresh(CCharacterComponent* character) {
    character->m_flags |= 0x4;
    character->SetDirtySections(0xFFFFFFFF);
}

bool BuildCustomizationPanel(const CCharacterComponent* character, uint32_t chrModel, std::vector<RetailPanelOption>& options);

bool RetailCustomizationPanel(const CCharacterComponent* character, std::vector<RetailPanelOption>& options) {
    options.clear();
    uint32_t retailRace = 0;
    const uint32_t chrModel = RetailModelOf(character, retailRace);
    if (!chrModel) return false;

    // The panel is built once per state and kept. The panel scripts read it one option at a time, and each read
    // built the whole panel again: a Dracthyr (some 30 options) rebuilt it 60 times per read, several reads per
    // randomize, which held the screen about 0.3 s per click. The key is everything the panel comes from: the
    // character, its body, its 3.3.5 values and its Retail choices.
    static std::string cachedKey;
    static std::vector<RetailPanelOption> cached;
    const auto& prefs = character->m_data.m_preferences;
    std::string key;
    key.reserve(256);
    key += std::to_string(reinterpret_cast<uintptr_t>(character)) + "/" +
        std::to_string(reinterpret_cast<uintptr_t>(character->m_data.m_model->m_shared)) + "/" + std::to_string(chrModel);
    for (const uint32_t value : {prefs.m_raceID, prefs.m_sexID, prefs.m_classId, prefs.m_skinID, prefs.m_faceId,
             prefs.m_hairStyleId, prefs.m_hairColorId, prefs.m_facialHairId})
        key += "/" + std::to_string(value);
    key += "|";
    if (const auto* extras = ExtraChoicesOf(character))
        for (const auto& extra : *extras) key += std::to_string(extra.first) + "=" + std::to_string(extra.second) + ",";
    if (key == cachedKey) {
        options = cached;
        return true;
    }
    const bool built = BuildCustomizationPanel(character, chrModel, options);
    cachedKey = std::move(key);
    cached = options;
    return built;
}

bool BuildCustomizationPanel(const CCharacterComponent* character, uint32_t chrModel, std::vector<RetailPanelOption>& options) {
    std::vector<ResolvedOption> resolved;
    std::vector<uint32_t> choices;
    Resolve(character, chrModel, resolved, choices);
    // What the character's own class may take, by option.
    std::vector<Retail::Appearance::Option> ownClass;
    Retail::Appearance::Options(chrModel, character->m_data.m_preferences.m_classId, ownClass);
    int32_t row = 0;
    for (const auto& option : resolved) {
        const bool isRow = option.Row();
        if (isRow || (option.driven && !option.name.empty())) {
            RetailPanelOption panel;
            panel.id = option.id;
            panel.name = option.name;
            Retail::Appearance::OptionDetails(option.id, panel.category, panel.type);
            panel.index = option.index;
            panel.row = isRow ? row : -1;
            panel.stock = option.driven ? option.stock : -1;
            panel.choices = option.offered;
            const std::vector<uint32_t>* own = nullptr;
            for (const auto& mine : ownClass)
                if (mine.id == option.id) own = &mine.choices;
            // A choice taken elsewhere may ask for particular choices of this option (a Death Knight skin colour
            // asks for a Death Knight face): then only those are offered here, and the UI moves to the first of them.
            std::vector<uint32_t> narrowed;
            for (const uint32_t taken : choices) {
                std::vector<uint32_t> needed;
                Retail::Appearance::RequiredChoices(taken, needed);
                for (const uint32_t id : needed)
                    if (std::find(option.offered.begin(), option.offered.end(), id) != option.offered.end()) narrowed.push_back(id);
            }
            for (const uint32_t id : option.offered)
                panel.allowed.push_back((!own || std::find(own->begin(), own->end(), id) != own->end()) &&
                    (narrowed.empty() || std::find(narrowed.begin(), narrowed.end(), id) != narrowed.end()));
            // Hidden too when its textures do not go with the choices taken (the faces of an unlocked NPC skin).
            panel.shows = Retail::Appearance::OptionShows(option.id, choices) && Retail::Appearance::OptionTextured(option.id, choices);
            options.push_back(std::move(panel));
        }
        if (isRow) ++row;
    }
    return true;
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
        if (option.Row()) options.push_back({option.name, option.index, static_cast<uint32_t>(option.offered.size())});
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
        if (!option.Row()) continue;
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
    s_shownLooks.erase(character);
    s_hiddenCharacters.erase(character);
    StopWaiting(const_cast<CCharacterComponent*>(character));
    StopWaitingBody(const_cast<CCharacterComponent*>(character));
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
        if (option.Row()) choices.emplace_back(option.id, option.offered[option.index]);
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

// FlashWOW Two Forms: choices that arrived during a form change, kept for the new body (RetailCustomizationFlushDeferred).
std::unordered_map<uint64_t, RetailChoiceList> s_deferredChoices;

void RetailCustomizationFlushDeferred(uint64_t guid) {
    const auto found = s_deferredChoices.find(guid);
    if (found == s_deferredChoices.end()) return;
    const RetailChoiceList choices = std::move(found->second);
    s_deferredChoices.erase(found);
    RetailCustomizationStore(guid, choices);
}

void RetailCustomizationStore(uint64_t guid, const RetailChoiceList& choices) {
    // FlashWOW Two Forms: during a form change the old body keeps the old choices; the new body gets these.
    if (AlteredFormTransitionPending(guid)) {
        s_deferredChoices[guid] = choices;
        return;
    }
    s_deferredChoices.erase(guid);
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

// FlashWOW Two Forms: the Worgen arms that grow from the Gilnean body during its change (a Retail
// collection model with its own copy of the change animation). It is not bound to the body's skeleton:
// its animation scales its own arm bones up from nothing, so it plays that animation itself, attached
// at the body's root, in step with the body. Its skin (texture type 1) is the Worgen's fur, composed
// from the Worgen look the change leads to.
constexpr uint32_t kTransformArmsSocket = 0xFFFFFFFDu;
constexpr uint32_t kTransformArmsAnim = 486;
std::set<CM2Model*> s_armsStarted;
std::map<std::string, std::shared_ptr<OwnedTexture>> s_armsSkins;
std::deque<std::string> s_armsSkinOrder;

CM2Model* TransformArmsOf(CM2Model* body) {
    for (CM2Model* child = body ? body->m_attachList : nullptr; child; child = child->m_attachNext)
        if (child->m_attachmentId == kTransformArmsSocket) return child;
    return nullptr;
}

// The Worgen fur sheet for the look the change leads to: its race, sex and selector values, and the
// choices that came with the change (else those stored for the character, else the defaults).
std::shared_ptr<OwnedTexture> TransformArmsSkin(uint64_t guid, const CharacterPreferences& prefs) {
    const auto& map = RaceMap();
    const auto race = map.find(prefs.m_raceID);
    if (race == map.end()) return nullptr;
    const uint32_t chrModel = Retail::Appearance::ChrModelFor(race->second, prefs.m_sexID);
    if (!chrModel) return nullptr;
    std::map<uint32_t, uint32_t> extras;
    const auto deferred = s_deferredChoices.find(guid);
    if (deferred != s_deferredChoices.end()) {
        extras.insert(deferred->second.begin(), deferred->second.end());
    } else if (const auto stored = s_storedChoices.find(guid); stored != s_storedChoices.end()) {
        extras = stored->second;
    }
    std::vector<ResolvedOption> resolved;
    std::vector<uint32_t> choices;
    ResolveFor(prefs, &extras, chrModel, resolved, choices);
    const auto look = LookFor(race->second, prefs.m_sexID, chrModel, choices);
    if (!look || !look->body || !look->body->width) return nullptr;
    const auto cached = s_armsSkins.find(look->bodyKey);
    if (cached != s_armsSkins.end()) return cached->second;
    CTexture* made = MakeTexture("Retail\\Character\\Arms\\" + std::to_string(std::hash<std::string>{}(look->bodyKey)), *look->body);
    if (!made) return nullptr;
    auto owned = std::make_shared<OwnedTexture>(made);
    s_armsSkins[look->bodyKey] = owned;
    s_armsSkinOrder.push_back(look->bodyKey);
    Trim(s_armsSkins, s_armsSkinOrder, 2);
    return owned;
}

bool RetailCharacterAttachTransformArms(CM2Model* body, uint64_t guid, uint32_t race, uint32_t sex, uint32_t classId,
    uint32_t skin, uint32_t face, uint32_t hairStyle, uint32_t hairColor, uint32_t facial) {
    RetailCharacterDropTransformArms(body);
    if (!body || !body->m_scene) return false;
    CharacterPreferences prefs{};
    prefs.m_raceID = race;
    prefs.m_sexID = sex;
    prefs.m_classId = classId;
    prefs.m_skinID = skin;
    prefs.m_faceId = face;
    prefs.m_hairStyleId = hairStyle;
    prefs.m_hairColorId = hairColor;
    prefs.m_facialHairId = facial;
    const auto fur = TransformArmsSkin(guid, prefs);
    const char* path = sex ? "item\\objectcomponents\\collections\\worgencombattransformation_wo_f.m2"
                           : "item\\objectcomponents\\collections\\worgencombattransformation_wo_m.m2";
    CM2Model* arms = body->m_scene->CreateModel(path, 0);
    if (!arms) {
        Blizzard::Log::Printf(1, kLog, "Transform arms %s could not be created\n", path);
        return false;
    }
    s_armsStarted.erase(arms);   // a model freed with an earlier body may have had this address
    arms->SetGeometryVisible(0, 65535, 0);   // shown once its animation starts (RetailCharacterUpdateTransformArms)
    arms->AttachToParent(body, kTransformArmsSocket, nullptr, 1);
    if (fur && fur->texture) arms->ReplaceTexture(1, fur->texture);
    arms->Release();   // the attachment holds the body's reference
    Blizzard::Log::Printf(1, kLog, "Transform arms %s attached to %s, fur %s\n", path,
        body->m_shared ? body->m_shared->m_fileName : "?", fur && fur->texture ? fur->texture->m_filename : "none");
    return true;
}

void RetailCharacterUpdateTransformArms(CM2Model* body, uint32_t elapsed) {
    CM2Model* arms = TransformArmsOf(body);
    if (!arms || !(arms->m_fFlags & 1) || s_armsStarted.count(arms)) return;
    s_armsStarted.insert(arms);
    // The arms (0) and bare hands (401); the glove (402-404) and sleeve (802, 803) variants stay hidden.
    arms->SetGeometryVisible(0, 0, 1);
    arms->SetGeometryVisible(401, 401, 1);
    arms->SetBoneSequence(0xFFFFFFFF, kTransformArmsAnim, 0xFFFFFFFF, 0, 1.0f, 0, 1);
    arms->SetBoneSequenceTime(0xFFFFFFFF, elapsed);
}

void RetailCharacterDropTransformArms(CM2Model* body) {
    while (CM2Model* arms = TransformArmsOf(body)) {
        s_armsStarted.erase(arms);
        arms->DetachFromParent();
    }
}

// Creates a finished body sheet's texture, keeps it, and puts it on every character waiting for it (main thread).
void FinishBody(BodyJob& job) {
    const auto start = Clock::now();
    CGxTexFlags flags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1);    // with the player's filtering (see MakeTexture)
    CTexture* texture = job.blp.empty() ? nullptr : TextureCreateFromBlpData(job.name.c_str(), flags, job.blp.data(), 1);
    const double create = Since(start);
    LogBuildType();
    Blizzard::Log::Printf(1, kLog,
        "Timing body: total %.1f ms (%.1f on this thread) | gather %.1f | copy %.1f | %u item layers (%u reused): decode %.1f, paint %.1f | "
        "build %.1f, create %.1f, %.1f MB | queued %.1f ms before done\n%s",
        job.gather + job.total + create, job.gather + create, job.gather, job.copy, static_cast<unsigned>(job.items.size()), job.reused,
        job.decode, job.paint, job.build, create, job.blp.size() / 1048576.0, Since(job.queued), job.errors.c_str());
    for (auto& decoded : job.decoded) {
        s_itemImages[decoded.first] = std::move(decoded.second);
        s_itemImageOrder.push_back(decoded.first);
    }
    Trim(s_itemImages, s_itemImageOrder, 64);
    if (texture) {
        s_bodies.emplace(job.name, texture);
        s_bodyOrder.push_back(job.name);
    }
    // Every waiting character takes it now, before the cache below may let it go (each model holds its own
    // reference): in a crowd many sheets finish in one frame.
    std::vector<CCharacterComponent*> waiting;
    for (const auto& entry : s_bodyWaiting)
        if (entry.second == job.name) waiting.push_back(entry.first);
    for (CCharacterComponent* character : waiting) {
        s_bodyWaiting.erase(character);
        if (texture && character->m_data.m_model) {
            character->m_data.m_model->ReplaceTexture(1, texture);
            ApplyCollections(character->m_data.m_model);
        }
        RevealCharacter(character, texture ? "look ready" : "body sheet failed");
    }
    // Each state is a full-size sheet; keep a handful.
    while (s_bodyOrder.size() > 8) {
        const auto old = s_bodies.find(s_bodyOrder.front());
        if (old != s_bodies.end()) {
            HandleClose(reinterpret_cast<HOBJECT>(old->second));
            s_bodies.erase(old);
        }
        s_bodyOrder.pop_front();
    }
}

void RetailCharacterPump() {
    // Diagnostics: a long gap between two updates means the main thread was held up (the screen froze).
    static Clock::time_point last;
    const auto now = Clock::now();
    if (last != Clock::time_point()) {
        const double gap = std::chrono::duration<double, std::milli>(now - last).count();
        if (gap > 100) Blizzard::Log::Printf(1, kLog, "Timing frame: %.0f ms since the last update%s\n", gap,
            s_waitingLooks.empty() ? "" : " (a look was awaited)");
    }
    last = now;
    // The safety net: nobody stays hidden for long, whatever happened to their look.
    if (!s_hiddenCharacters.empty()) {
        std::vector<CCharacterComponent*> overdue;
        for (const auto& hidden : s_hiddenCharacters) {
            const double waited = Since(hidden.second.since);
            if (waited > kRevealAlwaysMs || (waited > kRevealAfterMs && !BeingBuilt(hidden.first)))
                overdue.push_back(const_cast<CCharacterComponent*>(hidden.first));
        }
        for (CCharacterComponent* character : overdue) RevealCharacter(character, "timed out: shown without its look");
    }
    // Finished body sheets go straight to the characters waiting for them.
    if (!s_bodyJobs.empty()) {
        std::vector<std::unique_ptr<BodyJob>> bodies;
        {
            std::lock_guard<std::mutex> lock(s_jobMutex);
            for (auto it = s_bodyJobs.begin(); it != s_bodyJobs.end();)
                if (it->second->state == BodyJob::Done) {
                    bodies.push_back(std::move(it->second));
                    it = s_bodyJobs.erase(it);
                } else {
                    ++it;
                }
        }
        for (auto& body : bodies) FinishBody(*body);
    }
    // s_jobs is only changed on this thread (the worker only takes jobs off the queue), so this needs no lock.
    if (s_jobs.empty()) return;
    std::set<std::string> wanted;
    for (const auto& waiting : s_waitingLooks) wanted.insert(waiting.second);
    std::vector<std::unique_ptr<LookJob>> finished;
    {
        std::lock_guard<std::mutex> lock(s_jobMutex);
        for (auto it = s_jobs.begin(); it != s_jobs.end();) {
            LookJob* job = it->second.get();
            if (job->state == LookJob::Done) {
                finished.push_back(std::move(it->second));
                it = s_jobs.erase(it);
            } else if (job->state == LookJob::Queued && !wanted.count(it->first)) {
                // Nobody waits for it any more (clicked past it before it started): not built.
                s_jobQueue.erase(std::find(s_jobQueue.begin(), s_jobQueue.end(), job));
                it = s_jobs.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (finished.empty()) return;
    for (auto& job : finished) FinishLook(*job);    // kept even when nobody waits for it now
    finished.clear();

    // Each character whose look is ready takes it on its next geometry and section pass, queued now. Collected
    // first: queueing a character may prepare it on the spot, which asks for its look again.
    std::vector<CCharacterComponent*> ready;
    {
        std::lock_guard<std::mutex> lock(s_lookMutex);
        // Also one whose look is neither kept nor being built (dropped from the cache): it asks again.
        for (const auto& waiting : s_waitingLooks)
            if (s_looks.count(waiting.second) || !s_jobs.count(waiting.second)) ready.push_back(waiting.first);
    }
    for (CCharacterComponent* character : ready) {
        if (!s_waitingLooks.erase(character)) continue;
        character->m_flags |= 0x4;
        character->SetDirtySections(0xFFFFFFFF);
        character->RenderPrep(0);
    }
}

void RetailCharacterShutdown() {
    // The worker first: it reads the caches below and the Retail files.
    if (s_jobWorker) {
        {
            std::lock_guard<std::mutex> lock(s_jobMutex);
            s_jobStop = true;
        }
        s_jobWake.notify_all();
        s_jobWorker->join();
        s_jobWorker = nullptr;    // the thread object itself is left: nothing runs on it
    }
    s_jobQueue.clear();
    s_jobs.clear();
    s_bodyQueue.clear();
    s_bodyJobs.clear();
    s_bodyWaiting.clear();
    s_waitingLooks.clear();
    s_shownLooks.clear();
    s_hiddenCharacters.clear();
    {
        std::lock_guard<std::mutex> lock(s_lookMutex);
        s_looks.clear();
        s_lookOrder.clear();
    }
    s_composedBodies.clear();
    s_composedBodyOrder.clear();
    s_composedTextures.clear();
    s_composedTextureOrder.clear();
    s_tintedTextures.clear();
    s_tintedTextureOrder.clear();
    s_armsSkins.clear();
    s_armsSkinOrder.clear();
    s_armsStarted.clear();
    s_itemImages.clear();
    {
        std::lock_guard<std::mutex> lock(s_imageCache.mutex);
        s_imageCache.images.clear();
        s_imageCache.order.clear();
        s_imageCache.bytes = 0;
    }
    s_itemImageOrder.clear();
    s_collections.clear();
    for (auto& body : s_bodies) HandleClose(reinterpret_cast<HOBJECT>(body.second));
    s_bodies.clear();
    s_bodyOrder.clear();
    s_storedChoices.clear();
    s_deferredChoices.clear();
    s_boundGuids.clear();
    s_askedGuids.clear();
}

void RetailCharacterBody(CCharacterComponent* character) {
    RetailCharacterFitHelmet(character);
    const auto found = RetailLook(character);
    if (found) ApplyEyeTint(character->m_data.m_model, *found);
    if (!found || found->placeholder) return;
    if (!found->body || !found->body->width) {
        RevealCharacter(character, "no body sheet");
        return;
    }
    const Look& look = *found;

    // The 3.3.5 item section grid is the left half of the Retail sheet at twice the size, in the same
    // section order, so each equipped item texture is stretched over its section's rectangle.
    // The Dracthyr dragon wears no armour on its body, as on Retail: its armour pieces are customization
    // choices. Equipped items keep their models (weapons, shoulders) but paint nothing on the skin.
    const auto layers = WearsNoArmorTextures(look.chrModel) ? std::vector<EquipmentLayer>{}
                                                            : CollectEquipmentLayers(character, (character->m_flags & 0x20) != 0);
    // Named by the skin layers rather than every choice: a new hairstyle or ears keep the same equipped sheet.
    std::string name = "Retail\\Character\\" + std::to_string(look.chrModel) + "\\" +
        std::to_string(std::hash<std::string>{}(look.bodyKey)) + "\\body";
    for (const auto& layer : layers) {
        name += "/" + std::to_string(layer.section) + "." + std::to_string(layer.layer) + "." + std::to_string(layer.displayId);
        // Guild tabard layers have display ID zero; their file names tell the emblems and colours apart.
        if (!layer.displayId && layer.texture) name += ":" + std::string(static_cast<const CACHEENTRY*>(layer.texture)->m_fileName);
    }

    const auto cached = s_bodies.find(name);
    if (cached != s_bodies.end()) {
        StopWaitingBody(character);
        character->m_data.m_model->ReplaceTexture(1, cached->second);
        ApplyCollections(character->m_data.m_model);
        RevealCharacter(character, "look ready");
        return;
    }

    // Built on the look worker: meanwhile the character keeps the sheet it has (or stays hidden, if new).
    if (const auto waiting = s_bodyWaiting.find(character); waiting != s_bodyWaiting.end() && waiting->second != name)
        StopWaitingBody(character);
    s_bodyWaiting[character] = name;
    if (s_bodyJobs.count(name)) return;

    const auto gatherStart = Clock::now();
    auto job = std::make_unique<BodyJob>();
    job->name = name;
    job->skin = look.body;
    job->sections = look.sections;
    const auto& prefs = character->m_data.m_preferences;
    for (const auto& layer : layers) {
        BodyJob::Item item;
        item.section = layer.section;
        item.layer = layer.layer;
        item.displayId = layer.displayId;
        void* handle = layer.texture;
        // Only dirty sections reload their item textures; the others were released by the last pass.
        const bool reload = !handle;
        // An item's section texture depends on the display, section and the wearer's race and sex
        // (its _M/_F/_U file). Guild tabards (display zero) are not kept: their emblems vary.
        if (layer.displayId) {
            item.key = std::to_string(layer.displayId) + "/" + std::to_string(layer.section) + "/" + std::to_string(layer.layer)
                + "/" + std::to_string(prefs.m_raceID) + "/" + std::to_string(prefs.m_sexID);
            const auto kept = s_itemImages.find(item.key);
            if (kept != s_itemImages.end()) {
                item.decoded = kept->second;
                ++job->reused;
                job->items.push_back(std::move(item));
                continue;
            }
        }
        if (reload) {
            ItemDisplayInfoRec display;
            if (g_itemDisplayInfoDB.GetRecord(layer.displayId, &display))
                handle = character->LoadItemComponentTexture(&display, static_cast<COMPONENT_SECTIONS>(layer.section));
        }
        TCTEXTUREINFO info{};
        TextureCacheGetInfo(handle, info, 1);
        const CACHEENTRY* entry = static_cast<const CACHEENTRY*>(handle);
        if (entry && !entry->m_loadFailed && entry->m_data && info.m_width) {
            const auto* begin = reinterpret_cast<const uint8_t*>(entry->m_data);
            item.bytes.assign(begin, begin + entry->m_size);
        }
        if (reload && handle) TextureCacheDestroyTexture(handle);
        job->items.push_back(std::move(item));
    }
    job->gather = Since(gatherStart);
    job->queued = Clock::now();
    std::lock_guard<std::mutex> lock(s_jobMutex);
    EnsureLookWorker();
    s_bodyQueue.push_back(job.get());
    s_bodyJobs.emplace(name, std::move(job));
    s_jobWake.notify_one();
}

// Hand tuning for helmet placement per client race and sex, from Data\Retail\helmets.csv beside the
// executable: rows "race;sex;scale;x;y;z" (scale 0 keeps the measured scale; x y z are added to the
// offset, in yards, z up). Read once.
struct HelmetTuning {
    float scale = 0;
    float offset[3] = {0, 0, 0};
};

std::map<uint32_t, HelmetTuning>& HelmetTunings() {
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
    return tunings;
}

const HelmetTuning* HelmetTuningFor(uint32_t race, uint32_t sex) {
    auto& tunings = HelmetTunings();
    const auto found = tunings.find((race << 1) | (sex & 1));
    return found == tunings.end() ? nullptr : &found->second;
}

std::string RetailCharacterTuneHelmet(CCharacterComponent* character, float scale, float x, float y, float z) {
    if (!character) return std::string();
    const auto& prefs = character->m_data.m_preferences;
    HelmetTuning& tuning = HelmetTunings()[(prefs.m_raceID << 1) | (prefs.m_sexID & 1)];
    tuning.scale = scale;
    tuning.offset[0] = x;
    tuning.offset[1] = y;
    tuning.offset[2] = z;
    RetailCharacterFitHelmet(character);
    char row[96];
    std::snprintf(row, sizeof(row), "%u;%u;%.3f;%.3f;%.3f;%.3f", static_cast<unsigned>(prefs.m_raceID),
        static_cast<unsigned>(prefs.m_sexID), scale, x, y, z);
    Blizzard::Log::Printf(1, kLog, "Helmet tuning set from the game: %s (add it to helmets.csv to keep it)\n", row);
    return row;
}

// The stock client race (1-11) whose helmets a race wears: itself, or for a new race the stock race whose
// ChrRaces client prefix it shares (Void Elf "Be": Blood Elf helmet models, Blood Elf helmet hides). 0 if none.
uint32_t StockRaceFor(uint32_t raceId, uint32_t sexId) {
    // The stock prefixes, then the Retail prefixes of new races with their own helmets (CharacterComponent's
    // ComposeHelmModelFilePath tries the race's own helmet first and falls back to the stock race's).
    static const std::pair<const char*, uint32_t> kByPrefix[] = {{"Hu", 1}, {"Or", 2}, {"Dw", 3}, {"Ni", 4}, {"Sc", 5},
        {"Ta", 6}, {"Gn", 7}, {"Tr", 8}, {"Be", 10}, {"Dr", 11},
        {"Kt", 1}, {"Mo", 2}, {"Di", 3}, {"Ed", 3}, {"Hr", 4}, {"Ht", 6}, {"Mg", 7}, {"Za", 8}};
    if (raceId >= 1 && raceId <= 11 && raceId != 9) return raceId;
    // A race whose two bodies come from different stock races: the Dracthyr Visage ("Dv") is a Blood Elf
    // build as a male and a Human build as a female, and wears their helmets.
    static const struct { const char* prefix; uint32_t male, female; } kBySex[] = {{"Dv", 10, 1}};
    if (const ChrRacesRec* rec = g_chrRacesDB.GetRecord(raceId); rec && rec->m_clientPrefix)
        for (const auto& entry : kBySex)
            if (!_stricmp(rec->m_clientPrefix, entry.prefix)) return sexId ? entry.female : entry.male;
    if (const ChrRacesRec* rec = g_chrRacesDB.GetRecord(raceId); rec && rec->m_clientPrefix)
        for (const auto& entry : kByPrefix)
            if (!_stricmp(rec->m_clientPrefix, entry.first)) return entry.second;
    return 0;
}

bool RetailCharacterHelmFileExists(const char* mdxPath) {
    static std::map<std::string, bool> known;
    std::string path = mdxPath ? mdxPath : "";
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && !_stricmp(path.c_str() + dot, ".mdx")) path.replace(dot, std::string::npos, ".m2");
    const auto found = known.find(path);
    if (found != known.end()) return found->second;
    const bool exists = SFile::FileExists(path.c_str()) != 0;
    known.emplace(path, exists);
    return exists;
}

// Whether a helmet model is a new race's own (Helm_X_KtM or Helm_X_Kt_M for a race with a Retail prefix that falls
// back to a stock race): it was made for that body, so it sits on the socket as it is, with no 3.3.5 donor fit.
bool OwnRaceHelmet(uint32_t raceId, uint32_t sexId, const char* fileName) {
    const ChrRacesRec* rec = g_chrRacesDB.GetRecord(raceId);
    if (!rec || !rec->m_clientPrefix || !*rec->m_clientPrefix || !fileName || sexId > 1) return false;
    if (raceId >= 1 && raceId <= 11) return false;
    // Only races with a stock fallback (the Retail prefixes in StockRaceFor); the others (Vulpera "Vu", Pandaren
    // "Pa", ...) keep their helmets.csv tuning.
    if (!StockRaceFor(raceId, sexId)) return false;
    static const char* const kStockPrefixes[] = {"Hu", "Or", "Dw", "Ni", "Sc", "Ta", "Gn", "Tr", "Be", "Dr"};
    for (const char* stock : kStockPrefixes)
        if (!_stricmp(rec->m_clientPrefix, stock)) return false;
    std::string name = fileName;
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string prefix = rec->m_clientPrefix;
    for (char& c : prefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string sex = sexId ? "f" : "m";
    for (const std::string& tail : {"_" + prefix + sex + ".m2", "_" + prefix + "_" + sex + ".m2"})
        if (name.size() >= tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) return true;
    return false;
}

// The head and helmet socket of the 3.3.5 body a client race had (Character\<Race>\<Sex>\<Race><Sex>.m2),
// read once from the game's own archives; a new race uses its helmet race's body (StockRaceFor). Null when
// there is none or it cannot be read.
const Retail::HelmetAnchor* StockAnchor(uint32_t raceId, uint32_t sexId) {
    static const char* const kStock[] = {nullptr, "Human", "Orc", "Dwarf", "NightElf", "Scourge", "Tauren", "Gnome",
        "Troll", nullptr, "BloodElf", "Draenei"};
    const uint32_t stockRace = StockRaceFor(raceId, sexId);
    if (sexId > 1 || !stockRace || !kStock[stockRace]) return nullptr;
    const std::string race = kStock[stockRace], sex = sexId ? "Female" : "Male";
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
    return donor.valid ? &donor.anchor : nullptr;
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

    const auto& prefs = character->m_data.m_preferences;
    // A new race's own helmet was made for this body: it keeps the socket as it is.
    if (OwnRaceHelmet(prefs.m_raceID, prefs.m_sexID, helmet->m_shared->m_fileName)) return;
    const Retail::HelmetAnchor* donorAnchor = StockAnchor(prefs.m_raceID, prefs.m_sexID);
    if (!donorAnchor) {
        // A race with no 3.3.5 body to measure against (Pandaren, client prefix "Pa"): its helmets keep the
        // Retail socket, moved and scaled by hand from Data\Retail\helmets.csv when it has a row.
        const HelmetTuning* tuning = HelmetTuningFor(prefs.m_raceID, prefs.m_sexID);
        if (!tuning) return;
        const float scale = tuning->scale > 0 ? tuning->scale : 1.0f;
        C44Matrix& local = helmet->m_worldTransform;
        if ((helmet->m_fFlags & 0x8000) && local.a0 == scale && local.d0 == tuning->offset[0] && local.d1 == tuning->offset[1] &&
            local.d2 == tuning->offset[2]) return;
        local.Identity();
        local.a0 = scale;
        local.b1 = scale;
        local.c2 = scale;
        local.d0 = tuning->offset[0];
        local.d1 = tuning->offset[1];
        local.d2 = tuning->offset[2];
        helmet->m_fFlags |= 0x8000;
        static std::set<std::string> logged;
        if (logged.insert(std::string(body->m_shared->m_fileName)).second)
            Blizzard::Log::Printf(1, kLog, "Helmet fit on %s: no 3.3.5 body for race %u; helmets.csv scale %.3f, offset %.3f %.3f %.3f\n",
                body->m_shared->m_fileName, static_cast<unsigned>(prefs.m_raceID), scale, tuning->offset[0], tuning->offset[1], tuning->offset[2]);
        return;
    }
    struct { const Retail::HelmetAnchor& anchor; } donor{*donorAnchor};

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

// Whether a 3.3.5 foot texture covers most of the foot: at least half its pixels mostly opaque.
// Remembered per file.
bool FootTextureCovers(const char* path) {
    static std::map<std::string, bool> known;
    const auto found = known.find(path);
    if (found != known.end()) return found->second;
    bool covers = true;    // unreadable: keep the old behaviour (a named foot texture means a shoe)
    void* bytes = nullptr;
    uint32_t size = 0;
    if (SFile::LoadFile(path, &bytes, &size, 0, nullptr)) {
        const auto* begin = static_cast<const uint8_t*>(bytes);
        Retail::Image image;
        std::string error;
        if (Retail::DecodeBlp(std::vector<uint8_t>(begin, begin + size), image, error) && image.width && image.height) {
            size_t opaque = 0;
            const size_t pixels = size_t(image.width) * image.height;
            for (size_t i = 0; i < pixels; ++i)
                if (image.rgba[i * 4 + 3] >= 128) ++opaque;
            covers = opaque * 2 >= pixels;
        }
        SFile::Unload(bytes);
    }
    known.emplace(path, covers);
    return covers;
}

void RetailCharacterAfterStockGeometry(CCharacterComponent* character) {
    CM2Model* model = character->m_data.m_model;
    if (!model) return;
    s_helmetHides.erase(model);
    model->SetGeometryVisible(1701, 1799, 0);
    // The stock pass above shows its own eye glow (1703, from the 3.3.5 values); the Retail look's own
    // group 17 piece comes back: the Death Knight eye-glow wisps (1701), which the line above hid too.
    if (const auto look = RetailLook(character)) {
        for (const uint16_t geoset : look->geosets)
            if (geoset >= 1701 && geoset <= 1799) model->SetGeometryVisible(geoset, geoset, 1);
        ApplyEyeTint(model, *look);
    }
    ApplyCollections(model);

    // Feet. Retail bodies have a bare foot with toes (2001) and a closed shoe (2002) as separate meshes;
    // the 3.3.5 boot texture painted on the bare foot showed the toes. With boots that paint the foot, only
    // the shoe shows (Esteria's ApplyLegacyFootGeometry); boots that paint no foot (ankle wraps, leg bands)
    // leave the bare foot, which the shoe would have shown in plain skin. Hoofed races (Tauren, Draenei)
    // keep their feet. A race whose look picks its own feet (the Haranir paws, 2009, from the Feet option)
    // keeps them, boots or not: the shoe is the plain foot's covered form.
    const uint32_t race = character->m_data.m_preferences.m_raceID;
    uint16_t ownFeet = 0;
    if (const auto look = RetailLook(character))
        for (const uint16_t geoset : look->geosets)
            if (geoset > 2001 && geoset <= 2099) ownFeet = geoset;
    if (ownFeet) {
        model->SetGeometryVisible(2001, 2099, 0);
        model->SetGeometryVisible(ownFeet, ownFeet, 1);
    } else if (race != 6 && race != 11) {
        bool hasShoe = false;
        if ((model->m_fFlags & 1) && model->m_shared && model->m_shared->m_skinData) {
            const auto& sections = model->m_shared->m_skinData->m_skinSections;
            for (uint32_t i = 0; i < sections.m_count && !hasShoe; ++i)
                hasShoe = (sections[i].m_skinSectionId & 0xFFFF) == 2002;
        }
        bool paintsFoot = false;
        if (hasShoe && character->m_itemDisplayID[ITEMSLOT_BOOTS]) {
            ItemDisplayInfoRec boots;
            // A foot texture only counts when its file is there (as LoadItemComponentTexture finds it: the
            // unisex _U, else the wearer's sex) and it covers most of the foot. Wraps and sandals paint a
            // mostly see-through foot (the rogue's Thug Boots), and the shoe then showed in plain skin.
            if (g_itemDisplayInfoDB.GetRecord(character->m_itemDisplayID[ITEMSLOT_BOOTS], &boots) &&
                boots.m_texture[SECTION_FOOT] && *boots.m_texture[SECTION_FOOT]) {
                const char* sexes[] = {"U", character->m_data.m_preferences.m_sexID ? "F" : "M"};
                for (const char* sex : sexes) {
                    char path[260];
                    std::snprintf(path, sizeof(path), "Item\\TextureComponents\\FootTexture\\%s_%s.blp",
                        boots.m_texture[SECTION_FOOT], sex);
                    if (!SFile::FileExists(path)) continue;
                    paintsFoot = FootTextureCovers(path);
                    break;
                }
            }
        }
        const bool shoe = hasShoe && paintsFoot;
        model->SetGeometryVisible(2001, 2099, 0);
        model->SetGeometryVisible(shoe ? 2002 : 2001, shoe ? 2002 : 2001, 1);
    }

    // Group 14. No 3.3.5 item drives it, so the stock pass always shows 1401; most Retail bodies have nothing
    // there, but the Pandaren female's 1401 is her cloth flap with the Pandaren emblem, which then showed over
    // trousers and robes. It shows only with nothing worn on the legs, chest or tabard slot.
    if (character->m_itemDisplayID[ITEMSLOT_PANTS] || character->m_itemDisplayID[ITEMSLOT_CHEST] ||
        character->m_itemDisplayID[ITEMSLOT_TABARD])
        model->SetGeometryVisible(1401, 1499, 0);

    // Helmet hides. AddHelm writes them into the 3.3.5 selector values, which the Retail geometry does
    // not read, so the chosen hair and beard showed through. HelmetGeosetVisData slots 0-6 cover geoset
    // groups 0 (hair), 1-3 (facial hair), 7 (ears), 16 and 17, per race bit, as in the stock AddHelm. On
    // Retail bodies group 0's 1-99 are the hairstyles (0 itself is the scalp), so those go.
    // Every range hidden here is kept for the body's collections (ApplyCollection), which are applied again below.
    std::vector<std::pair<uint16_t, uint16_t>> hidden;
    const auto hide = [&](uint32_t from, uint32_t to) {
        model->SetGeometryVisible(from, to, 0);
        hidden.emplace_back(static_cast<uint16_t>(from), static_cast<uint16_t>(to));
    };
    struct Rebind {
        CM2Model* model; std::vector<std::pair<uint16_t, uint16_t>>& hidden;
        ~Rebind() { if (!hidden.empty()) { s_helmetHides[model] = hidden; ApplyCollections(model); } }
    } rebind{model, hidden};
    const auto& prefs = character->m_data.m_preferences;
    const uint32_t displayId = character->m_itemDisplayID[ITEMSLOT_HEAD];
    // The race whose HelmetGeosetVisData bit applies: a new race's stock race, an NPC copy's stock race (34-43),
    // a moved NPC race's old bit. A race past the 32-bit masks with none of those hides nothing.
    const uint32_t hideRace = RetailCharacterHelmetRace(character);
    if (!displayId || prefs.m_sexID > 1 || hideRace >= 32) return;
    CM2Model* helmet = model->m_attachList;
    while (helmet && helmet->m_attachmentId != ATTACH_HELM) helmet = helmet->m_attachNext;
    if (!helmet) return;    // hidden helm, barber shop
    ItemDisplayInfoRec display;
    if (!g_itemDisplayInfoDB.GetRecord(displayId, &display)) return;
    const auto* visibility = g_helmetGeosetVisDataDB.GetRecord(display.m_helmetGeosetVisID[prefs.m_sexID]);
    if (!visibility) return;
    static const uint32_t kGroups[7] = {0, 1, 2, 3, 7, 16, 17};
    const uint32_t raceBit = uint32_t{1} << hideRace;
    const auto hides = [&](uint32_t slot) { return (static_cast<uint32_t>(visibility->m_hideGeoset[slot]) & raceBit) != 0; };
    // Facial hair goes only under helmets that hide all of it (full-face helms). Hoods and half helms
    // that hid one piece of the small 3.3.5 facial hair would take a whole Retail mustache or sideburns.
    const bool wholeFace = hides(1) && hides(2) && hides(3);
    uint32_t retailRace = 0;
    RetailModelOf(character, retailRace);
    for (uint32_t slot = 0; slot < 7; ++slot) {
        if (!hides(slot) || (slot >= 1 && slot <= 3 && !wholeFace)) continue;
        if (slot == 2 && (retailRace == 35 || retailRace == 22)) continue;    // Vulpera and Worgen ears, below
        const uint32_t group = kGroups[slot];
        hide(group ? group * 100 : 1, group * 100 + 99);
    }
    // Retail head gear has no 3.3.5 slot: group 37 is the headdress or circlet on every race that has one
    // (Draenei Circlet, Tauren and Dwarf female Headdress). It goes with the hair, under the same helmets.
    if (hides(0)) hide(3701, 3799);
    // Earrings (group 35) hang from the ears: when a helmet hides the ears (slot 4, group 7) they go too,
    // instead of floating beside the head.
    if (hides(4)) hide(3501, 3599);
    // Some races keep hair, not facial hair, in group 2: the Tauren long Hair and the Void Elf Tentacles,
    // which hang from the scalp. They go with the hair rather than waiting for a full-face helmet.
    if (hides(0) && (retailRace == 6 || retailRace == 29))
        hide(201, 299);
    // The Lightforged forehead Rune (group 25) is a glowing mesh standing off the brow; 3.3.5 hats, made for
    // the old heads, do not cover it, so it shines through their crowns. It goes with the hair.
    if (hides(0) && retailRace == 30) hide(2501, 2599);
    // The Vulpera and Worgen Ears (group 2) are the ears, not facial hair: they go with the 3.3.5 ears slot only,
    // full-face helmets included, so each helmet's HelmetGeosetVisData ears bit decides whether they stick out.
    if (hides(4) && (retailRace == 35 || retailRace == 22))
        hide(201, 299);
    // The Pandaren Eyebrows (group 34) are tufts standing off the brow, above the head's front; hat brims
    // and helmet rims cut through them, so they go with the hair.
    if (hides(0) && retailRace >= 24 && retailRace <= 26) hide(3401, 3499);
}

#endif
