// SXC: the terrain under the camera as a table (Vulkan/Ground, SXC/Ground; kb: our-shaders, 16's terrain blend):
// for a world position, the terrain chunk there and its colour as our terrain shader blends it (Terrain.frag: the
// alpha map's weights, the layers' heights), unlit. Set 2 bindings 1 and 2 of our WMO pipelines; the includer
// enables GL_EXT_nonuniform_qualifier.

struct GroundChunk {
    vec4  place;         // the chunk's position (x, y; v23), the alpha map's scale (x, y)
    vec4  layerScale;    // each layer's coordinate scale (v18 + k)
    vec4  heightScale;   // p14
    vec4  heightOffset;  // p15 (offset - 1)
    uvec4 layerTex;      // texture slots + 1 (0: none)
    uvec4 heightTex;
    uvec4 misc;          // the alpha map's slot + 1, the layer count, flags (1 big alpha, 2 layer 0 reflective)
};

layout(std430, set = 2, binding = 1) readonly buffer GroundTable {
    vec4  groundViewInv[3];   // the world scene's view back to the world, a row per world axis
    ivec4 groundWindow;       // the window's first map chunk (i along world x, j along y), chunks per side, entries
    vec4  groundGrid;         // a map chunk's size, the map's corner from its middle
    vec4  groundLight[4];     // the terrain's light as last drawn: toward the sun (world), the ambient, the sun's diffuse,
                              // the sun's highlight (colour, 0 with terrain specular off; exponent)
    uint  groundCells[1024];  // [j * side + i]: entry + 1 (0: none)
    uint  groundHoles[1024];  // per cell its map chunk's holes (4 x 4 bits: (row / 2) * 4 + column / 2)
    uint  groundColours[1024 * 145];   // per cell its map chunk's vertex colours (MCCV, D3DCOLOR): 9 x 9 corners, 8 x 8 centres, by rows
    float groundHeights[1024 * 145];   // and its vertex heights (world z), alike
    uint  groundNormals[1024 * 145];   // and its vertex normals (MCNR: x, y, z as signed bytes), alike
    GroundChunk groundChunks[];
};

// Directions between the world scene's view and the world (the view is a rotation and a move).
vec3 GroundDirToWorld(vec3 v)
{
    return vec3(dot(groundViewInv[0].xyz, v), dot(groundViewInv[1].xyz, v), dot(groundViewInv[2].xyz, v));
}

vec3 GroundDirToView(vec3 w)
{
    return groundViewInv[0].xyz * w.x + groundViewInv[1].xyz * w.y + groundViewInv[2].xyz * w.z;
}

// The terrain mesh's triangle at world xy in window cell c: four triangles a quad around its centre vertex
// (CMapChunk::CreateVerticesWorld: rows run along world x, columns along y; CreateIndices): its three vertices'
// indices into the cell's per-vertex arrays and their weights. `hole`: the quad is left out of the mesh.
void GroundTriangle(vec2 xy, ivec2 c, out uvec3 at, out vec3 w, out bool hole)
{
    ivec2 global = c + groundWindow.xy;
    vec2 q = (vec2(groundGrid.y) - xy) / groundGrid.x * 8.0 - vec2(global) * 8.0;
    ivec2 rc = clamp(ivec2(floor(q)), ivec2(0), ivec2(7));
    vec2 f = clamp(q - vec2(rc), 0.0, 1.0);
    uint cell = uint(c.y * groundWindow.z + c.x);
    hole = (groundHoles[cell] & (1u << uint((rc.x >> 1) * 4 + (rc.y >> 1)))) != 0u;
    uint base = cell * 145u;
    uint corner = base + uint(rc.x * 17 + rc.y);       // the quad's corner (row, column)
    uint centre = base + uint(rc.x * 17 + 9 + rc.y);   // its centre vertex
    // The triangle whose outer edge is nearest, and the weights of its two corners and the centre.
    float top = f.x, bottom = 1.0 - f.x, left = f.y, right = 1.0 - f.y;
    float edge = min(min(top, bottom), min(left, right));
    uint a, b;
    float wb;

    if (edge == top)         { a = corner;       b = corner + 1u;  wb = f.y - top; }
    else if (edge == bottom) { a = corner + 17u; b = corner + 18u; wb = f.y - bottom; }
    else if (edge == left)   { a = corner;       b = corner + 17u; wb = f.x - left; }
    else                     { a = corner + 1u;  b = corner + 18u; wb = f.x - right; }

    float wc = 2.0 * edge;
    at = uvec3(a, b, centre);
    w = vec3(1.0 - wb - wc, wb, wc);
}

