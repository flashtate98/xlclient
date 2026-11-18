#include "Engine/Source/Retail/RetailWmo.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace Retail {
namespace {

// A chunk tag as the file stores it (bytes reversed: "MVER" is written "REVM").
constexpr uint32_t Tag(const char (&name)[5]) {
    return (uint32_t(uint8_t(name[0])) << 24) | (uint32_t(uint8_t(name[1])) << 16) | (uint32_t(uint8_t(name[2])) << 8) | uint32_t(uint8_t(name[3]));
}

std::string TagName(uint32_t tag) {
    return std::string{char(tag >> 24), char(tag >> 16), char(tag >> 8), char(tag)};
}

struct Chunk {
    uint32_t tag;
    const uint8_t* data;
    uint32_t size;
};

bool Fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

uint32_t U32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t U16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void Put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }

// With `notes`, a chunk whose size runs past the end is cut at the end, as 3.3.5's positional reader in
// effect does (a retroported group whose MOGP size was 96 bytes too large loaded in 3.3.5, 2026-11-18).
bool Walk(const uint8_t* data, size_t size, std::vector<Chunk>& chunks, std::string& error, std::vector<std::string>* notes = nullptr) {
    for (size_t p = 0; p < size;) {
        if (size - p < 8) {
            if (notes && !chunks.empty()) {
                notes->push_back(std::to_string(size - p) + " stray bytes at the end ignored");
                return true;
            }
            return Fail(error, "truncated chunk header at " + std::to_string(p));
        }
        const uint32_t tag = U32(data + p);
        uint32_t length = U32(data + p + 4);
        p += 8;
        if (length > size - p) {
            if (!notes) return Fail(error, TagName(tag) + " runs past the end of the file");
            notes->push_back(TagName(tag) + " says " + std::to_string(length) + " bytes, " + std::to_string(size - p) + " are there; cut at the end");
            length = uint32_t(size - p);
        }
        chunks.push_back({tag, data + p, length});
        p += length;
    }
    return true;
}

const Chunk* Find(const std::vector<Chunk>& chunks, uint32_t tag, size_t nth = 0) {
    for (const auto& chunk : chunks) {
        if (chunk.tag == tag && nth-- == 0) return &chunk;
    }
    return nullptr;
}

struct Writer {
    std::vector<uint8_t>& out;

    size_t Begin(uint32_t tag) {
        const size_t at = out.size();
        out.resize(at + 8);
        Put32(out.data() + at, tag);
        return at;
    }
    void End(size_t at) { Put32(out.data() + at + 4, uint32_t(out.size() - at - 8)); }
    void Bytes(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        out.insert(out.end(), p, p + size);
    }
    void Chunk(uint32_t tag, const void* data, size_t size) {
        const size_t at = Begin(tag);
        Bytes(data, size);
        End(at);
    }
    void Chunk(uint32_t tag, const std::vector<uint8_t>& data) { Chunk(tag, data.data(), data.size()); }
    void Copy(const Retail::Chunk* chunk, uint32_t tag) { chunk ? Chunk(tag, chunk->data, chunk->size) : Chunk(tag, nullptr, 0); }
};

// A string table as 3.3.5 reads it: offset 0 is an empty name (a material's "no texture"), each name
// null-terminated and padded to 4 bytes.
struct NameTable {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(4, 0);
    std::map<std::string, uint32_t> offsets;

    uint32_t Add(const std::string& name) {
        if (name.empty()) return 0;
        const auto found = offsets.find(name);
        if (found != offsets.end()) return found->second;
        const uint32_t at = uint32_t(bytes.size());
        bytes.insert(bytes.end(), name.begin(), name.end());
        bytes.push_back(0);
        while (bytes.size() % 4) bytes.push_back(0);
        offsets[name] = at;
        return at;
    }
};

std::string ArchiveName(std::string path) {
    for (auto& c : path) if (c == '/') c = '\\';
    return path;
}

