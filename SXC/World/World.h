#pragma once

#include <cstdint>

class WeatherRec;

// sxc.world's seams in the client (SXC/Api/sxc_world.h): each called from one marked line.
namespace SXC::WorldApi {
    // SMSG_WEATHER, before the client applies it (visuals and ambience): notes the server's weather and
    // answers true while a test weather (sxc_world_api::set_weather) holds it back. It's applied when
    // the test ends.
    bool HoldServerWeather(uint32_t type, float intensity, const WeatherRec* rec, bool immediate, float transition, int32_t soundKitId);

    // The painted share of each terrain layer at a world spot (0..1; layer 0 the rest), as the terrain
    // sample weighs them: the layers' count, 0 where nothing is known (no chunk, a hole). For the grass
    // placed by paint (CMapChunk::CreateDetailDoodads, SXC::Grass::PaintedMix).
    int32_t LayerWeights(float x, float y, float* out, int32_t max);

    // The terrain's height at a world spot as the client's collision has it (CMap::GetHeightTerrain);
    // false off the map or in a hole.
    bool GroundHeight(float x, float y, float& z);

    // Where a world point is on the window, 0..1 from the left / top (sxc_world_api::project); false
    // behind the camera. For debug markers drawn over the world.
    bool ToWindow(float x, float y, float z, float& outX, float& outY);
}
