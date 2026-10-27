// sxc.cover (SXC/Api/sxc_cover.h) and its seam in the terrain chunk draw (SXC/Cover/Cover.h).

#include "SXC/Cover/Cover.h"
#include "SXC/Cover/Trails.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "SXC/Api/sxc_cover.h"
#include "SXC/Log.h"
#include "SXC/Settings/Settings.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/GroundEffectTextureRec.h"
#include "WoW/Source/WorldClient/CChunkLiquid.h"
#include "WoW/Source/WorldClient/CMapBaseObjLink.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapRenderChunk.h"
#include "WoW/Source/WorldClient/Types.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    using namespace SXC::Cover;

    static_assert(sizeof(sxc_cover_entry) == 80, "the terrain shader reads entries as five vec4");
    static_assert(SXC_COVER_MAX_ENTRIES == kMaxEntries && SXC_COVER_MAX_TEXTURES == kMaxTextures, "SXC/Cover/Cover.h follows the API");

    bool s_available = false;
    uint32_t s_slots = 0; // texture slots the device's shader has
    StatusFn s_deviceStatus = nullptr;

    sxc_cover_resolve_fn s_resolve = nullptr;
    void* s_resolveUser = nullptr;
    uint32_t s_generation = 1; // the resolver's answers; a chunk cached under another one asks again

    sxc_cover_look s_look = {};
    bool s_haveLook = false;

    std::vector<sxc_cover_entry> s_entries;
    uint32_t s_entryVersion = 1;

    struct Map {
        std::vector<float> cells;
        uint32_t size = 0;
        float cell = 1.0f;
        int32_t firstI = 0, firstJ = 0;
        uint32_t version = 1;
        float mostFallen = 0.0f; // yd: the deepest fallen snow in it
    };
    Map s_near, s_far;

    // The surface grid as last read back (step 5e): RGBA32F per cell, the device's layout, read in its
    // buffer. 96 yd across, in cells of 0.125 yd (fine: footprints' dips stay apart) or 0.25 yd.
    constexpr float kSurfaceAcross = 96.0f;
    const float* s_surface = nullptr;
    int32_t s_surfaceSize = 0;
    float s_surfaceCell = 0.25f;

    float SurfaceCellWanted() {
        static SXC::Settings::Bool s_fine("Terrain", "CoverSurfaceFine", true, "Cover surface grid: fine",
                                          "The covers' surface as the client reads it back (decals on the cover; later effects at units' feet, "
                                          "units and the camera) in cells of 0.125 yd rather than 0.25 yd: decals over footprints stay still. "
                                          "Four times the data copied back from the GPU each frame (about 9 MB rather than 2.4 MB).");
        return s_fine.Get() ? 0.125f : 0.25f;
    }

    // The cover textures: loaded through the client's texture cache like the terrain's layers, when a
    // drawn chunk first shows them, then kept. A gloss texture tries its "_s" variant first (where later
    // tilesets keep the gloss mask in alpha). With fewer slots than textures (no per-pixel indexing),
    // the slots go to the textures the chunks drawn use most; one keeps its slot while it's in view.
    struct Texture {
        std::string path;
        uint32_t flags = 0;
        CTexture* texture = nullptr;
        bool specular = false;  // the "_s" variant was found
        bool failed = false;    // tried and couldn't be loaded
        uint32_t uses = 0;      // chunks drawn showing it since the slots were last assigned
        uint64_t lastUsed = 0;  // the frame it was last shown
        uint32_t slot = 0;      // 1.. while it has one
    };
    std::vector<Texture> s_textures;
    uint32_t s_textureCount = 0;
    uint64_t s_frameNo = 0;
    uint32_t s_wanted = 0, s_unslotted = 0; // last assignment: textures in view, and those without a slot
    std::vector<uint32_t> s_slotTexture;   // per slot (fallback): the texture holding it, 0 free
    constexpr uint64_t kKeepFrames = 120;  // a texture out of view this long gives its slot up

    // Per render chunk: what the resolver answered, and what it was asked about (a chunk object is
    // reused for other ground as the map streams).
    struct Cached {
        const CMapChunk* chunks[2] = {};
        int32_t textureIds[SXC_COVER_MAX_LAYERS] = {};
        uint32_t layerCount = 0;
        uint32_t generation = 0;
        float painted[4] = {}, snow[4] = {};
    };
    std::unordered_map<const CMapRenderChunk*, Cached> s_chunks;
    uint64_t s_resolved = 0, s_draws = 0, s_withCover = 0;

    Frame s_frame;
    char s_status[1024] = "";

    void Release(Texture& t) {
        if (t.texture) {
            HandleClose(reinterpret_cast<HOBJECT>(t.texture));
        }

        t = Texture{};
    }

    CTexture* LoadTexture(Texture& t) {
        t.specular = false;

        if ((t.flags & SXC_COVER_TEXTURE_GLOSS) && t.path.size() > 4 && SStrCmpI(t.path.c_str() + t.path.size() - 4, ".blp", 4) == 0) {
            const std::string gloss = t.path.substr(0, t.path.size() - 4) + "_s.blp";

            if (SFile::FileExists(gloss.c_str())) {
                if (CTexture* texture = CMap::LoadTexture(gloss.c_str())) {
                    t.specular = true;
                    return texture;
                }
            }
        }

        return CMap::LoadTexture(t.path.c_str());
    }

    // The area tile a chunk belongs to (CMapRenderChunk::CreateLayers' way).
    CMapArea* AreaOf(const CMapChunk* chunk) {
        CMapBaseObjLink* link = chunk ? const_cast<CMapChunk*>(chunk)->m_parentLinkList.Head() : nullptr;
        return link ? static_cast<CMapArea*>(link->m_ref) : nullptr;
    }

    // A render layer's description, from the map chunk whose layers name its texture.
    bool DescribeLayer(const CMapRenderChunk* rc, int32_t textureId, sxc_cover_layer& out) {
        for (const CMapChunk* chunk : rc->m_mapChunks) {
            if (!chunk || !chunk->m_layers || !chunk->m_header) {
                continue;
            }

            CMapArea* area = AreaOf(chunk);

            for (uint32_t l = 0; l < chunk->m_header->m_nLayers; l++) {
                const SMLayer& layer = chunk->m_layers[l];

                if (static_cast<int32_t>(layer.m_textureId) != textureId) {
                    continue;
                }

                out.texture = area && layer.m_textureId < area->m_textures.Count() && area->m_textures[layer.m_textureId].m_textureName
                                  ? area->m_textures[layer.m_textureId].m_textureName
                                  : "";
                out.ground_effect = layer.m_offectId;
                out.terrain_type = -1;

                if (const GroundEffectTextureRec* rec = g_groundEffectTextureDB.GetRecord(static_cast<int32_t>(layer.m_offectId))) {
                    out.terrain_type = rec->m_sound; // GroundEffectTexture's last column: the TerrainType
                }

                return true;
            }
        }

        return false;
    }

    void Resolve(const CMapRenderChunk* rc, Cached& c) {
        sxc_cover_chunk q = {};
        q.struct_size = sizeof(q);
        q.map_id = ::World::QueryMapId();
        q.area = rc->m_mapChunks[0] ? static_cast<uint32_t>(rc->m_mapChunks[0]->m_areaId) : 0u;
        q.layer_count = static_cast<int32_t>(c.layerCount);

        for (uint32_t l = 0; l < c.layerCount; l++) {
            q.layers[l] = sxc_cover_layer{ "", 0, -1 };
            DescribeLayer(rc, c.textureIds[l], q.layers[l]);
        }

        s_resolve(s_resolveUser, &q);
        s_resolved++;

        for (uint32_t l = 0; l < SXC_COVER_MAX_LAYERS; l++) {
            const bool have = l < c.layerCount;
            c.painted[l] = have && q.painted[l] <= kMaxEntries ? static_cast<float>(q.painted[l]) : 0.0f;
            c.snow[l] = have && q.snow_look[l] <= kMaxEntries ? static_cast<float>(q.snow_look[l]) : 0.0f;
        }
    }

    // --- the service --------------------------------------------------------------------------------

    wxl_bool WXL_CALL Available() {
        return s_available ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL SetResolver(sxc_cover_resolve_fn resolve, void* user) {
        s_resolve = resolve;
        s_resolveUser = user;
        s_generation++;
    }

    void WXL_CALL Invalidate() {
        s_generation++;
    }

    void WXL_CALL SetEntries(const sxc_cover_entry* entries, uint32_t count) {
        count = entries ? (count < kMaxEntries ? count : kMaxEntries) : 0;
        s_entries.assign(entries, entries + count);
        s_entryVersion++;
    }

    void WXL_CALL SetTextures(const char* const* paths, const uint32_t* flags, uint32_t count) {
        count = paths ? (count < kMaxTextures ? count : kMaxTextures) : 0;

        // A texture whose path and flags stay keeps what's loaded (the mod sends the list again whenever
        // its table reloads); the others load when they're next shown.
        for (uint32_t k = count; k < s_textures.size(); k++) {
            Release(s_textures[k]);
        }

        s_textures.resize(count);

        for (uint32_t k = 0; k < count; k++) {
            Texture& t = s_textures[k];
            const char* path = paths[k] ? paths[k] : "";
            const uint32_t f = flags ? flags[k] : 0u;

            if (t.path != path || t.flags != f) {
                Release(t);
                t.path = path;
                t.flags = f;
            }
        }

        s_textureCount = count;
        s_slotTexture.clear();
    }

    // The slots for this frame from what the chunks showed: every texture its own where the device has a
    // slot for each, else the most shown ones (a texture in view keeps its slot).
    void AssignSlots() {
        s_frameNo++;
        s_wanted = 0;
        s_unslotted = 0;

        // Shown textures load (once).
        for (uint32_t k = 0; k < s_textureCount; k++) {
            Texture& t = s_textures[k];

            if (t.uses == 0 || t.texture || t.failed || t.path.empty()) {
                continue;
            }

            t.texture = LoadTexture(t);
            t.failed = !t.texture;

            if (t.failed) {
                SXC::LogOnce((std::string("cover-texture|") + t.path).c_str(), "cover: the texture \"%s\" could not be loaded", t.path.c_str());
            }
        }

        if (s_slots >= s_textureCount) {
            for (uint32_t k = 0; k < s_textureCount; k++) {
                Texture& t = s_textures[k];
                t.slot = t.texture ? k + 1 : 0;
                s_wanted += t.uses > 0;
                t.uses = 0;
            }

            return;
        }

        if (s_slotTexture.size() != s_slots) {
            s_slotTexture.assign(s_slots, 0);

            for (Texture& t : s_textures) {
                t.slot = 0;
            }
        }

        // Residents out of view for a while give their slot up.
        for (uint32_t s = 0; s < s_slots; s++) {
            const uint32_t id = s_slotTexture[s];

            if (id && (id > s_textureCount || !s_textures[id - 1].texture || s_frameNo - s_textures[id - 1].lastUsed > kKeepFrames)) {
                if (id <= s_textureCount) {
                    s_textures[id - 1].slot = 0;
                }

                s_slotTexture[s] = 0;
            }
        }

        // The shown ones without a slot, most shown first, into the free slots.
        std::vector<uint32_t> waiting;

        for (uint32_t k = 0; k < s_textureCount; k++) {
            const Texture& t = s_textures[k];
            s_wanted += t.uses > 0;

            if (t.uses > 0 && t.texture && !t.slot) {
                waiting.push_back(k + 1);
            }
        }

        std::sort(waiting.begin(), waiting.end(), [](uint32_t a, uint32_t b) { return s_textures[a - 1].uses > s_textures[b - 1].uses; });
        size_t next = 0;

        for (uint32_t s = 0; s < s_slots && next < waiting.size(); s++) {
            if (!s_slotTexture[s]) {
                s_slotTexture[s] = waiting[next];
                s_textures[waiting[next] - 1].slot = s + 1;
                next++;
            }
        }

        s_unslotted = static_cast<uint32_t>(waiting.size() - next);

        for (Texture& t : s_textures) {
            t.uses = 0;
        }
    }

    void SetMap(Map& map, const sxc_cover_map* in) {
        map.version++;

        map.mostFallen = 0.0f;

        if (!in || in->struct_size < sizeof(sxc_cover_map) || !in->cells || in->size == 0 || in->size > SXC_COVER_MAX_MAP) {
            map.size = 0;
            map.cells.clear();
            return;
        }

        const size_t floats = static_cast<size_t>(in->size) * in->size * 4;
        map.cells.resize(floats);

        for (size_t i = 0; i < floats; i++) {
            map.cells[i] = std::isfinite(in->cells[i]) ? in->cells[i] : 0.0f;

            if ((i & 3) == 0 && map.cells[i] > map.mostFallen) {
                map.mostFallen = map.cells[i];
            }
        }

        map.size = in->size;
        map.cell = in->cell_size > 0.01f ? in->cell_size : 0.01f;
        map.firstI = in->first_i;
        map.firstJ = in->first_j;
    }

    void WXL_CALL SetNear(const sxc_cover_map* map) { SetMap(s_near, map); }
    void WXL_CALL SetFar(const sxc_cover_map* map) { SetMap(s_far, map); }

    void WXL_CALL SetLook(const sxc_cover_look* look) {
        // A mod built before the appended fields passes the size it knows; those stay 0.
        if (!look || look->struct_size < offsetof(sxc_cover_look, steady_wind)) {
            s_haveLook = false;
            return;
        }

        s_look = sxc_cover_look{};
        std::memcpy(&s_look, look, look->struct_size < sizeof(sxc_cover_look) ? look->struct_size : sizeof(sxc_cover_look));
        s_haveLook = true;
    }

    const char* WXL_CALL Status() {
        if (!s_available) {
            return "off: this renderer doesn't draw covers (Vulkan with our terrain shader only)";
        }

        if (!s_haveLook || !s_look.enabled) {
            return "off: no mod turned it on";
        }

        uint32_t loaded = 0;

        for (uint32_t k = 0; k < s_textureCount; k++) {
            loaded += s_textures[k].texture != nullptr;
        }

        char textures[120];

        if (s_slots >= s_textureCount) {
            std::snprintf(textures, sizeof(textures), "textures: %u of %u loaded, %u in view (per-pixel indexing: all show)", loaded, s_textureCount, s_wanted);
        } else {
            std::snprintf(textures, sizeof(textures), "textures: %u of %u loaded, %u in view, %u slots (%u in view without one draw plain)", loaded, s_textureCount,
                          s_wanted, s_slots, s_unslotted);
        }

        char trails[400] = "trails off";

        if (s_look.trails) {
            const Trails::Stats t = Trails::GetStats();
            int at = std::snprintf(trails, sizeof(trails), "trails: %u feet and %u body parts followed, %u carvings last frame, %llu footsteps", t.feet,
                                   t.parts, t.stamps, static_cast<unsigned long long>(t.footsteps));

            // Your feet: the event and bone each follows, and where it is now (yd ahead, to the left, up).
            for (int k = 0; k < 2 && at > 0 && at < static_cast<int>(sizeof(trails)); k++) {
                const Trails::FootView& v = t.player[k];

                if (!v.known) {
                    at += std::snprintf(trails + at, sizeof(trails) - at, "\nyour %s foot: not known yet", k ? "right" : "left");
                    continue;
                }

                const char e[5] = { static_cast<char>(v.event & 0xFF), static_cast<char>((v.event >> 8) & 0xFF), static_cast<char>((v.event >> 16) & 0xFF),
                                    static_cast<char>((v.event >> 24) & 0xFF), 0 };
                at += std::snprintf(trails + at, sizeof(trails) - at,
                                    "\nyour %s foot (last step %s): bone %u (key %d, parent %d), %.2f below its joint: %+.2f ahead, %+.2f left, %+.2f up",
                                    k ? "right" : "left", e, v.bone, v.keyBone, v.parent == 0xFFFF ? -1 : static_cast<int>(v.parent), v.sole, v.forward,
                                    v.left, v.up);
            }
        }

        // The surface grid under the player (step 5e): what the terrain wrote, read back.
        char surface[120] = "surface under you: not known (terrain not drawn there yet)";

        if (CGPlayer_C* player = ClntObjMgrGetActivePlayerObj()) {
            const C3Vector p = player->GetPosition();
            float top = 0.0f, ground = 0.0f;

            if (SXC::Cover::SurfaceAt(p.x, p.y, top, &ground)) {
                std::snprintf(surface, sizeof(surface), "surface under you: %.2f yd of cover, %+.2f yd from your feet", top - ground, top - p.z);
            }
        }

        std::snprintf(s_status, sizeof(s_status), "%s; chunks drawn %llu (%llu with cover), resolved %llu, %u entries; %s; %s\n%s",
                      s_deviceStatus ? s_deviceStatus() : "", static_cast<unsigned long long>(s_draws), static_cast<unsigned long long>(s_withCover),
                      static_cast<unsigned long long>(s_resolved), static_cast<unsigned>(s_entries.size()), textures, trails, surface);
        return s_status;
    }

    int32_t WXL_CALL TextureState(uint32_t texture) {
        if (texture < 1 || texture > s_textureCount || !s_textures[texture - 1].texture) {
            return 0;
        }

        const Texture& t = s_textures[texture - 1];
        return (TextureIsLoaded(t.texture) ? (t.specular ? 3 : 2) : 1) + (t.slot ? 4 : 0);
    }

    const sxc_cover_api s_api = { sizeof(sxc_cover_api), Available, SetResolver, Invalidate, SetEntries, SetTextures, SetNear, SetFar, SetLook, Status, TextureState };
}