// Chunks of retail roots and groups 3.3.5 has no use for yet, named so the log says what isn't drawn.
// TODO(modern-wmos): MNLD/MNLR dynamic lights (our point lights), MAVG/MAVD/MAVR ambient volumes, MOLV,
// MFED, MGI2, MDDI/MDDL, MOUV (UV animation), MOMX, MOPE, MPVD/MBVD and their group refs, MOBS, MDAL,
// MOGX/MOQG (query faces), MOLP/MOLS/MLSS/MLSP/MLSK (light points and shadows), MOTA, MFVR, MOP2.
const std::set<uint32_t> kRootCarried = {Tag("MVER"), Tag("MOHD"), Tag("MOTX"), Tag("MOMT"), Tag("MOGN"), Tag("MOGI"), Tag("MOSB"),
    Tag("MOSI"), Tag("MOPV"), Tag("MOPT"), Tag("MOPR"), Tag("MOVV"), Tag("MOVB"), Tag("MOLT"), Tag("MODS"), Tag("MODN"), Tag("MODI"),
    Tag("MODD"), Tag("MFOG"), Tag("MCVP"), Tag("GFID")};
const std::set<uint32_t> kGroupCarried = {Tag("MOPY"), Tag("MPY2"), Tag("MOVI"), Tag("MOVX"), Tag("MOVT"), Tag("MONR"), Tag("MOTV"),
    Tag("MOBA"), Tag("MOLR"), Tag("MODR"), Tag("MOBN"), Tag("MOBR"), Tag("MOCV"), Tag("MLIQ"), Tag("MORI"), Tag("MORB")};

void NoteLeftOut(const std::vector<Chunk>& chunks, const std::set<uint32_t>& carried, std::vector<std::string>& notes) {
    std::set<std::string> left;
    for (const auto& chunk : chunks) if (!carried.count(chunk.tag)) left.insert(TagName(chunk.tag));
    if (left.empty()) return;
    std::string list;
    for (const auto& name : left) list += (list.empty() ? "" : " ") + name;
    notes.push_back("not read yet: " + list);
}

// Group flags and the optional chunks they announce, in the order 3.3.5 reads them
// (CMapObjGroup::CreateOptionalDataPointers).
constexpr uint32_t kHasLights = 0x200, kHasDoodads = 0x800, kHasBsp = 0x1, kHasMpb = 0x400, kHasColors = 0x4, kHasLiquid = 0x1000,
    kHasStrips = 0x20000, kHasColors2 = 0x1000000, kHasUv2 = 0x2000000, kHasUv3 = 0x40000000;

}

bool IsRetailWmoRoot(const std::vector<uint8_t>& bytes) {
    std::vector<Chunk> chunks;
    std::string error;
    std::vector<std::string> notes;
    return Walk(bytes.data(), bytes.size(), chunks, error, &notes) && Find(chunks, Tag("MOHD")) && Find(chunks, Tag("GFID"));
}

