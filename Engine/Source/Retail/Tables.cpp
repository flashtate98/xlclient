// Retail DB2 table declarations, from the Esteria fork of wowdecomp (Engine/Source/Retail/Appearance.cpp),
// derived from WarcraftXL wxl-db2 (GPL-3.0-or-later). The layout hashes are those of retail build
// 12.1.5 (WOWSTATIC_12_1_5_70077); a table from another build is refused instead of misread.
#include "Engine/Source/Retail/Tables.h"
#include "Engine/Source/Retail/Db2.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include <chrono>
#include <mutex>

namespace Retail::Db2::tables
{
namespace
{
constexpr const char* kLog = "Logs/RetailTables.log";

    constexpr Field kChrRaceXChrModelFields[] = {
        { "ChrRacesID" }, { "ChrModelID" }, { "Sex" }, { "AllowedTransmogSlots" },
    };
    constexpr Definition kChrRaceXChrModel{
        "chrracexchrmodel", "DBFilesClient\\chrracexchrmodel.db2", 0xA203BC29u,
        kChrRaceXChrModelFields,
    };

    constexpr Field kChrModelFields[] = {
        { "FaceCustomizationOffset", 3 }, { "CustomizeOffset", 3 }, { "ID" }, { "Sex" },
        { "DisplayID" }, { "CharComponentTextureLayoutID" }, { "Flags" },
        { "SkeletonFileDataID" }, { "ModelFallbackChrModelID" }, { "TextureFallbackChrModelID" },
        { "HelmVisFallbackChrModelID" }, { "CustomizeScale" }, { "CustomizeFacing" },
        { "CameraDistanceOffset" }, { "BarberShopCameraOffsetScale" },
        { "BarberShopCameraRotationFacing" }, { "BarberShopCameraRotationOffset" },
    };
    constexpr Definition kChrModel{
        "chrmodel", "DBFilesClient\\chrmodel.db2", 0x03FAB755u, kChrModelFields,
    };

    constexpr Field kChrCustomizationOptionFields[] = {
        { "Name_lang", 1, true }, { "ID" }, { "SecondaryID" }, { "Flags" }, { "ChrModelID" },
        { "OrderIndex" }, { "ChrCustomizationCategoryID" }, { "OptionType" },
        { "BarberShopCostModifier" }, { "ChrCustomizationID" }, { "Requirement" },
        { "SecondaryOrderIndex" }, { "AddedInPatch" },
    };
    constexpr Definition kChrCustomizationOption{
        "chrcustomizationoption", "DBFilesClient\\chrcustomizationoption.db2", 0xDCC2A86Eu,
        kChrCustomizationOptionFields,
    };

    constexpr Field kChrCustomizationChoiceFields[] = {
        { "Name_lang", 1, true }, { "ID" }, { "ChrCustomizationOptionID" }, { "ChrCustomizationReqID" },
        { "ChrCustomizationVisReqID" }, { "OrderIndex" }, { "UiOrderIndex" }, { "Flags" },
        { "AddedInPatch" }, { "SoundKitID" }, { "SwatchColor", 2 },
    };
    constexpr Definition kChrCustomizationChoice{
        "chrcustomizationchoice", "DBFilesClient\\chrcustomizationchoice.db2", 0x9559C358u,
        kChrCustomizationChoiceFields,
    };

    constexpr Field kChrCustomizationElementFields[] = {
        { "ChrCustomizationChoiceID" }, { "RelatedChrCustomizationChoiceID" },
        { "ChrCustomizationGeosetID" }, { "ChrCustomizationSkinnedModelID" },
        { "ChrCustomizationMaterialID" }, { "ChrCustomizationBoneSetID" },
        { "ChrCustomizationCondModelID" }, { "ChrCustomizationDisplayInfoID" },
        { "ChrCustItemGeoModifyID" }, { "ChrCustomizationVoiceID" }, { "AnimKitID" },
        { "ParticleColorID" }, { "ChrCustGeoComponentLinkID" },
    };
    constexpr Definition kChrCustomizationElement{
        "chrcustomizationelement", "DBFilesClient\\chrcustomizationelement.db2", 0x6483C37Eu,
        kChrCustomizationElementFields,
    };