namespace SXC::Cover {
    const sxc_cover_api* Api() {
        return &s_api;
    }

    void SetDevice(bool available, uint32_t textureSlots, StatusFn status) {
        s_available = available;
        s_slots = textureSlots < kMaxSlots ? textureSlots : kMaxSlots;
        s_slotTexture.clear();
        s_deviceStatus = status;
        s_near.version++; // a new device holds no maps yet
        s_far.version++;
        s_entryVersion++;
    }

    const Frame& Current() {
        Frame& f = s_frame;
        f.on = s_available && s_haveLook && s_look.enabled;

        if (!f.on) {
            return f;
        }

        f.fallenSnow = s_look.fallen_snow != 0;
        f.debug = s_look.debug;
        f.brightness = s_look.brightness;
        f.wetDarkening = s_look.wet_darkening;
        f.thinDepth = s_look.thin_depth > 0.001f ? s_look.thin_depth : 0.001f;
        f.depthScale = s_look.depth_scale;
        f.breakupScale = s_look.breakup_scale;
        f.driftScale = s_look.drift_scale;
        f.textureTile = s_look.texture_tile > 0.01f ? s_look.texture_tile : 33.3333333f / 8.0f;

        for (int k = 0; k < 3; k++) {
            f.outside[k] = std::isfinite(s_look.outside[k]) ? s_look.outside[k] : 0.0f;
        }

        f.tessDetail = s_look.tess_detail > 0.0f ? (s_look.tess_detail > 0.002f ? s_look.tess_detail : 0.002f) : 0.0f;
        f.tessReach = s_look.tess_reach > 1.0f ? s_look.tess_reach : 1.0f;
        f.tessMost = s_look.tess_most >= 1.0f ? s_look.tess_most : 1.0f;

        // Trails: carved now (the feet's bones are this frame's), handed over with what changed.
        Trails::Settings trails;
        trails.on = s_look.trails != 0 && f.tessDetail > 0.0f;
        trails.reach = s_look.trail_reach > 1.0f ? s_look.trail_reach : 1.0f;
        trails.carve = s_look.trail_carve;
        trails.lift = s_look.trail_lift > 0.0f ? s_look.trail_lift : 0.0f;

        // The deepest any cover is now (the entries' depths and fallen snow at their most drifted, x the
        // look's scale, a margin): a carving point higher above its unit can't reach a cover.
        {
            const float scale = s_look.depth_scale > 0.0f ? s_look.depth_scale : 0.0f;
            float painted = 0.0f, drift = 0.35f;

            for (const sxc_cover_entry& e : s_entries) {
                const float d = e.depth * (1.0f + std::fmax(e.drift, 0.0f) * s_look.drift_scale) * scale + std::fmax(e.z_offset, 0.0f);
                painted = std::fmax(painted, d);
                drift = std::fmax(drift, e.drift);
            }

            const float fallen = s_look.fallen_snow ? std::fmax(std::fmax(s_near.mostFallen, s_far.mostFallen), s_look.outside[0]) : 0.0f;
            trails.deepest = std::fmax(painted, fallen * (1.0f + drift * s_look.drift_scale) * scale) + 0.1f;
        }
        Trails::Update(trails);
        f.trails = trails.on;
        f.trailNow = Trails::Now();
        f.trailPersist = s_look.trail_persist > 1.0f ? s_look.trail_persist : 1.0f;
        f.trailSlur = s_look.trail_slur > 0.0f ? s_look.trail_slur : 0.0f;
        f.trailRim = s_look.trail_rim > 0.0f ? s_look.trail_rim : 0.0f;
        static std::vector<int32_t> s_rects[2];
        const Trails::Layer* layers[2] = { &Trails::Fine(), &Trails::Coarse() };
        Frame::TrailLayer* views[2] = { &f.trailFine, &f.trailCoarse };

        for (int k = 0; k < 2; k++) {
            const Trails::Layer& L = *layers[k];
            Frame::TrailLayer& v = *views[k];
            s_rects[k].clear();

            for (const Trails::Rect& r : L.dirty) {
                s_rects[k].insert(s_rects[k].end(), { r.x, r.y, r.w, r.h });
            }

            v.size = trails.on ? L.size : 0;
            v.cell = L.cell;
            v.firstI = L.firstI;
            v.firstJ = L.firstJ;
            v.cells = L.cells.empty() ? nullptr : L.cells.data();
            v.rects = s_rects[k].empty() ? nullptr : s_rects[k].data();
            v.rectCount = static_cast<uint32_t>(L.dirty.size());
            v.full = L.full;
        }

        Trails::TookChanges();

        // The surface grid's window: around the player (the terrain writes the cells in it).
        f.surface = false;

        if (CGPlayer_C* player = ClntObjMgrGetActivePlayerObj()) {
            const C3Vector p = player->GetPosition();
            const float cell = SurfaceCellWanted();
            const int32_t size = static_cast<int32_t>(kSurfaceAcross / cell + 0.5f);
            f.surface = true;
            f.surfaceSize = static_cast<uint32_t>(size);
            f.surfaceCell = cell;
            f.surfaceFirstI = static_cast<int32_t>(std::floor(p.x / cell)) - size / 2;
            f.surfaceFirstJ = static_cast<int32_t>(std::floor(p.y / cell)) - size / 2;
        }

        f.entries = s_entries.empty() ? nullptr : s_entries.data();
        f.entryCount = static_cast<uint32_t>(s_entries.size());
        f.entryVersion = s_entryVersion;

        f.nearSize = s_near.size;
        f.nearCell = s_near.cell;
        f.nearFirstI = s_near.firstI;
        f.nearFirstJ = s_near.firstJ;
        f.nearCells = s_near.size ? s_near.cells.data() : nullptr;
        f.nearVersion = s_near.version;
        f.farSize = s_far.size;
        f.farCell = s_far.cell;
        f.farFirstI = s_far.firstI;
        f.farFirstJ = s_far.firstJ;
        f.farCells = s_far.size ? s_far.cells.data() : nullptr;
        f.farVersion = s_far.version;

        AssignSlots();
        f.slotCount = 0;

        for (uint32_t k = 0; k < s_textureCount; k++) {
            const Texture& t = s_textures[k];
            f.textureGloss[k] = (t.flags & SXC_COVER_TEXTURE_GLOSS) != 0;

            if (!t.slot || t.slot > s_slots) {
                continue;
            }

            // Asking without waiting also moves a texture still loading up the loader's queue.
            f.slotTexture[t.slot - 1] = static_cast<uint16_t>(k + 1);
            f.slotGx[t.slot - 1] = TextureGetGxTex(t.texture, 0, nullptr);
            f.slotCount = t.slot > f.slotCount ? t.slot : f.slotCount;
        }

        // Slots below the highest with nothing in them this frame.
        for (uint32_t s = 0; s < f.slotCount; s++) {
            const uint32_t id = f.slotTexture[s];

            if (!id || id > s_textureCount || s_textures[id - 1].slot != s + 1) {
                f.slotTexture[s] = 0;
                f.slotGx[s] = nullptr;
            }
        }

        return f;
    }

