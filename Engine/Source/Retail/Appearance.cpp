#include "Engine/Source/Retail/Appearance.h"
#include "Engine/Source/Retail/Db2.h"
#include "Engine/Source/Retail/Tables.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace Retail::Appearance
{
namespace
{
using Db2::Table;
using Db2::wdc5::Row;

struct Index {
    std::once_flag once;
    bool ready = false;
    const Table* raceModel = nullptr;
    const Table* option = nullptr;
    const Table* choice = nullptr;
    const Table* element = nullptr;
    const Table* geoset = nullptr;
    const Table* skinnedModel = nullptr;
    const Table* requirement = nullptr;
    const Table* requirementChoice = nullptr;
    std::unordered_map<uint32_t, std::vector<uint32_t>> choicesByRequirement;
    const Table* chrModel = nullptr;
    const Table* material = nullptr;
    const Table* textureLayer = nullptr;
    const Table* layout = nullptr;
    const Table* section = nullptr;
    const Table* textureFile = nullptr;
    std::unordered_map<uint32_t, std::vector<const Row*>> layersByLayout;
    std::unordered_map<uint32_t, std::vector<const Row*>> sectionsByLayout;
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> filesByMaterial; // (usage, file)
    std::unordered_map<uint64_t, uint32_t> modelByRaceSex;
    std::unordered_map<uint32_t, std::vector<const Row*>> optionsByModel;
    std::unordered_map<uint32_t, std::vector<const Row*>> choicesByOption;
    std::unordered_map<uint32_t, std::vector<const Row*>> elementsByChoice;
};

constexpr uint64_t RaceSexKey(uint32_t race, uint32_t sex) {
    return (static_cast<uint64_t>(race) << 32) | sex;
}

// Ties on OrderIndex break on the row ID, so the order never changes between runs.
void SortByOrderIndex(std::unordered_map<uint32_t, std::vector<const Row*>>& index, const Table& table) {
    for (auto& entry : index) {
        std::sort(entry.second.begin(), entry.second.end(), [&table](const Row* a, const Row* b) {
            const uint32_t oa = table.Value(*a, "OrderIndex"), ob = table.Value(*b, "OrderIndex");
            return oa != ob ? oa < ob : a->id < b->id;
        });
    }
}

void Build(Index& x) {
    x.raceModel = Db2::tables::Get("chrracexchrmodel");
    x.option = Db2::tables::Get("chrcustomizationoption");
    x.choice = Db2::tables::Get("chrcustomizationchoice");
    x.element = Db2::tables::Get("chrcustomizationelement");
    x.geoset = Db2::tables::Get("chrcustomizationgeoset");
    x.skinnedModel = Db2::tables::Get("chrcustomizationskinnedmodel");
    x.chrModel = Db2::tables::Get("chrmodel");
    x.requirement = Db2::tables::Get("chrcustomizationreq");
    x.requirementChoice = Db2::tables::Get("chrcustomizationreqchoice");
    x.material = Db2::tables::Get("chrcustomizationmaterial");
    x.textureLayer = Db2::tables::Get("chrmodeltexturelayer");
    x.layout = Db2::tables::Get("charcomponenttexturelayouts");
    x.section = Db2::tables::Get("charcomponenttexturesections");
    x.textureFile = Db2::tables::Get("texturefiledata");
    if (!x.raceModel || !x.option || !x.choice || !x.element || !x.geoset) return;
    if (x.requirementChoice)
        for (const Row& row : x.requirementChoice->Rows())
            x.choicesByRequirement[row.parentId].push_back(x.requirementChoice->Value(row, "ChrCustomizationChoiceID"));
    if (x.textureLayer)
        for (const Row& row : x.textureLayer->Rows()) x.layersByLayout[row.parentId].push_back(&row);
    if (x.section)
        for (const Row& row : x.section->Rows()) {
            const uint32_t layout = x.section->Value(row, "CharComponentTextureLayoutID");
            x.sectionsByLayout[layout ? layout : row.parentId].push_back(&row);
        }
    if (x.textureFile)
        for (const Row& row : x.textureFile->Rows())
            x.filesByMaterial[x.textureFile->Value(row, "MaterialResourcesID")].push_back(
                {x.textureFile->Value(row, "UsageType"), row.id});

    for (const Row& row : x.raceModel->Rows())
        x.modelByRaceSex[RaceSexKey(x.raceModel->Value(row, "ChrRacesID"), x.raceModel->Value(row, "Sex"))] =
            x.raceModel->Value(row, "ChrModelID");
    for (const Row& row : x.option->Rows())
        x.optionsByModel[x.option->Value(row, "ChrModelID")].push_back(&row);
    for (const Row& row : x.choice->Rows())
        x.choicesByOption[x.choice->Value(row, "ChrCustomizationOptionID")].push_back(&row);
    for (const Row& row : x.element->Rows())
        x.elementsByChoice[x.element->Value(row, "ChrCustomizationChoiceID")].push_back(&row);
    SortByOrderIndex(x.optionsByModel, *x.option);
    SortByOrderIndex(x.choicesByOption, *x.choice);
    x.ready = true;
}

Index& Ready() {
    static Index index;
    std::call_once(index.once, [] { Build(index); });
    return index;
}

// Whether a character of the class may take the choice: its ChrCustomizationReq must be a player
// requirement (NPC-only and Transmog choices are left out) whose class mask includes the class.
// Achievement, quest and item unlocks are not checked. Without the requirement table only the Transmog
// placeholders are skipped (Esteria's CreationPolicy.h).
bool CreationAllowed(const Index& x, const Row& choice, uint32_t classId) {
    const uint32_t id = x.choice->Value(choice, "ChrCustomizationReqID");
    if (id == 10 || x.choice->Text(choice, "Name_lang") == "Transmog") return false;
    if (!id || !x.requirement) return true;
    const Row* requirement = x.requirement->Find(id);
    if (!requirement) return true;
    if (x.requirement->Value(*requirement, "ReqType") != 3) return false;
    const uint32_t classes = x.requirement->Value(*requirement, "ClassMask");
    if (!classes || classes == 0xFFFFFFFFu || !classId || classId > 32) return true;
    // FlashWOW has no Demon Hunter class (12): its choices (horns, blindfolds, fel eyes, the extra
    // skins and tattoos) are open to every class, on top of the class's own.
    constexpr uint32_t kDemonHunter = 1u << (12 - 1);
    return ((classes >> (classId - 1)) & 1) || (classes & kDemonHunter);
}
}

// Whether a choice is offered to Demon Hunters only (its requirement's class mask is class 12 alone).
static bool DemonHunterOnly(const Index& x, uint32_t choiceId) {
    const Row* choice = x.choice->Find(choiceId);
    const uint32_t id = choice ? x.choice->Value(*choice, "ChrCustomizationReqID") : 0;
    const Row* requirement = id && x.requirement ? x.requirement->Find(id) : nullptr;
    return requirement && x.requirement->Value(*requirement, "ClassMask") == (1u << (12 - 1));
}

// Whether a choice belongs to an Eye Color option.
static bool IsEyeColor(const Index& x, uint32_t choiceId) {
    const Row* choice = x.choice->Find(choiceId);
    const Row* option = choice ? x.option->Find(x.choice->Value(*choice, "ChrCustomizationOptionID")) : nullptr;
    return option && x.option->Text(*option, "Name_lang") == "Eye Color";
}

uint32_t ChrModelFor(uint32_t retailRace, uint32_t sex) {
    Index& x = Ready();
    if (!x.ready) return 0;
    const auto found = x.modelByRaceSex.find(RaceSexKey(retailRace, sex));
    return found == x.modelByRaceSex.end() ? 0 : found->second;
}

void Options(uint32_t chrModel, uint32_t classId, std::vector<Option>& options) {
    options.clear();
    Index& x = Ready();
    if (!x.ready) return;
    const auto found = x.optionsByModel.find(chrModel);
    if (found == x.optionsByModel.end()) return;
    for (const Row* row : found->second) {
        Option option;
        option.id = row->id;
        option.name = x.option->Text(*row, "Name_lang");
        const auto list = x.choicesByOption.find(row->id);
        if (list != x.choicesByOption.end())
            for (const Row* choice : list->second)
                if (CreationAllowed(x, *choice, classId)) option.choices.push_back(choice->id);
        if (!option.choices.empty()) options.push_back(std::move(option));
    }
}

bool RequirementsMet(uint32_t choiceId, const std::vector<uint32_t>& selected) {
    Index& x = Ready();
    if (!x.ready) return true;
    const Row* choice = x.choice->Find(choiceId);
    const uint32_t requirement = choice ? x.choice->Value(*choice, "ChrCustomizationReqID") : 0;
    const auto needed = requirement ? x.choicesByRequirement.find(requirement) : x.choicesByRequirement.end();
    if (needed == x.choicesByRequirement.end()) return true;
    for (const uint32_t id : needed->second)
        if (std::find(selected.begin(), selected.end(), id) != selected.end()) return true;
    return false;
}

void DefaultChoices(uint32_t chrModel, uint32_t classId, std::vector<uint32_t>& choices) {
    choices.clear();
    std::vector<Option> options;
    Options(chrModel, classId, options);
    for (const Option& option : options) choices.push_back(option.choices.front());
}

void Geosets(uint32_t chrModel, const std::vector<uint32_t>& choices, std::vector<uint16_t>& geosets) {
    geosets.clear();
    Index& x = Ready();
    if (!x.ready) return;
    for (const uint32_t id : choices) {
        const Row* choice = x.choice->Find(id);
        const Row* option = choice ? x.option->Find(x.choice->Value(*choice, "ChrCustomizationOptionID")) : nullptr;
        if (!option || x.option->Value(*option, "ChrModelID") != chrModel) continue;
        const auto elements = x.elementsByChoice.find(id);
        if (elements == x.elementsByChoice.end()) continue;
        for (const Row* element : elements->second) {
            // A hair style lists one element per hair colour, each waiting for its colour.
            const uint32_t needs = x.element->Value(*element, "RelatedChrCustomizationChoiceID");
            if (needs && std::find(choices.begin(), choices.end(), needs) == choices.end()) continue;
            const uint32_t geosetId = x.element->Value(*element, "ChrCustomizationGeosetID");
            const Row* geoset = geosetId ? x.geoset->Find(geosetId) : nullptr;
            if (!geoset) continue;
            const uint32_t value = x.geoset->Value(*geoset, "GeosetType") * 100u + x.geoset->Value(*geoset, "GeosetID");
            if (value <= 0xFFFF) geosets.push_back(static_cast<uint16_t>(value));
        }
    }
}

void SkinnedModels(uint32_t chrModel, const std::vector<uint32_t>& choices, std::vector<SkinnedModel>& parts) {
    parts.clear();
    Index& x = Ready();
    if (!x.ready || !x.skinnedModel) return;
    for (const uint32_t id : choices) {
        const Row* choice = x.choice->Find(id);
        const Row* option = choice ? x.option->Find(x.choice->Value(*choice, "ChrCustomizationOptionID")) : nullptr;
        if (!option || x.option->Value(*option, "ChrModelID") != chrModel) continue;
        const auto elements = x.elementsByChoice.find(id);
        if (elements == x.elementsByChoice.end()) continue;
        for (const Row* element : elements->second) {
            const uint32_t needs = x.element->Value(*element, "RelatedChrCustomizationChoiceID");
            if (needs && std::find(choices.begin(), choices.end(), needs) == choices.end()) continue;
            const uint32_t partId = x.element->Value(*element, "ChrCustomizationSkinnedModelID");
            const Row* part = partId ? x.skinnedModel->Find(partId) : nullptr;
            if (!part) continue;
            SkinnedModel model;
            model.choice = id;
            model.fileDataId = x.skinnedModel->Value(*part, "CollectionsFileDataID");
            model.geoset = x.skinnedModel->Value(*part, "GeosetType") * 100u + x.skinnedModel->Value(*part, "GeosetID");
            model.flags = x.skinnedModel->Value(*part, "Flags");
            if (model.fileDataId) parts.push_back(model);
        }
    }
}

uint32_t LayoutFor(uint32_t chrModel) {
    Index& x = Ready();
    const Row* row = x.ready && x.chrModel ? x.chrModel->Find(chrModel) : nullptr;
    return row ? x.chrModel->Value(*row, "CharComponentTextureLayoutID") : 0;
}

bool LayoutSize(uint32_t layout, uint32_t& width, uint32_t& height) {
    Index& x = Ready();
    const Row* row = x.ready && x.layout ? x.layout->Find(layout) : nullptr;
    if (!row) return false;
    width = x.layout->Value(*row, "Width");
    height = x.layout->Value(*row, "Height");
    return width && height;
}

void Sections(uint32_t layout, std::vector<TextureSection>& sections) {
    sections.clear();
    Index& x = Ready();
    if (!x.ready || !x.section) return;
    const auto found = x.sectionsByLayout.find(layout);
    if (found == x.sectionsByLayout.end()) return;
    for (const Row* row : found->second)
        sections.push_back({x.section->Value(*row, "SectionType"), x.section->Value(*row, "X"), x.section->Value(*row, "Y"),
            x.section->Value(*row, "Width"), x.section->Value(*row, "Height")});
}

namespace {
// TextureFileData lists a material's files by usage; usage 0 is the colour map (Esteria falls back to 2).
uint32_t MaterialFile(Index& x, uint32_t material) {
    const auto found = x.filesByMaterial.find(material);
    if (found == x.filesByMaterial.end() || found->second.empty()) return 0;
    for (const uint32_t wanted : {0u, 2u})
        for (const auto& file : found->second)
            if (file.first == wanted) return file.second;
    return found->second.front().second;
}
}

void TextureLayers(uint32_t chrModel, const std::vector<uint32_t>& choices, std::vector<TextureLayer>& layers) {
    layers.clear();
    Index& x = Ready();
    if (!x.ready || !x.material || !x.textureLayer) return;
    const uint32_t layout = LayoutFor(chrModel);
    const auto layoutLayers = x.layersByLayout.find(layout);
    if (layoutLayers == x.layersByLayout.end()) return;
    for (const uint32_t id : choices) {
        const Row* choice = x.choice->Find(id);
        const Row* option = choice ? x.option->Find(x.choice->Value(*choice, "ChrCustomizationOptionID")) : nullptr;
        if (!option || x.option->Value(*option, "ChrModelID") != chrModel) continue;
        const auto elements = x.elementsByChoice.find(id);
        if (elements == x.elementsByChoice.end()) continue;
        const auto paint = [&](const Row* material) {
            const uint32_t target = x.material->Value(*material, "ChrModelTextureTargetID");
            const uint32_t file = MaterialFile(x, x.material->Value(*material, "MaterialResourcesID"));
            for (const Row* layer : layoutLayers->second) {
                if (x.textureLayer->Value(*layer, "ChrModelTextureTargetID") != target) continue;
                layers.push_back({x.textureLayer->Value(*layer, "TextureType"), x.textureLayer->Value(*layer, "Layer"),
                    x.textureLayer->Value(*layer, "BlendMode"), x.textureLayer->Value(*layer, "TextureSectionTypeBitMask"), file});
            }
        };
        // A material element may wait for a related choice (a face's texture per skin colour, a tattoo
        // colour per tattoo). Demon Hunter blindfolds are textured only alongside the Demon Hunter fel
        // eye colours, which every Demon Hunter has; with Demon Hunter choices open to every class
        // (CreationAllowed) a blindfold can come with ordinary eyes and would draw untextured. So a
        // visible Demon Hunter-only choice whose texture waits only on a Demon Hunter-only Eye Color
        // takes its first such element. Everything else waiting (faces, tattoo colours, the hidden
        // fel-eye effect options) is skipped as before.
        const Row* ownOption = x.option->Find(x.choice->Value(*choice, "ChrCustomizationOptionID"));
        const bool impliesFelEyes = DemonHunterOnly(x, id) && ownOption && !x.option->Text(*ownOption, "Name_lang").empty();
        std::vector<uint32_t> painted;
        std::vector<std::pair<uint32_t, const Row*>> waiting;    // target, first material for it
        for (const Row* element : elements->second) {
            const uint32_t materialId = x.element->Value(*element, "ChrCustomizationMaterialID");
            const Row* material = materialId ? x.material->Find(materialId) : nullptr;
            if (!material) continue;
            const uint32_t target = x.material->Value(*material, "ChrModelTextureTargetID");
            const uint32_t needs = x.element->Value(*element, "RelatedChrCustomizationChoiceID");
            if (needs && std::find(choices.begin(), choices.end(), needs) == choices.end()) {
                if (impliesFelEyes && DemonHunterOnly(x, needs) && IsEyeColor(x, needs) &&
                    std::find_if(waiting.begin(), waiting.end(), [target](const auto& w) { return w.first == target; }) == waiting.end())
                    waiting.emplace_back(target, material);
                continue;
            }
            painted.push_back(target);
            paint(material);
        }
        for (const auto& w : waiting)
            if (std::find(painted.begin(), painted.end(), w.first) == painted.end()) paint(w.second);
    }
    std::stable_sort(layers.begin(), layers.end(), [](const TextureLayer& a, const TextureLayer& b) { return a.layer < b.layer; });
}

void DefaultGeosets(const std::vector<uint16_t>& chosen, std::vector<uint16_t>& defaults) {
    defaults.clear();
    for (const uint16_t base : {2001, 2201, 2301, 3202, 3301, 5101}) {
        bool picked = false;
        for (const uint16_t geoset : chosen) picked = picked || geoset / 100 == base / 100;
        if (!picked) defaults.push_back(base);
    }
}
}
