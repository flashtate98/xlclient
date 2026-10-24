#ifndef ENGINE_SOURCE_RETAIL_APPEARANCE_H
#define ENGINE_SOURCE_RETAIL_APPEARANCE_H

#include <cstdint>
#include <string>
#include <vector>

// Retail character appearance from the customization tables (Tables.h). Geoset selection follows the
// Esteria fork's resolver (Engine/Source/Retail/Appearance.cpp there): a choice expands to its
// ChrCustomizationElement rows, an element that names a related choice applies only when that
// choice is taken too, and a ChrCustomizationGeoset row becomes geoset type * 100 + id.
namespace Retail::Appearance
{
    // The ChrModel for a Retail ChrRaces ID and sex (ChrRaceXChrModel), or 0.
    uint32_t ChrModelFor(uint32_t retailRace, uint32_t sex);

    // A customization option of a model and the choices a new character of the class may take, in
    // OrderIndex order: NPC-only choices, Transmog placeholders and choices whose ChrCustomizationReq
    // names other classes are left out (class 0: any class). Options without such choices are left out.
    struct Option {
        uint32_t id = 0;
        std::string name;              // "Skin Color", "Hair Style", ...
        std::vector<uint32_t> choices;
    };
    void Options(uint32_t chrModel, uint32_t classId, std::vector<Option>& options);

    // Whether the choice's requirement is met by the other choices taken: some choices (the newer Human
    // skins, for instance) are offered only alongside particular choices of another option
    // (ChrCustomizationReqChoice). True when the choice asks for none.
    bool RequirementsMet(uint32_t choiceId, const std::vector<uint32_t>& selected);

    // One choice per option of the model: the first that a new character of the class may take.
    void DefaultChoices(uint32_t chrModel, uint32_t classId, std::vector<uint32_t>& choices);

    // The geosets the choices select on the model, in choice order. Choices of another model are
    // ignored.
    void Geosets(uint32_t chrModel, const std::vector<uint32_t>& choices, std::vector<uint16_t>& geosets);

    // Anatomy that no customization choice of a race names, shown unless a choice picks that group:
    // feet (20), torso (22), hands (23), head (32), eyes (33) and eye overlays (51). From Esteria's
    // RaceGeometry.h.
    void DefaultGeosets(const std::vector<uint16_t>& chosen, std::vector<uint16_t>& defaults);

    // One layer of a composed texture: ChrModelTextureLayer matched to a chosen material.
    struct TextureLayer {
        uint32_t textureType = 0;     // the M2 texture type it paints (1 body skin, 6 hair, ...)
        uint32_t layer = 0;           // paint order
        uint32_t blendMode = 0;       // the character compositor's blend enum
        uint32_t sectionMask = 0;     // body sections it covers (0xFFFFFFFF: the whole texture)
        uint32_t fileDataId = 0;      // the BLP, from TextureFileData
    };
    struct TextureSection {
        uint32_t sectionType = 0, x = 0, y = 0, width = 0, height = 0;
    };

    // The model's CharComponentTextureLayouts row, its size and its sections.
    uint32_t LayoutFor(uint32_t chrModel);
    bool LayoutSize(uint32_t layout, uint32_t& width, uint32_t& height);
    void Sections(uint32_t layout, std::vector<TextureSection>& sections);

    // The texture layers the choices paint, in paint order (Esteria's AddMaterialLayers): each chosen
    // ChrCustomizationMaterial matches the layout's ChrModelTextureLayer rows of the same target.
    void TextureLayers(uint32_t chrModel, const std::vector<uint32_t>& choices, std::vector<TextureLayer>& layers);
}

#endif