bool PrepareWmoRoot(const std::vector<uint8_t>& raw, const PathOfFileDataId& pathOf, WmoRoot& root, std::string& error) {
    std::vector<Chunk> chunks;
    if (!Walk(raw.data(), raw.size(), chunks, error, &root.notes)) return false;
    const Chunk* mohd = Find(chunks, Tag("MOHD"));
    const Chunk* momt = Find(chunks, Tag("MOMT"));
    const Chunk* mogi = Find(chunks, Tag("MOGI"));
    if (!mohd || mohd->size < 64) return Fail(error, "no MOHD");
    if (!momt || momt->size % 64) return Fail(error, "no MOMT, or not 64-byte materials");
    if (!mogi || mogi->size % 32) return Fail(error, "no MOGI, or not 32-byte group infos");
    NoteLeftOut(chunks, kRootCarried, root.notes);

    const auto resolve = [&](uint32_t id, const char* what) -> std::string {
        if (!id) return {};
        std::string path;
        if (pathOf(id, path) && !path.empty()) return ArchiveName(path);
        root.notes.push_back(std::string(what) + " FileDataID " + std::to_string(id) + " has no path in the listfile");
        return {};
    };

    // Materials: since Legion the three texture fields are FileDataIDs and there is no MOTX. 3.3.5 wants
    // offsets into a name table.
    std::vector<uint8_t> materials(momt->data, momt->data + momt->size);
    std::vector<uint8_t> textureNames;
    if (const Chunk* motx = Find(chunks, Tag("MOTX"))) {
        textureNames.assign(motx->data, motx->data + motx->size);
    } else {
        NameTable names;
        for (size_t m = 0; m < materials.size(); m += 64) {
            for (size_t field : {size_t(0x0C), size_t(0x18), size_t(0x24)}) {
                Put32(materials.data() + m + field, names.Add(resolve(U32(materials.data() + m + field), "texture")));
            }
            // TODO(modern-wmos): shader 23's further textures (run-time data words) aren't read; 3.3.5 keeps its
            // texture pointers in those words.
        }
        textureNames = std::move(names.bytes);
    }
    if (materials.size() / 64 > 255) {
        root.notes.push_back(std::to_string(materials.size() / 64) + " materials; 3.3.5 batches and faces address 255");
    }

    // Doodads: retail names them by FileDataID (MODI), and MODD's name field is an index into it.
    std::vector<uint8_t> doodadNames, doodadDefs;
    const Chunk* modd = Find(chunks, Tag("MODD"));
    if (modd) doodadDefs.assign(modd->data, modd->data + modd->size);
    if (const Chunk* modn = Find(chunks, Tag("MODN"))) {
        doodadNames.assign(modn->data, modn->data + modn->size);
    } else if (const Chunk* modi = Find(chunks, Tag("MODI"))) {
        NameTable names;
        std::vector<uint32_t> offsets;
        for (size_t i = 0; i + 4 <= modi->size; i += 4) offsets.push_back(names.Add(resolve(U32(modi->data + i), "doodad")));
        for (size_t d = 0; d + 40 <= doodadDefs.size(); d += 40) {
            const uint32_t word = U32(doodadDefs.data() + d), index = word & 0xFFFFFF;
            Put32(doodadDefs.data() + d, (word & 0xFF000000) | (index < offsets.size() ? offsets[index] : 0));
        }
        doodadNames = std::move(names.bytes);
    } else {
        doodadNames.assign(4, 0);
    }

    // The skybox: a name (MOSB), or since Legion a FileDataID (MOSI).
    std::vector<uint8_t> skybox(4, 0);
    if (const Chunk* mosb = Find(chunks, Tag("MOSB"))) {
        skybox.assign(mosb->data, mosb->data + mosb->size);
    } else if (const Chunk* mosi = Find(chunks, Tag("MOSI")); mosi && mosi->size >= 4) {
        const std::string name = resolve(U32(mosi->data), "skybox");
        skybox.assign(name.begin(), name.end());
        skybox.push_back(0);
        while (skybox.size() % 4) skybox.push_back(0);
    }

    if (const Chunk* gfid = Find(chunks, Tag("GFID"))) {
        // One entry per group, per LOD set after the first; the first set is the groups 3.3.5 draws.
        const size_t groups = mogi->size / 32;
        for (size_t i = 0; i < groups && (i + 1) * 4 <= gfid->size; i++) root.groupFiles.push_back(U32(gfid->data + i * 4));
    }

    Writer w{root.bytes};
    root.bytes.clear();
    const uint32_t version = 17;
    w.Chunk(Tag("MVER"), &version, 4);
    w.Copy(mohd, Tag("MOHD"));
    w.Chunk(Tag("MOTX"), textureNames);
    w.Chunk(Tag("MOMT"), materials);
    w.Copy(Find(chunks, Tag("MOGN")), Tag("MOGN"));
    w.Copy(mogi, Tag("MOGI"));
    w.Chunk(Tag("MOSB"), skybox);
    w.Copy(Find(chunks, Tag("MOPV")), Tag("MOPV"));
    w.Copy(Find(chunks, Tag("MOPT")), Tag("MOPT"));
    w.Copy(Find(chunks, Tag("MOPR")), Tag("MOPR"));
    w.Copy(Find(chunks, Tag("MOVV")), Tag("MOVV"));
    w.Copy(Find(chunks, Tag("MOVB")), Tag("MOVB"));
    w.Copy(Find(chunks, Tag("MOLT")), Tag("MOLT"));
    w.Copy(Find(chunks, Tag("MODS")), Tag("MODS"));
    w.Chunk(Tag("MODN"), doodadNames);
    w.Chunk(Tag("MODD"), doodadDefs);
    w.Copy(Find(chunks, Tag("MFOG")), Tag("MFOG"));
    if (const Chunk* mcvp = Find(chunks, Tag("MCVP"))) w.Copy(mcvp, Tag("MCVP"));
    return true;
}

