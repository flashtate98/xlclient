#pragma once

#include <cstdint>

// TerrainTextureParams (DBFilesClient\TerrainTextureParams.cdbc): per terrain texture, what later ADTs
// keep in MTXF and MTXP, for 3.3.5 ADTs that lack it or lost it in conversion. A row applies
// everywhere, to one map or to one tile (its Scope); the most specific row for a texture wins.
//
//   MTXF / MTXFMask   the MTXF bits the row sets (to MTXF's value) over the tile's own; bits outside
//                     the mask stay as the ADT has them. 0x1: no specular (a cube map); bits 4-7:
//                     texture scale, the texture spread over 1 << n times the area.
//   HeightScale /     MTXP's height blending (the _h texture), Blizzard's values: 0 / 1 = off.
//   HeightOffset
//   NormalStrength    ours: the _h texture's bumps in the light (yards of relief over its full
//                     height), 0 = off.
namespace SXC::Terrain {
    struct TextureParams {
        uint32_t mtxf = 0;          // the tile's MTXF value with the row's bits applied
        float heightScale = 0.0f;
        float heightOffset = 1.0f;
        float normalStrength = 0.0f;
        bool matched = false;       // a row applied
    };

    // Registers the table (SXC::Startup, before the tables load).
    void Register();

    // Changes with every reload and edit of the table and with the [Terrain] TextureParams switch:
    // cache what Resolve gave by it.
    uint32_t Generation();

    // What a texture of the tile `mapName`_`tileX`_`tileY` (the ADT's name) gets, starting from the
    // ADT's own MTXF value. `texture` as MTEX names it.
    TextureParams Resolve(const char* mapName, int32_t tileX, int32_t tileY, const char* texture, uint32_t mtxf);

    // The UV scale MTXF's texture scale bits ask for: 1 / (1 << n).
    inline float UvScale(uint32_t mtxf) {
        return 1.0f / static_cast<float>(1u << ((mtxf >> 4) & 0xFu));
    }
}
