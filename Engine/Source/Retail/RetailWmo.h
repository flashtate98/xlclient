#ifndef ENGINE_SOURCE_RETAIL_RETAILWMO_H
#define ENGINE_SOURCE_RETAIL_RETAILWMO_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Retail {

// Retail WMOs (Legion on) for the 3.3.5 map object code, prepared in memory like the Retail models: the
// bytes on disk are never changed. 3.3.5 reads a root and a group by walking their chunks in a fixed order
// (CMapObj::Load, CMapObjGroup::CreateDataPointers), so a retail file, whose chunks differ, is rebuilt into
// that order: names resolved from FileDataIDs, wider ids narrowed, chunks 3.3.5 can't use left out and
// listed. Each left-out thing is a stand-in to replace (kb: modern-wmos), not a conversion we keep.

// A retail root: it lists its groups by FileDataID (GFID), which no 3.3.5 root has.
bool IsRetailWmoRoot(const std::vector<uint8_t>& bytes);

using PathOfFileDataId = std::function<bool(uint32_t id, std::string& path)>;

struct WmoRoot {
    std::vector<uint8_t> bytes;          // in 3.3.5's chunk order
    std::vector<uint32_t> groupFiles;    // the groups' FileDataIDs, in group order (0: none)
    std::vector<std::string> notes;      // what isn't carried yet, and names not resolved
};

bool PrepareWmoRoot(const std::vector<uint8_t>& raw, const PathOfFileDataId& pathOf, WmoRoot& root, std::string& error);

struct WmoGroup {
    std::vector<uint8_t> bytes;          // in 3.3.5's chunk order
    std::vector<std::string> notes;
};

bool PrepareWmoGroup(const std::vector<uint8_t>& raw, WmoGroup& group, std::string& error);

}
#endif
