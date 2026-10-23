#ifndef ENGINE_SOURCE_RETAIL_LOCALSOURCE_H
#define ENGINE_SOURCE_RETAIL_LOCALSOURCE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "Engine/Source/Retail/SourceContext.h"

namespace Retail {

// Serves Retail (MD21) models from the client's own archives and loose files.
//
// A model is recognised by its header, whatever its path, so a Retail model exported into a patch
// MPQ at its normal name ("creature/foo/foo.m2") is picked up the moment the engine opens it. Its
// skins, external animations, skeleton and textures are referenced by FileDataID inside the file;
// those are found through a listfile ("<FileDataID>;<path>" rows) and read from the archives too.
// Skins, animations and the model's own skeleton fall back to the conventional names next to the
// model, so only textures and parent skeletons strictly need a listfile entry.
//
// Nothing is converted on disk: each file is prepared in memory when it is opened (RetailModel.cpp).
class LocalSource {
public:
    LocalSource();
    ~LocalSource();
    LocalSource(const LocalSource&) = delete;
    LocalSource& operator=(const LocalSource&) = delete;

    // True for "retail/..." names, for files a loaded Retail model has published, and for any .m2
    // whose header is MD21. Always false while the source itself is reading the archives.
    bool Handles(const char* name);
    bool Exists(const char* name, std::string& error);
    // The prepared (engine-ready) bytes of a handled file.
    bool Read(const char* name, std::vector<uint8_t>& bytes, std::string& error);
    // Prepared size once read, otherwise the size on disk.
    bool Size(const char* name, uint32_t& size);
    // Drops the published dependency names once the last CM2Shared using the model goes away.
    void ReleaseModel(const char* name);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

LocalSource& Source();
inline LocalSource& SourceForPath(const char*) { return Source(); }

}
#endif
