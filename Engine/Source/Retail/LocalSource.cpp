#include "Engine/Source/Retail/LocalSource.h"
#include "Engine/Source/Retail/RetailModel.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Retail {
namespace {

constexpr uint32_t kMaxFileSize = 128 * 1024 * 1024;
constexpr const char* kLog = "Logs/RetailModels.log";

// Set while this source reads the archives itself, so the SFile hooks treat those reads as
// ordinary files instead of calling back in here.
thread_local int t_bypass = 0;

struct Bypass {
    Bypass() { ++t_bypass; }
    ~Bypass() { --t_bypass; }
};

std::string Normalize(const char* name) {
    std::string key(name ? name : "");
    for (auto& c : key) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return key;
}

std::string ArchivePath(std::string path) {
    for (auto& c : path) if (c == '/') c = '\\';
    return path;
}

std::string Extension(const std::string& key) {
    const auto slash = key.find_last_of('/');
    const auto dot = key.find_last_of('.');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? "" : key.substr(dot);
}

std::string Stem(const std::string& key) {
    const auto ext = Extension(key);
    return key.substr(0, key.size() - ext.size());
}

bool Number(const std::string& text, uint32_t& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() && value;
}

bool Fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

// Reads a whole file through the client's normal lookup order (loose files, then patch MPQs).
bool RawRead(const std::string& path, std::vector<uint8_t>& bytes, std::string& error) {
    bytes.clear();
    Bypass bypass;
    SFileImpl* file = nullptr;
    if (!SFile::OpenEx(nullptr, ArchivePath(path).c_str(), 0, &file) || !file)
        return Fail(error, "cannot open " + path);
    uint32_t high = 0;
    const uint32_t size = SFileGetFileSize(file, &high);
    if (high || size > kMaxFileSize) {
        SFileCloseFile(file);
        return Fail(error, path + " is larger than 128 MiB");
    }
    bytes.resize(size);
    uint32_t read = 0;
    const bool ok = !size || (SFileReadFile(file, bytes.data(), size, &read, nullptr, 0) && read == size);
    SFileCloseFile(file);
    if (!ok) {
        bytes.clear();
        return Fail(error, "cannot read " + path);
    }
    return true;
}

// Reads only the first four bytes, without pulling the whole file into memory.
bool RawMagic(const std::string& path, uint32_t& magic, uint32_t& size) {
    Bypass bypass;
    SFileImpl* file = nullptr;
    if (!SFile::OpenEx(nullptr, ArchivePath(path).c_str(), 0, &file) || !file) return false;
    uint32_t high = 0;
    size = SFileGetFileSize(file, &high);
    uint32_t read = 0;
    magic = 0;
    const bool ok = !high && size >= 4 && SFileReadFile(file, &magic, 4, &read, nullptr, 0) && read == 4;
    SFileCloseFile(file);
    return ok;
}

bool RawExists(const std::string& path) {
    Bypass bypass;
    return SFile::FileExistsEx(ArchivePath(path).c_str(), 0) != 0;
}

struct Model {
    std::vector<uint8_t> bytes;
    ModelFiles files;
};

// What a handled name points at: a FileDataID (resolved through the listfile), a fallback path
// in the archives, or both. Skins and animations also remember the model they belong to.
struct Reference {
    uint32_t id = 0;
    std::string path;
    std::shared_ptr<Model> owner;
    uint16_t animation = 0;
    uint16_t variation = 0;
};

}

struct LocalSource::Impl {
    std::mutex mutex;
    bool listLoaded = false;
    std::unordered_map<uint32_t, std::string> listfile;
    std::unordered_map<std::string, bool> modelKinds;        // .m2 name -> header is MD21
    std::map<std::string, Reference> dependencies;           // published skin/anim names
    std::map<std::string, std::pair<std::shared_ptr<Model>, size_t>> models;
    std::map<std::string, uint32_t> preparedSizes;