    constexpr Field kChrCustomizationGeosetFields[] = {
        { "GeosetType" }, { "GeosetID" }, { "Modifier" },
    };
    constexpr Definition kChrCustomizationGeoset{
        "chrcustomizationgeoset", "DBFilesClient\\chrcustomizationgeoset.db2", 0xBF55F2FBu,
        kChrCustomizationGeosetFields,
    };

    constexpr Field kChrCustomizationSkinnedModelFields[] = {
        { "CollectionsFileDataID" }, { "GeosetType" }, { "GeosetID" }, { "Modifier" }, { "Flags" },
    };
    constexpr Definition kChrCustomizationSkinnedModel{
        "chrcustomizationskinnedmodel", "DBFilesClient\\chrcustomizationskinnedmodel.db2",
        0x4C32AA8Au, kChrCustomizationSkinnedModelFields,
    };

    constexpr Field kChrCustomizationMaterialFields[] = {
        { "ChrModelTextureTargetID" }, { "MaterialResourcesID" },
    };
    constexpr Definition kChrCustomizationMaterial{
        "chrcustomizationmaterial", "DBFilesClient\\chrcustomizationmaterial.db2", 0xBE9767E9u,
        kChrCustomizationMaterialFields,
    };

    constexpr Field kChrCustomizationCondModelFields[] = {
        { "CreatureModelDataID" }, { "Field_9_0_1_34081_001", 4 },
    };
    constexpr Definition kChrCustomizationCondModel{
        "chrcustomizationcondmodel", "DBFilesClient\\chrcustomizationcondmodel.db2",
        0x601A4522u, kChrCustomizationCondModelFields,
    };

    // Declared because kChrModelTextureLayer names it as a relation target, and a target that is
    // not declared fails validation for the WHOLE set: one missing table is the difference
    // between every appearance resolving and none of them resolving.
    constexpr Field kCharComponentTextureLayoutsFields[] = {
        { "Width" }, { "Height" },
    };
    constexpr Definition kCharComponentTextureLayouts{
        "charcomponenttexturelayouts", "DBFilesClient\\charcomponenttexturelayouts.db2",
        0x0B9AF134u, kCharComponentTextureLayoutsFields,
    };

    constexpr Field kChrModelTextureLayerFields[] = {
        { "TextureType" }, { "Layer" }, { "Flags" }, { "BlendMode" },
        { "TextureSectionTypeBitMask" }, { "TextureSectionTypeBitMask2" },
        { "Field_9_0_1_34365_006", 3 }, { "ChrModelTextureTargetID", 2 },
    };
    constexpr Relation kChrModelTextureLayerRelations[] = {
        { "CharComponentTextureLayoutsID", RelationSource::ParentId, {}, 0,
          "charcomponenttexturelayouts" },
    };
    constexpr Definition kChrModelTextureLayer{
        "chrmodeltexturelayer", "DBFilesClient\\chrmodeltexturelayer.db2", 0xD0583FB4u,
        kChrModelTextureLayerFields, kChrModelTextureLayerRelations,
    };