// The vertex colour at world xy in window cell c (MCCV; 0.5 is neutral), as the terrain's mesh interpolates it.
vec3 GroundVertexColour(vec2 xy, ivec2 c)
{
    uvec3 at;
    vec3 w;
    bool hole;
    GroundTriangle(xy, c, at, w, hole);
    return unpackUnorm4x8(groundColours[at.x]).zyx * w.x + unpackUnorm4x8(groundColours[at.y]).zyx * w.y +
           unpackUnorm4x8(groundColours[at.z]).zyx * w.z;
}

// The terrain's normal at world xy (its vertex normals over the mesh's triangles, as it's lit), or false where there's no
// chunk (holes count: a seam can run along one).
bool GroundNormal(vec2 xy, out vec3 n)
{
    n = vec3(0.0, 0.0, 1.0);
    ivec2 c = ivec2(floor((vec2(groundGrid.y) - xy) / groundGrid.x)) - groundWindow.xy;

    if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, ivec2(groundWindow.z))) || groundCells[c.y * groundWindow.z + c.x] == 0u)
    {
        return false;
    }

    uvec3 at;
    vec3 w;
    bool hole;
    GroundTriangle(xy, c, at, w, hole);
    vec3 sum = unpackSnorm4x8(groundNormals[at.x]).xyz * w.x + unpackSnorm4x8(groundNormals[at.y]).xyz * w.y +
               unpackSnorm4x8(groundNormals[at.z]).xyz * w.z;
    n = dot(sum, sum) > 0.0 ? normalize(sum) : vec3(0.0, 0.0, 1.0);
    return true;
}

// The terrain's surface height at world xy (as its mesh lies), or false where there's none: outside the window, a chunk
// not drawn yet, a hole.
bool GroundHeight(vec2 xy, out float z)
{
    z = 0.0;
    ivec2 c = ivec2(floor((vec2(groundGrid.y) - xy) / groundGrid.x)) - groundWindow.xy;

    if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, ivec2(groundWindow.z))) || groundCells[c.y * groundWindow.z + c.x] == 0u)
    {
        return false;
    }

    uvec3 at;
    vec3 w;
    bool hole;
    GroundTriangle(xy, c, at, w, hole);
    z = groundHeights[at.x] * w.x + groundHeights[at.y] * w.y + groundHeights[at.z] * w.z;
    return !hole;
}

// The terrain's share by nearness (16's proximity blend; SXC::Wmo::TerrainBlendParams' fourth column: on, the fade above
// the terrain, the ceiling): all terrain where the surface lies under the terrain, fading out over `above` yd above it,
// and out again toward `ceiling` yd under it (terrain that high is a roof, not the ground at a seam). None over a hole.
float GroundProximity(vec3 world, float above, float ceiling)
{
    float z;

    if (!GroundHeight(world.xy, z))
    {
        return 0.0;
    }

    float d = world.z - z;   // + above the terrain, - under it
    return d >= 0.0 ? 1.0 - smoothstep(0.0, max(above, 1e-3), d)
                    : 1.0 - smoothstep(max(ceiling - 0.5, 0.0), max(ceiling, 1e-3), -d);
}

layout(set = 2, binding = 2) uniform sampler2D groundTex[2048];

// A view-space position of the world scene in the world.
vec3 GroundWorld(vec3 view)
{
    vec4 p = vec4(view, 1.0);
    return vec3(dot(groundViewInv[0], p), dot(groundViewInv[1], p), dot(groundViewInv[2], p));
}

// The terrain's light for a world normal, as its vertex shader lights it (TerrainVertex.glsl's TerrainLight: the sun's
// diffuse by N.L and the ambient, clamped; the vertex colour is in GroundColour's colour).
vec3 GroundLight(vec3 n)
{
    return min(groundLight[2].xyz * clamp(dot(groundLight[0].xyz, n), 0.0, 1.0) + groundLight[1].xyz, vec3(1.0));
}

// The terrain's sun highlight at a view-space position for a world normal, as its vertex shader computes it
// (TerrainLight's kSpecular: Blinn-Phong toward the sun, v27's exponent and colour), here in the world; the gloss is
// the caller's.
vec3 GroundHighlight(vec3 viewPos, vec3 n)
{
    if (all(equal(groundLight[3].xyz, vec3(0.0))))
    {
        return vec3(0.0);
    }

    vec3 toEye = normalize(GroundDirToWorld(-viewPos));
    vec3 h = normalize(toEye + groundLight[0].xyz);
    return pow(max(dot(h, n), 0.0), max(groundLight[3].w, 1.0)) * groundLight[3].xyz;
}

// The window cell at world xy, and the entry of its chunk, or -1 (outside the window, or a chunk not drawn yet).
int GroundChunkAt(vec2 xy, out ivec2 c)
{
    c = ivec2(floor((vec2(groundGrid.y) - xy) / groundGrid.x)) - groundWindow.xy;

    if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, ivec2(groundWindow.z))))
    {
        return -1;
    }

    return int(groundCells[c.y * groundWindow.z + c.x]) - 1;
}