    static std::filesystem::path ListfilePath() {
        if (const char* env = std::getenv("WOW_RETAIL_LISTFILE"); env && *env) return env;
#ifdef _WIN32
        char exe[MAX_PATH];
        const DWORD length = GetModuleFileNameA(nullptr, exe, sizeof(exe));
        if (length && length < sizeof(exe)) {
            const auto beside = std::filesystem::path(exe).parent_path() / "Data" / "Retail" / "listfile.csv";
            std::error_code code;
            if (std::filesystem::exists(beside, code)) return beside;
        }
#endif
        return std::filesystem::path("Data") / "Retail" / "listfile.csv";
    }

    // Called with the mutex held. Loads once; a missing listfile leaves only the fallbacks.
    void LoadListfile() {
        if (listLoaded) return;
        listLoaded = true;
        const auto path = ListfilePath();
        std::ifstream input(path);
        if (!input) {
            Blizzard::Log::Printf(1, kLog, "No listfile at %s: textures and parent skeletons cannot be resolved\n", path.string().c_str());
            return;
        }
        std::string line;
        size_t rows = 0;
        while (std::getline(input, line)) {
            const auto separator = line.find(';');
            if (separator == std::string::npos) continue;
            uint32_t id = 0;
            if (!Number(line.substr(0, separator), id)) continue;
            auto name = line.substr(separator + 1);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ' || name.back() == '\t')) name.pop_back();
            if (name.empty()) continue;
            listfile[id] = Normalize(name.c_str());
            ++rows;
        }
        Blizzard::Log::Printf(1, kLog, "Listfile %s: %u entries\n", path.string().c_str(), static_cast<unsigned>(rows));
    }

    // Called with the mutex held.
    bool Resolve(const std::string& key, Reference& ref, std::string& error) {
        const auto dep = dependencies.find(key);
        if (dep != dependencies.end()) {
            ref = dep->second;
            return true;
        }
        if (IsVirtualPath(key.c_str())) {
            const auto internal = key.substr(std::strlen(SourcePrefix()));
            const auto stem = internal.substr(0, internal.find('.'));
            if (Number(stem, ref.id)) return true;
            if (stem.find_first_not_of("0123456789") == std::string::npos) return Fail(error, "invalid FileDataID in " + key);
            ref.path = internal; // An embedded filename from an older model: read it as is.
            return true;
        }
        ref.path = key; // A Retail model opened by its normal path.
        return true;
    }

    // Called with the mutex held; only looks up paths.
    std::vector<std::string> Candidates(const Reference& ref) {
        std::vector<std::string> paths;
        if (ref.id) {
            LoadListfile();
            const auto found = listfile.find(ref.id);
            if (found != listfile.end()) paths.push_back(found->second);
        }
        if (!ref.path.empty() && std::find(paths.begin(), paths.end(), ref.path) == paths.end()) paths.push_back(ref.path);
        return paths;
    }

    std::vector<std::string> CandidatesLocked(const Reference& ref) {
        std::lock_guard<std::mutex> lock(mutex);
        return Candidates(ref);
    }

    // Called without the mutex: reads the archives.
    bool Fetch(const Reference& ref, std::vector<uint8_t>& bytes, std::string& error) {
        const auto paths = CandidatesLocked(ref);
        if (paths.empty()) return Fail(error, "FileDataID " + std::to_string(ref.id) + " is not in the listfile");
        std::string last;
        for (const auto& path : paths) {
            if (RawRead(path, bytes, last)) return true;
        }
        return Fail(error, ref.id ? "FileDataID " + std::to_string(ref.id) + ": " + last : last);
    }