void EmptyWmoGroup(std::vector<uint8_t>& bytes) {
    bytes.clear();
    Writer out{bytes};
    const uint32_t version = 17;
    out.Chunk(Tag("MVER"), &version, 4);
    const size_t at = out.Begin(Tag("MOGP"));
    const std::vector<uint8_t> header(0x44, 0);
    out.Bytes(header.data(), header.size());
    for (const uint32_t tag : {Tag("MOPY"), Tag("MOVI"), Tag("MOVT"), Tag("MONR"), Tag("MOTV"), Tag("MOBA")}) out.Chunk(tag, nullptr, 0);
    out.End(at);
}

bool PrepareWmoGroup(const std::vector<uint8_t>& raw, WmoGroup& group, std::string& error) {
    std::vector<Chunk> top;
    if (!Walk(raw.data(), raw.size(), top, error, &group.notes)) return false;
    const Chunk* mogp = Find(top, Tag("MOGP"));
    if (!mogp || mogp->size < 0x44) return Fail(error, "no MOGP");
    std::vector<Chunk> chunks;
    if (!Walk(mogp->data + 0x44, mogp->size - 0x44, chunks, error, &group.notes)) return false;
    NoteLeftOut(chunks, kGroupCarried, group.notes);

    std::vector<uint8_t> header(mogp->data, mogp->data + 0x44);
    uint32_t flags = U32(header.data() + 8);

    // Faces: MPY2 (since Dragonflight) has 16-bit flags and material ids; 3.3.5 reads 8 bits of each.
    std::vector<uint8_t> faces;
    if (const Chunk* mopy = Find(chunks, Tag("MOPY"))) {
        faces.assign(mopy->data, mopy->data + mopy->size);
    } else if (const Chunk* mpy2 = Find(chunks, Tag("MPY2"))) {
        size_t clamped = 0;
        for (size_t i = 0; i + 4 <= mpy2->size; i += 4) {
            const uint16_t faceFlags = U16(mpy2->data + i), material = U16(mpy2->data + i + 2);
            faces.push_back(uint8_t(faceFlags));
            if (material != 0xFFFF && material > 0xFE) clamped++;
            faces.push_back(material == 0xFFFF ? 0xFF : material > 0xFE ? 0 : uint8_t(material));
        }
        if (clamped) group.notes.push_back(std::to_string(clamped) + " faces with a material past 254 drawn with material 0");
    } else {
        // A group without geometry (only portals, MOPL): written with empty geometry chunks.
        group.notes.push_back("no faces (a group without geometry)");
    }

    // Indices: MOVX (32-bit) where a group has more than 65,536 vertices; 3.3.5 indexes with 16 bits.
    std::vector<uint8_t> indices;
    if (const Chunk* movi = Find(chunks, Tag("MOVI"))) {
        indices.assign(movi->data, movi->data + movi->size);
    } else if (const Chunk* movx = Find(chunks, Tag("MOVX"))) {
        for (size_t i = 0; i + 4 <= movx->size; i += 4) {
            const uint32_t index = U32(movx->data + i);
            if (index > 0xFFFF) return Fail(error, "an index past 65,535 (MOVX); 3.3.5 groups index with 16 bits");
            indices.push_back(uint8_t(index));
            indices.push_back(uint8_t(index >> 8));
        }
    }

    const Chunk* movt = Find(chunks, Tag("MOVT"));
    if (movt && movt->size % 12) return Fail(error, "MOVT is not 12-byte vertices");
    const size_t vertexCount = movt ? movt->size / 12 : 0;
    if (faces.size() / 2 != indices.size() / 6) return Fail(error, std::to_string(faces.size() / 2) + " faces for " + std::to_string(indices.size() / 6) + " triangles");
    if (vertexCount > 0x10000) return Fail(error, std::to_string(vertexCount) + " vertices; 3.3.5 groups index with 16 bits");

    // Batches: since Legion the first 10 bytes are no bounding box and a material past 255 sits at byte 10
    // (batch flag 0x2). 3.3.5 culls batches by their box: computed from the batch's vertices.
    std::vector<uint8_t> batches;
    if (const Chunk* moba = Find(chunks, Tag("MOBA"))) batches.assign(moba->data, moba->data + moba->size);
    size_t largeMaterials = 0;
    for (size_t b = 0; b + 24 <= batches.size(); b += 24) {
        uint8_t* batch = batches.data() + b;
        const uint16_t first = U16(batch + 18), last = U16(batch + 20);
        float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (size_t v = first; v <= last && v < vertexCount; v++) {
            for (int k = 0; k < 3; k++) {
                float c;
                std::memcpy(&c, movt->data + v * 12 + k * 4, 4);
                lo[k] = std::min(lo[k], c);
                hi[k] = std::max(hi[k], c);
            }
        }
        const uint8_t batchFlags = batch[22];
        const uint16_t material = (batchFlags & 0x2) ? U16(batch + 10) : batch[23];
        for (int k = 0; k < 3; k++) {
            const bool any = lo[k] <= hi[k];
            Put16(batch + k * 2, uint16_t(int16_t(any ? std::max(-32768.0f, std::floor(lo[k])) : 0)));
            Put16(batch + 6 + k * 2, uint16_t(int16_t(any ? std::min(32767.0f, std::ceil(hi[k])) : 0)));
        }
        if (material > 0xFF) largeMaterials++;
        batch[23] = material > 0xFF ? 0 : uint8_t(material);
        batch[22] = batchFlags & ~0x2;
    }
    if (largeMaterials) group.notes.push_back(std::to_string(largeMaterials) + " batches with a material past 255 drawn with material 0");

    // The optional chunks, written only where both the flag and the chunk are there (3.3.5 reads by flag).
    const auto keep = [&](uint32_t flag, bool present) { if (!present) flags &= ~flag; return present && (flags & flag); };
    flags &= ~kHasMpb;   // MPBV/MPBP/MPBI/MPBG: not in retail files
    if (flags & kHasUv3) {
        flags &= ~kHasUv3;
        group.notes.push_back("a third UV set (TODO: shaders that read it)");
    }

    std::vector<uint8_t> body;
    Writer w{body};
    w.Chunk(Tag("MOPY"), faces);
    w.Chunk(Tag("MOVI"), indices);
    w.Copy(movt, Tag("MOVT"));
    w.Copy(Find(chunks, Tag("MONR")), Tag("MONR"));
    w.Copy(Find(chunks, Tag("MOTV")), Tag("MOTV"));
    w.Chunk(Tag("MOBA"), batches);
    if (keep(kHasLights, Find(chunks, Tag("MOLR")) != nullptr)) w.Copy(Find(chunks, Tag("MOLR")), Tag("MOLR"));
    if (keep(kHasDoodads, Find(chunks, Tag("MODR")) != nullptr)) w.Copy(Find(chunks, Tag("MODR")), Tag("MODR"));
    if (keep(kHasBsp, Find(chunks, Tag("MOBN")) && Find(chunks, Tag("MOBR")))) {
        w.Copy(Find(chunks, Tag("MOBN")), Tag("MOBN"));
        w.Copy(Find(chunks, Tag("MOBR")), Tag("MOBR"));
    }
    // Vertex colours: with flag 0x1000000 but not 0x4 a group's only MOCV is the second set (whose alpha blends
    // the two-layer shaders' textures); 3.3.5 reads that case too (base colour, second set).
    const Chunk* colors = Find(chunks, Tag("MOCV"));
    const Chunk* colors2 = Find(chunks, Tag("MOCV"), 1);
    if (!(flags & kHasColors) && (flags & kHasColors2)) {
        colors2 = colors;
        colors = nullptr;
    }
    if (keep(kHasColors, colors != nullptr)) w.Copy(colors, Tag("MOCV"));
    if (keep(kHasLiquid, Find(chunks, Tag("MLIQ")) != nullptr)) w.Copy(Find(chunks, Tag("MLIQ")), Tag("MLIQ"));
    if (keep(kHasStrips, Find(chunks, Tag("MORI")) && Find(chunks, Tag("MORB")))) {
        w.Copy(Find(chunks, Tag("MORI")), Tag("MORI"));
        w.Copy(Find(chunks, Tag("MORB")), Tag("MORB"));
    }
    if (keep(kHasUv2, Find(chunks, Tag("MOTV"), 1) != nullptr)) w.Copy(Find(chunks, Tag("MOTV"), 1), Tag("MOTV"));
    if (keep(kHasColors2, colors2 != nullptr)) w.Copy(colors2, Tag("MOCV"));
    Put32(header.data() + 8, flags);

    group.bytes.clear();
    Writer out{group.bytes};
    const uint32_t version = 17;
    out.Chunk(Tag("MVER"), &version, 4);
    const size_t at = out.Begin(Tag("MOGP"));
    out.Bytes(header.data(), header.size());
    out.Bytes(body.data(), body.size());
    out.End(at);
    return true;
}

}