    constexpr Field kCreatureDisplayInfoFields[] = {
        { "ID" }, { "ModelID" }, { "SoundID" }, { "SizeClass" }, { "CreatureModelScale" },
        { "CreatureModelAlpha" }, { "BloodID" }, { "ExtendedDisplayInfoID" }, { "NPCSoundID" },
        { "ParticleColorID" }, { "PortraitCreatureDisplayInfoID" },
        { "PortraitTextureFileDataID" }, { "ObjectEffectPackageID" }, { "AnimReplacementSetID" },
        { "Flags" }, { "StateSpellVisualKitID" }, { "PlayerOverrideScale" },
        { "PetInstanceScale" }, { "UnarmedWeaponType" }, { "MountPoofSpellVisualKitID" },
        { "DissolveEffectID" }, { "Gender" }, { "DissolveOutEffectID" },
        { "CreatureModelMinLod" }, { "ConditionalCreatureModelID" }, { "MountMaxBankingAngle" },
        { "Field_11_0_0_54210_026" }, { "TextureVariationFileDataID", 4 },
    };
    constexpr Definition kCreatureDisplayInfo{
        "creaturedisplayinfo", "DBFilesClient\\creaturedisplayinfo.db2", 0x7275F5F6u,
        kCreatureDisplayInfoFields,
    };

    constexpr Field kCreatureModelDataFields[] = {
        { "GeoBox", 6 }, { "Flags" }, { "FileDataID" }, { "WalkSpeed" }, { "RunSpeed" },
        { "BloodID" }, { "FootprintTextureID" }, { "FootprintTextureLength" },
        { "FootprintTextureWidth" }, { "FootprintParticleScale" }, { "FoleyMaterialID" },
        { "FootstepCameraEffectID" }, { "DeathThudCameraEffectID" }, { "SoundID" },
        { "SizeClass" }, { "CollisionWidth" }, { "CollisionHeight" }, { "WorldEffectScale" },
        { "CreatureGeosetDataID" }, { "HoverHeight" }, { "AttachedEffectScale" },
        { "ModelScale" }, { "MissileCollisionRadius" }, { "MissileCollisionPush" },
        { "MissileCollisionRaise" }, { "MountHeight" }, { "OverrideLootEffectScale" },
        { "OverrideNameScale" }, { "OverrideSelectionRadius" }, { "TamedPetBaseScale" },
        { "MountScaleOtherIndex" }, { "MountScaleSelf" }, { "Field_11_0_0_54210_032" },
        { "MountScaleOther", 2 },
    };
    constexpr Definition kCreatureModelData{
        "creaturemodeldata", "DBFilesClient\\creaturemodeldata.db2", 0x0F5449F8u,
        kCreatureModelDataFields,
    };

    constexpr Field kCreatureDisplayInfoGeosetDataFields[] = {
        { "GeosetIndex" }, { "GeosetValue" },
    };
    constexpr Relation kCreatureDisplayInfoGeosetDataRelations[] = {
        { "CreatureDisplayInfoID", RelationSource::ParentId, {}, 0, "creaturedisplayinfo" },
    };
    constexpr Definition kCreatureDisplayInfoGeosetData{
        "creaturedisplayinfogeosetdata", "DBFilesClient\\creaturedisplayinfogeosetdata.db2",
        0x5E539080u, kCreatureDisplayInfoGeosetDataFields, kCreatureDisplayInfoGeosetDataRelations,
    };

    // Where each piece of a composited character sheet goes. The column order is not guessed: the
    // file's own values name them. Field 0 repeats the layout id the relationship column already
    // carries, field 1 runs 0..14 across every layout, and the four that follow tile each layout
    // exactly -- layout 1's rows cover 1024x1024 with no gap and no overlap, which is the size
    // CharComponentTextureLayouts states for it.
    constexpr Field kCharComponentTextureSectionsFields[] = {
        { "CharComponentTextureLayoutID" }, { "SectionType" },
        { "X" }, { "Y" }, { "Width" }, { "Height" }, { "OverlapSectionMask" },
    };
    constexpr Relation kCharComponentTextureSectionsRelations[] = {
        { "LayoutID", RelationSource::ParentId, {}, 0, "charcomponenttexturelayouts" },
    };
    constexpr Definition kCharComponentTextureSections{
        "charcomponenttexturesections", "DBFilesClient\\charcomponenttexturesections.db2",
        0x2173BA71u, kCharComponentTextureSectionsFields, kCharComponentTextureSectionsRelations,
    };

