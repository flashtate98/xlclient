#if defined(WOWDECOMP_RETAIL_MODELS)

#include "WoW/Common/ComponentCore/RetailCharacter.h"
#include "WoW/Common/ComponentCore/CharacterComponent.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Retail/Appearance.h"
#include "Engine/Source/Retail/LocalSource.h"
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
    std::vector<uint16_t> geosets;   // chosen, then the anatomy defaults
};

// The default look per Retail race and sex; logged the first time it is built.
const Look& LookFor(uint32_t retailRace, uint32_t sex) {
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
    if (look.chrModel) {
        Retail::Appearance::DefaultChoices(look.chrModel, choices);
        Retail::Appearance::Geosets(look.chrModel, choices, look.geosets);
        Retail::Appearance::DefaultGeosets(look.geosets, defaults);
        look.geosets.insert(look.geosets.end(), defaults.begin(), defaults.end());
    }

    std::string text;
    for (const uint32_t id : choices) text += " " + std::to_string(id);
    text += " | geosets";
    for (const uint16_t id : look.geosets) text += " " + std::to_string(id);
    Blizzard::Log::Printf(1, kLog, "Retail race %u sex %u: ChrModel %u, default choices%s\n",
        static_cast<unsigned>(retailRace), static_cast<unsigned>(sex), static_cast<unsigned>(look.chrModel), text.c_str());
    return look;
}

}

bool RetailCharacterGeometry(CCharacterComponent* character) {
    CM2Model* model = character->m_data.m_model;
    if (!model || !model->m_shared || !Retail::Source().Handles(model->m_shared->m_fileName)) return false;

    const auto& map = RaceMap();
    const auto race = map.find(character->m_data.m_preferences.m_raceID);
    if (race == map.end()) return false;
    const Look& look = LookFor(race->second, character->m_data.m_preferences.m_sexID);
    if (!look.chrModel) return false;

    // The stock reset only reaches geoset 2000; Retail bodies carry groups above it (feet 20,
    // torso 22, hands 23, customization 24-51), which would otherwise all draw at once.
    model->SetGeometryVisible(2001, 9999, 0);

    for (const uint16_t geoset : look.geosets) {
        model->SetGeometryVisible(geoset, geoset, 1);
        // The stock pass shows one geoset per group below 19 from m_geosets; a Retail choice replaces
        // the 3.3.5 hair and facial-hair pick for its group.
        if (geoset / 100 < 19) character->m_data.m_geosets[geoset / 100] = geoset;
    }
    return true;
}

#endif
