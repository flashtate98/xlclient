#pragma once

#include <cstdint>

class CGxTex;
class CMapRenderChunk;

// The terrain under the camera as a table (kb: our-shaders, 16's terrain blend): what our terrain shader
// draws a chunk with - its layers' textures, the alpha map, the layers' scales and heights - noted at each
// chunk's draw and kept until the chunk lets go of its layers. Each frame a window of map chunks around the
// camera goes to the device: a grid of chunk entries and the textures they name, which a shader reads at a
// world position to blend the terrain's own colours there (WMO shader 16 first; later grass, puddles).
// Main thread.
namespace SXC::Ground {
    constexpr uint32_t kGrid = 32;          // map chunks per side of the window (33.3 yd each: ~1067 yd)
    constexpr uint32_t kMaxEntries = 1024;  // render chunks in the window (one covers one or two map chunks)
    constexpr uint32_t kMaxTextures = 2048; // distinct textures they name (layers, heights, alpha maps)

    // One render chunk as the shader reads it (std430, 7 x 16 bytes). Texture fields are slots + 1 (0: none).
    struct Entry {
        float place[4];        // the chunk's position (x, y; v23), the alpha map's scale (x, y; v18 + layers)
        float layerScale[4];   // each layer's coordinate scale (v18 + k; both axes alike)
        float heightScale[4];  // TerrainTextureParams' height scale per layer (p14)
        float heightOffset[4]; // and offset - 1 (p15; all zero with the scales: the stock blend)
        uint32_t layerTex[4];
        uint32_t heightTex[4];
        uint32_t alphaTex;
        uint32_t layers;
        uint32_t flags;        // kBigAlpha, kReflective
        uint32_t unused;
    };
    constexpr uint32_t kBigAlpha = 1;    // the map's alpha maps weight the layers (layer 0 takes the rest)
    constexpr uint32_t kReflective = 2;  // layer 0 is a cube map (not in the table: drawn as its neighbours)
    constexpr uint32_t kVertexColour = 4; // the terrain's vertex colours tint it (else 0.5 throughout, as our terrain shader)
    constexpr uint32_t kColours = 145;   // a map chunk's vertex colours (MCCV): 9 x 9 corners, 8 x 8 centres between, by rows

    // A texture of the frame: alpha maps are read clamped, layers and heights wrapped.
    struct Texture {
        CGxTex* gx = nullptr;
        bool clamp = false;
    };

    struct Frame {
        float viewInv[12] = {};   // the world scene's view back to the world, a row per world axis
        float light[16] = {};     // the terrain's light as last drawn: toward the sun (world), the ambient, the sun's diffuse (xyz each,
                                  // w 0), the sun's highlight colour (0 with terrain specular off) and exponent (v27)
        int32_t firstI = 0, firstJ = 0;   // the window's first map chunk (i along world x, j along y, from the map's corner)
        uint32_t grid[kGrid * kGrid] = {};    // [j * kGrid + i]: entry + 1 (0: no chunk known)
        uint32_t holes[kGrid * kGrid] = {};   // per cell its map chunk's holes (MCNK's 4 x 4 bits: (row / 2) * 4 + column / 2)
        uint32_t colours[kGrid * kGrid * kColours] = {};   // per cell its map chunk's vertex colours (D3DCOLOR, 0x7F7F7F neutral)
        float heights[kGrid * kGrid * kColours] = {};      // per cell its map chunk's vertex heights (world z), as the colours
        uint32_t normals[kGrid * kGrid * kColours] = {};   // and its vertex normals (MCNR: x, y, z as signed bytes, /127)
        Entry entries[kMaxEntries] = {};
        uint32_t entryCount = 0;
        Texture textures[kMaxTextures];
        uint32_t textureCount = 0;
    };

    // A render chunk as the terrain's single-pass draw has it (CMapRenderChunk::RenderSinglePassVertexPixelShader,
    // after its layers' textures are resolved): the device textures, the coordinates' and heights' values the
    // draw gives our terrain shader, and the centres of its one or two map chunks.
    struct Note {
        CGxTex* layer[4] = {};
        CGxTex* height[4] = {};
        CGxTex* alpha = nullptr;
        float origin[2] = {};
        float alphaScale[2] = {};
        float layerScale[4] = {};
        float heightScale[4] = {};
        float heightOffset[4] = {};
        uint32_t layers = 0;
        uint32_t flags = 0;
        float centres[2][2] = {};
        const uint32_t* shading[2] = {};   // each map chunk's vertex colours (kColours; null: none)
        const float* heights[2] = {};      // its vertex heights (kColours, from baseZ; null: none)
        const int8_t* normals[2] = {};     // its vertex normals (kColours x 3; null: none)
        float baseZ[2] = {};
        uint16_t holes[2] = {};
        uint32_t centreCount = 0;
        float sunDir[3] = {};    // the terrain's light at this draw: toward the sun (view space, v24),
        float ambient[3] = {};   // the ambient (v25),
        float diffuse[3] = {};   // the sun's diffuse colour (v26)
        float specular[4] = {};  // the sun's highlight colour and exponent (v27; colour 0 with terrain specular off)
    };
    void NoteChunk(const CMapRenderChunk* chunk, const Note& note);

    // Called when a chunk lets go of its layers (CMapRenderChunk::FreeLayers: also on its destruction).
    void ForgetChunk(const CMapRenderChunk* chunk);

    // This frame's table around the camera, built anew at each call (the device asks once a frame). False before
    // the world.
    bool Build(Frame& out);

    // One line for a panel: chunks known, in the window, textures.
    const char* Status();
}