vec4 GroundSample(uint slot, vec2 uv, vec2 dx, vec2 dy, vec4 none)
{
    return slot == 0u ? none : textureGrad(groundTex[nonuniformEXT(slot - 1u)], uv, dx, dy);
}

// The terrain's colour at world xy (its screen derivatives dx, dy taken by the caller in uniform control flow):
// the coordinates as the terrain's vertex shader makes them ((world.yx - the chunk's position.yx) x the scale.yx,
// TerrainRegs.glsl), the weights as CoverLayerWeights, by the layers' heights as HeightWeights where they have any.
// False where there's no chunk.
// `view` (Wmo/TerrainBlendDebug): 4 the alpha map (layers 1..3 in r, g, b), 5 the final weights (layer 0 red,
// 1 green, 2 blue, 3 yellow), 6 the vertex colour (0.5 grey neutral); else the colour.
bool GroundColour(vec2 xy, vec2 dx, vec2 dy, int view, out vec3 colour, out float gloss, out int entry)
{
    colour = vec3(0.0);
    gloss = 0.0;
    ivec2 cell;
    entry = GroundChunkAt(xy, cell);

    if (entry < 0)
    {
        return false;
    }

    GroundChunk g = groundChunks[entry];
    int layers = int(min(g.misc.y, 4u));
    vec2 r = xy.yx - g.place.yx, rx = dx.yx, ry = dy.yx;
    vec2 a = g.place.wz;
    vec4 alpha = GroundSample(g.misc.x, r * a, rx * a, ry * a, vec4(0.0));

    vec4 w = vec4(1.0, 0.0, 0.0, 0.0);

    if ((g.misc.z & 1u) != 0u)
    {
        w = vec4(1.0 - clamp(dot(alpha.xyz, vec3(1.0)), 0.0, 1.0), alpha.xyz);
    }
    else
    {
        for (int k = 1; k < layers; k++)
        {
            w *= 1.0 - alpha[k - 1];
            w[k] = alpha[k - 1];
        }
    }

    vec4 layer[4];   // rgb, and the alpha: the gloss where the terrain shines (the _s tileset textures)
    vec4 h = vec4(0.0);

    for (int k = 0; k < 4; k++)
    {
        layer[k] = vec4(0.0);

        if (k >= layers)
        {
            w[k] = 0.0;
            continue;
        }

        float s = g.layerScale[k];
        layer[k] = GroundSample(g.layerTex[k], r * s, rx * s, ry * s, vec4(0.5, 0.5, 0.5, 0.0));
        // A layer without a height texture counts as 1: the terrain binds the white image in its place (Pipelines'
        // height slots), so its height blend weighs it fully (read as 0, snow with heights hid the stone beside it).
        h[k] = GroundSample(g.heightTex[k], r * s, rx * s, ry * s, vec4(1.0)).a;
    }

    // A reflective layer 0 (a cube map, not in the table) shows as the layers beside it.
    if ((g.misc.z & 2u) != 0u && layers > 1)
    {
        w.x = 0.0;
    }

    // The heights (Terrain.frag's HeightWeights), where the chunk's layers have any.
    if (layers >= 2 && (any(notEqual(g.heightScale, vec4(0.0))) || any(notEqual(g.heightOffset, vec4(0.0)))))
    {
        vec4 pct = w * (h * g.heightScale + g.heightOffset + 1.0);
        float strongest = max(max(pct.x, pct.y), max(pct.z, pct.w));
        pct *= 1.0 - clamp(strongest - pct, 0.0, 1.0);
        w = pct;
    }

    w /= max(dot(w, vec4(1.0)), 1e-5);
    vec4 mixed = layer[0] * w.x + layer[1] * w.y + layer[2] * w.z + layer[3] * w.w;
    gloss = mixed.a;
    colour = view == 6 ? ((g.misc.z & 4u) != 0u ? GroundVertexColour(xy, cell) : vec3(0.5))
           : view == 4 ? alpha.rgb
           : view == 5 ? vec3(1.0, 0.0, 0.0) * w.x + vec3(0.0, 1.0, 0.0) * w.y + vec3(0.0, 0.0, 1.0) * w.z + vec3(1.0, 1.0, 0.0) * w.w
                       : mixed.rgb;

    // The vertex colours tint it as they scale the terrain's light (0.5 neutral: x 2), where the map's vertices
    // carry them (else 0.5, as our terrain shader's kVertexColor).
    if (view < 4 && (g.misc.z & 4u) != 0u)
    {
        colour *= 2.0 * GroundVertexColour(xy, cell);
    }

    return true;
}