    // Called without the mutex. Follows SKPD parents (by FileDataID) and merges them in.
    bool LoadSkeleton(const Reference& ref, std::vector<uint8_t>& bytes, std::string& error, std::vector<uint32_t>& chain) {
        if (chain.size() >= 8 || (ref.id && std::find(chain.begin(), chain.end(), ref.id) != chain.end()))
            return Fail(error, "skeleton parent cycle or depth limit");
        chain.push_back(ref.id);
        if (!Fetch(ref, bytes, error)) return false;
        const auto word = [&](size_t at) {
            return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) | (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
        };
        uint32_t parent = 0;
        bool found = false;
        for (size_t p = 0; p < bytes.size();) {
            if (bytes.size() - p < 8) return Fail(error, "truncated skeleton chunk header");
            const uint32_t tag = word(p), size = word(p + 4);
            p += 8;
            if (size > bytes.size() - p) return Fail(error, "skeleton chunk exceeds file size");
            if (tag == 0x44504b53) { // SKPD: the parent FileDataID is payload word 2.
                if (found || size < 12) return Fail(error, "invalid or duplicate skeleton parent chunk");
                parent = word(p + 8);
                found = true;
            }
            p += size;
        }
        if (parent) {
            std::vector<uint8_t> ancestor, merged;
            Reference up;
            up.id = parent;
            if (!LoadSkeleton(up, ancestor, error, chain) || !MergeSkeleton(bytes, ancestor, merged, error)) return false;
            bytes.swap(merged);
        }
        chain.pop_back();
        return true;
    }
};

LocalSource::LocalSource() : m_impl(new Impl) {}
LocalSource::~LocalSource() = default;

bool LocalSource::Handles(const char* name) {
    if (t_bypass || !name || !*name) return false;
    const auto key = Normalize(name);
    if (IsVirtualPath(key.c_str())) return true;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->dependencies.count(key) || m_impl->models.count(key)) return true;
        if (Extension(key) != ".m2") return false;
        const auto kind = m_impl->modelKinds.find(key);
        if (kind != m_impl->modelKinds.end()) return kind->second;
    }
    // First sight of this model name: look at its header outside the lock (this reads the archives).
    uint32_t magic = 0, size = 0;
    const bool retail = RawMagic(key, magic, size) && magic == 0x3132444d; // "MD21"
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->modelKinds[key] = retail;
    return retail;
}

bool LocalSource::Exists(const char* name, std::string& error) {
    error.clear();
    const auto key = Normalize(name);
    Reference ref;
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->preparedSizes.count(key)) return true;
        if (!m_impl->Resolve(key, ref, error)) return false;
        paths = m_impl->Candidates(ref);
    }
    for (const auto& path : paths) if (RawExists(path)) return true;
    return Fail(error, "not found: " + key);
}

bool LocalSource::Size(const char* name, uint32_t& size) {
    const auto key = Normalize(name);
    Reference ref;
    std::vector<std::string> paths;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto prepared = m_impl->preparedSizes.find(key);
        if (prepared != m_impl->preparedSizes.end()) {
            size = prepared->second;
            return true;
        }
        if (!m_impl->Resolve(key, ref, error)) return false;
        paths = m_impl->Candidates(ref);
    }
    for (const auto& path : paths) {
        uint32_t magic = 0;
        if (RawMagic(path, magic, size)) return true;
    }
    return false;
}