    void TakeSurface(const float* cells, uint32_t size, float cell) {
        const bool ok = cells && size > 1 && cell > 0.0f;
        s_surface = ok ? cells : nullptr;
        s_surfaceSize = ok ? static_cast<int32_t>(size) : 0;
        s_surfaceCell = ok ? cell : 0.25f;
    }

    bool SurfaceAt(float x, float y, float& surfaceZ, float* groundZ) {
        const int32_t n = s_surfaceSize;

        if (!s_surface || n < 2) {
            return false;
        }

        // Bilinear between the four cells around the point, over those that hold their own cell (a cell
        // the terrain didn't draw lately keeps an older one's tag, or none).
        const float cx = x / s_surfaceCell - 0.5f, cy = y / s_surfaceCell - 0.5f;
        const int32_t bi = static_cast<int32_t>(std::floor(cx)), bj = static_cast<int32_t>(std::floor(cy));
        const float fx = cx - static_cast<float>(bi), fy = cy - static_cast<float>(bj);
        float weight = 0.0f, surface = 0.0f, ground = 0.0f;

        for (int k = 0; k < 4; k++) {
            const int32_t i = bi + (k & 1), j = bj + (k >> 1);
            const int32_t si = ((i % n) + n) % n, sj = ((j % n) + n) % n;
            const float* c = &s_surface[(static_cast<size_t>(sj) * n + si) * 4];

            if (c[3] < 0.5f || c[1] != static_cast<float>((i & 4095) * 4096 + (j & 4095))) {
                continue;
            }

            const float w = std::fmax(((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy), 0.001f);
            weight += w;
            surface += w * c[0];
            ground += w * c[2];
        }

        if (weight <= 0.0f) {
            return false;
        }

        surfaceZ = surface / weight;

        if (groundZ) {
            *groundZ = ground / weight;
        }

        return true;
    }

    void LiftAtFeet(C44Matrix& pose, const C44Matrix& view, const C44Matrix& viewInv) {
        if (!s_surface) {
            return;
        }

        // The origin in the world (row vectors: world = view-space point x the view's inverse).
        const float vx = pose.d0, vy = pose.d1, vz = pose.d2;
        const float x = vx * viewInv.a0 + vy * viewInv.b0 + vz * viewInv.c0 + viewInv.d0;
        const float y = vx * viewInv.a1 + vy * viewInv.b1 + vz * viewInv.c1 + viewInv.d1;
        const float z = vx * viewInv.a2 + vy * viewInv.b2 + vz * viewInv.c2 + viewInv.d2;
        // The cover's thickness around (the thickest within the ring: the unit's own footprint is thin),
        // on the ground under the origin (a slope's uphill ground doesn't count).
        constexpr float kRing = 1.5f;
        const float ring[8][2] = { { kRing, 0.0f }, { -kRing, 0.0f }, { 0.0f, kRing }, { 0.0f, -kRing },
                                   { 1.06f, 1.06f }, { -1.06f, 1.06f }, { 1.06f, -1.06f }, { -1.06f, -1.06f } };
        float surface = 0.0f, ground = 0.0f;

        if (!SurfaceAt(x, y, surface, &ground)) {
            return;
        }

        float depth = surface - ground;

        for (const auto& o : ring) {
            float s = 0.0f, g = 0.0f;

            if (SurfaceAt(x + o[0], y + o[1], s, &g)) {
                depth = std::fmax(depth, s - g);
            }
        }

        const float lift = ground + depth - z;

        // Only onto covers (a few yd at most): a unit high above, or bare ground, keeps its pose.
        if (lift <= 0.0f || lift > 3.0f) {
            return;
        }

        // The world's up in view space: the view's z row.
        pose.d0 += lift * view.c0;
        pose.d1 += lift * view.c1;
        pose.d2 += lift * view.c2;
    }

    void KeepCameraAbove(C3Vector& position) {
        constexpr float kAbove = 0.3f;
        float top = 0.0f, ground = 0.0f;

        if (SurfaceAt(position.x, position.y, top, &ground) && position.z >= ground - 0.1f && position.z < top + kAbove && top - ground > 0.005f) {
            position.z = top + kAbove;
        }
    }

    // The projected texture being drawn onto the covers (DecalBegin .. DecalEnd), or none.
    Decal s_decal;
    bool s_decalOn = false;

    bool DecalsOnCover() {
        return s_available && s_haveLook && s_look.enabled && s_look.tess_detail > 0.0f;
    }

    void DecalBegin(int kind, float r, float g, float b, float a) {
        s_decal.kind = kind;
        s_decal.colour[0] = r;
        s_decal.colour[1] = g;
        s_decal.colour[2] = b;
        s_decal.colour[3] = a;
        s_decal.debug = s_look.debug == 7;
        s_decalOn = true;
    }

    void DecalEnd() {
        s_decalOn = false;
    }

    const Decal* CurrentDecal() {
        return s_decalOn ? &s_decal : nullptr;
    }

    float SteadyWind() {
        return s_haveLook && s_look.enabled ? std::fmax(s_look.steady_wind, 0.0f) : 0.0f;
    }

    void BeforeChunkDraw(CMapRenderChunk* rc) {
        float constants[12] = {};

        if (rc && s_available && s_haveLook && s_look.enabled && s_resolve) {
            s_draws++;

            // The cache stays bounded (some thousand chunks stream in and out over a session).
            if (s_chunks.size() > 6000) {
                s_chunks.clear();
            }

            Cached& c = s_chunks[rc];
            const uint32_t layers = rc->m_layersCount < SXC_COVER_MAX_LAYERS ? rc->m_layersCount : SXC_COVER_MAX_LAYERS;
            bool same = c.generation == s_generation && c.layerCount == layers && c.chunks[0] == rc->m_mapChunks[0] && c.chunks[1] == rc->m_mapChunks[1];

            for (uint32_t l = 0; l < layers && same; l++) {
                same = c.textureIds[l] == rc->m_layers[l].m_textureId;
            }

            if (!same) {
                c = Cached{};
                c.generation = s_generation;
                c.layerCount = layers;
                c.chunks[0] = rc->m_mapChunks[0];
                c.chunks[1] = rc->m_mapChunks[1];

                for (uint32_t l = 0; l < layers; l++) {
                    c.textureIds[l] = rc->m_layers[l].m_textureId;
                }

                Resolve(rc, c);
            }

            for (int l = 0; l < 4; l++) {
                constants[l] = c.painted[l];
                constants[4 + l] = c.snow[l];
            }

            // The textures this chunk shows (for loading them and handing out the slots).
            for (int k = 0; k < 8; k++) {
                const uint32_t e = static_cast<uint32_t>(constants[k]);

                if (e >= 1 && e <= s_entries.size()) {
                    const int32_t id = s_entries[e - 1].texture;

                    if (id >= 1 && static_cast<uint32_t>(id) <= s_textureCount) {
                        s_textures[id - 1].uses++;
                        s_textures[id - 1].lastUsed = s_frameNo;
                    }
                }
            }

            s_withCover += (constants[0] + constants[1] + constants[2] + constants[3]) > 0.0f;

            // The chunk's water line beyond the near map: the lowest top of its liquids (none: far below).
            float surface = -100000.0f;

            for (CMapChunk* chunk : rc->m_mapChunks) {
                for (CChunkLiquid* liquid = chunk ? chunk->m_liquidChunkLinkList.Head() : nullptr; liquid; liquid = chunk->m_liquidChunkLinkList.Next(liquid)) {
                    if (std::isfinite(liquid->m_height.x) && (surface <= -100000.0f || liquid->m_height.x < surface)) {
                        surface = liquid->m_height.x;
                    }
                }
            }

            constants[9] = surface;

            // Depth where the chunk can have any: a painted cover with depth, or fallen snow around.
            if (s_look.tess_detail > 0.0f && s_look.depth_scale > 0.0f) {
                bool raised = s_look.fallen_snow && std::fmax(std::fmax(s_near.mostFallen, s_far.mostFallen), s_look.outside[0]) > 0.002f;

                // The deepest a painted cover's base lies below the terrain (mud): prints go down to it,
                // and the chunk's underlay sinks under it (Terrain.vert).
                float below = 0.0f;

                for (int l = 0; l < 4; l++) {
                    const uint32_t e = static_cast<uint32_t>(constants[l]);

                    if (e >= 1 && e <= s_entries.size() && s_entries[e - 1].depth > 0.0f) {
                        raised = true;
                        below = std::fmax(below, -s_entries[e - 1].z_offset);
                    }
                }

                constants[8] = raised ? 1.0f : 0.0f;
                constants[10] = raised && s_look.trails ? below : 0.0f;
            }
        }

        GxShaderConstantsSet(GxSh_Pixel, 11, constants, 3);
    }
}
