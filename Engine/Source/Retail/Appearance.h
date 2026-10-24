#ifndef ENGINE_SOURCE_RETAIL_APPEARANCE_H
#define ENGINE_SOURCE_RETAIL_APPEARANCE_H

#include <cstdint>
#include <vector>

// Retail character appearance from the customization tables (Tables.h). Geoset selection follows the
// Esteria fork's resolver (Engine/Source/Retail/Appearance.cpp there): a choice expands to its
// ChrCustomizationElement rows, an element that names a related choice applies only when that
// choice is taken too, and a ChrCustomizationGeoset row becomes geoset type * 100 + id.
namespace Retail::Appearance
{
    // The ChrModel for a Retail ChrRaces ID and sex (ChrRaceXChrModel), or 0.
    uint32_t ChrModelFor(uint32_t retailRace, uint32_t sex);

    // One choice per option of the model: the first by OrderIndex that a new character may take
    // (the internal "Transmog" placeholders are skipped). Options appear in their OrderIndex order.
    void DefaultChoices(uint32_t chrModel, std::vector<uint32_t>& choices);

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