bool LocalSource::Read(const char* name, std::vector<uint8_t>& bytes, std::string& error) {
    error.clear();
    bytes.clear();
    const auto key = Normalize(name);
    auto& s = *m_impl;
    Reference ref;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.Resolve(key, ref, error)) return false;
    }
    if (!s.Fetch(ref, bytes, error)) return false;

    const auto ext = Extension(key);
    bool success = true;
    if (ext == ".m2" || ext == ".mdx") {
        auto model = std::make_shared<Model>();
        const auto base = Stem(key);
        // A shared skeleton lives in its own file; fetch it (and its parents) before preparing.
        ModelFiles inspected;
        std::vector<uint8_t> skeleton;
        if (InspectModel(bytes, inspected, error) && inspected.skeleton) {
            Reference skel;
            skel.id = inspected.skeleton;
            if (!IsVirtualPath(key.c_str())) skel.path = base + ".skel";
            std::vector<uint32_t> chain;
            success = s.LoadSkeleton(skel, skeleton, error, chain);
        }
        success = success && PrepareModel(bytes, model->bytes, model->files, error, skeleton.empty() ? nullptr : &skeleton);

        std::map<std::string, Reference> pending;
        const bool normalPath = !IsVirtualPath(key.c_str());
        // Check every advertised profile before exposing the model to the renderer.
        for (size_t i = 0; success && i < model->files.skins.size(); ++i) {
            if (i >= 4 || i >= (model->bytes.size() > 0x44 ? model->bytes[0x44] : 0)) break;
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "%02u.skin", static_cast<unsigned>(i));
            Reference skin;
            skin.id = model->files.skins[i];
            skin.owner = model;
            if (normalPath) skin.path = base + suffix;
            std::vector<uint8_t> raw;
            // Lenient materials: a multi-texture Retail shader the legacy renderer lacks is drawn with its
            // first texture instead of refusing the whole model (see PrepareSkin).
            success = s.Fetch(skin, raw, error) && PrepareSkin(raw, model->bytes, error, true);
            pending.emplace(base + suffix, std::move(skin));
        }
        for (const auto& anim : model->files.animations) {
            if (!anim.fileId) continue;
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "%04u-%02u.anim", anim.id, anim.variation);
            Reference file;
            file.id = anim.fileId;
            file.owner = model;
            file.animation = anim.id;
            file.variation = anim.variation;
            if (normalPath) file.path = base + suffix;
            const auto added = pending.emplace(base + suffix, file);
            if (!added.second && added.first->second.id != anim.fileId) success = Fail(error, "conflicting AFID entries");
        }
        if (success) {
            std::lock_guard<std::mutex> lock(s.mutex);
            // The same name always denotes the same file; keep the first prepared copy.
            auto& owner = s.models[key];
            if (!owner.first) owner.first = model;
            ++owner.second;
            for (auto& dep : pending) {
                dep.second.owner = owner.first;
                s.dependencies[dep.first] = dep.second;
            }
            bytes = owner.first->bytes;
        }
    } else if (ext == ".skin") {
        success = ref.owner != nullptr;
        if (!success) error = "open the Retail model before its skin";
        success = success && PrepareSkin(bytes, ref.owner->bytes, error, true);
    } else if (ext == ".anim") {
        success = ref.owner ? (StripAnimationChunk(bytes, error) &&
            ValidateAnimation(bytes, ref.owner->bytes, ref.animation, ref.variation, error)) :
            Fail(error, "open the Retail model before its animation");
    } else if (ext == ".blp") {
        // BLP2 format version 1 is the client's native texture format.
        success = bytes.size() >= 148 && std::memcmp(bytes.data(), "BLP2\1\0\0\0", 8) == 0;
        if (!success) error = "texture is not a supported BLP2 file";
    } else {
        success = Fail(error, "Retail models reference only M2, SKIN, ANIM and BLP files");
    }

    if (!success) {
        bytes.clear();
        Blizzard::Log::Printf(1, kLog, "%s: %s\n", key.c_str(), error.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    s.preparedSizes[key] = static_cast<uint32_t>(bytes.size());
    return true;
}

void LocalSource::ReleaseModel(const char* name) {
    const auto key = Normalize(name);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto model = m_impl->models.find(key);
    if (model == m_impl->models.end() || --model->second.second) return;
    for (auto it = m_impl->dependencies.begin(); it != m_impl->dependencies.end();) {
        if (it->second.owner == model->second.first) {
            m_impl->preparedSizes.erase(it->first);
            it = m_impl->dependencies.erase(it);
        } else {
            ++it;
        }
    }
    m_impl->preparedSizes.erase(key);
    m_impl->models.erase(model);
}

LocalSource& Source() {
    static LocalSource source;
    return source;
}

}
