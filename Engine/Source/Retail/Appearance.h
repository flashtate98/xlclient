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
}

#endif