    constexpr Field kTextureFileDataFields[] = {
        { "ID" }, { "UsageType" }, { "MaterialResourcesID" },
    };
    constexpr Definition kTextureFileData{
        "texturefiledata", "DBFilesClient\\texturefiledata.db2", 0xBD7C74C2u, kTextureFileDataFields,
    };

constexpr Definition kDefinitions[] = {
    kChrRaceXChrModel, kChrModel, kChrCustomizationOption, kChrCustomizationChoice,
    kChrCustomizationElement, kChrCustomizationGeoset, kChrCustomizationSkinnedModel,
    kChrCustomizationMaterial, kChrCustomizationCondModel, kCharComponentTextureLayouts,
    kChrModelTextureLayer, kCreatureDisplayInfo, kCreatureModelData, kCreatureDisplayInfoGeosetData,
    kCharComponentTextureSections, kTextureFileData,
};
constexpr size_t kCount = sizeof(kDefinitions) / sizeof(kDefinitions[0]);

struct Store {
    std::once_flag once;
    Table tables[kCount];
    bool loaded[kCount] = {};
    std::string errors[kCount];
    uint32_t milliseconds[kCount] = {};
    Status status;
};

Store& State() {
    static Store store;
    return store;
}

void Load(Store& s) {
    std::string error;
    if (!ValidateDefinitions(kDefinitions, &error)) {
        s.status.error = error;
        Blizzard::Log::Printf(1, kLog, "Table declarations are inconsistent: %s\n", error.c_str());
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kCount; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        s.loaded[i] = s.tables[i].Load(kDefinitions[i], &s.errors[i]);
        s.milliseconds[i] = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count());
        if (s.loaded[i]) {
            ++s.status.tables;
            Blizzard::Log::Printf(1, kLog, "%-30s %7u rows  %5u ms\n", std::string(kDefinitions[i].name).c_str(),
                static_cast<unsigned>(s.tables[i].Rows().size()), static_cast<unsigned>(s.milliseconds[i]));
        } else {
            ++s.status.failed;
            if (s.status.error.empty()) s.status.error = s.errors[i];
            Blizzard::Log::Printf(1, kLog, "%-30s FAILED: %s\n", std::string(kDefinitions[i].name).c_str(), s.errors[i].c_str());
        }
    }
    s.status.milliseconds = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count());
    s.status.loaded = s.status.failed == 0;
    Blizzard::Log::Printf(1, kLog, "%u of %u tables loaded in %u ms\n", static_cast<unsigned>(s.status.tables),
        static_cast<unsigned>(kCount), static_cast<unsigned>(s.status.milliseconds));
}

Store& Loaded() {
    Store& s = State();
    std::call_once(s.once, [&s] { Load(s); });
    return s;
}
}

const Status& GetStatus() {
    return Loaded().status;
}

const Table* Get(std::string_view name) {
    Store& s = Loaded();
    for (size_t i = 0; i < kCount; ++i)
        if (kDefinitions[i].name == name) return s.loaded[i] ? &s.tables[i] : nullptr;
    return nullptr;
}

int32_t CCommand_RetailTables(const char*, const char*) {
    Store& s = Loaded();
    for (size_t i = 0; i < kCount; ++i) {
        const std::string name(kDefinitions[i].name);
        if (s.loaded[i])
            ConsolePrintf("%s: %u rows (%u ms)", name.c_str(), static_cast<unsigned>(s.tables[i].Rows().size()), static_cast<unsigned>(s.milliseconds[i]));
        else
            ConsolePrintf("%s: FAILED: %s", name.c_str(), s.errors[i].c_str());
    }
    ConsolePrintf("%u of %u Retail tables loaded in %u ms (Logs\\RetailTables.log)", static_cast<unsigned>(s.status.tables),
        static_cast<unsigned>(kCount), static_cast<unsigned>(s.status.milliseconds));
    return 1;
}
}
