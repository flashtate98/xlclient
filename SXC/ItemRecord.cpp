// Item.dbc answered from the item cache.
//
// Item.dbc holds five things about an item -- class, subclass (and its sound override), material,
// display, inventory type, sheathe type -- and everything else comes from the server's item cache
// (ItemStats_C, filled by item queries). An item the server knows but Item.dbc does not (every
// custom item) loses those five, so its model, icon, slot and sheathe come out wrong or empty.
//
// tswow's item-dbc-disabler patch (kebabstorm, after BenjaminLSR and rajkosto) redirects about 15
// call sites to the cache. Here the one lookup every Item.dbc reader goes through,
// WowClientDB<ItemRec>::GetRecord, is specialised instead (declared in DBClient.h), so all ~47
// readers agree:
//
//   1. The item cache: the server's record, authoritative, custom items included. A missing entry
//      is queried from the server (a no-op callback asks for it, as the patch does).
//   2. Until the answer arrives, Item.dbc as stock, so stock items look right from the first frame.
//
// TODO(verify): transmog. Unconfirmed whether transmog systems still work with the display id coming
// from the item cache instead of Item.dbc (orchestration/docs/tracks/client/verification.md).
#include <unordered_map>
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/DBClient/DBCacheInstances.h"
#include "WoW/Source/Object/ItemStats.h"

namespace {
    // The cache only queries the server for a missing record when given a callback.
    void QueryOnly(int32_t id, const WOWGUID& guid, void* param, bool success) {}

    // One ItemRec per item id, rewritten from the cache on every lookup. Node-based, so a returned
    // pointer stays valid while other ids are added.
    std::unordered_map<int32_t, ItemRec> s_fromCache;
}

template <>
ItemRec* WowClientDB<ItemRec>::GetRecord(int32_t id) const {
    WOWGUID guid = WOWGUID();
    ItemStats_C* stats = g_itemDBCache.GetRecord(id, guid, QueryOnly, nullptr, true);

    if (stats) {
        ItemRec& rec = s_fromCache[id];
        rec.m_id = id;
        rec.m_classID = static_cast<int32_t>(stats->m_class);
        rec.m_subclassID = static_cast<int32_t>(stats->m_subClass);
        rec.m_soundOverrideSubclassID = static_cast<int32_t>(stats->m_soundOverrideSubclass);
        rec.m_material = static_cast<int32_t>(stats->m_material);
        rec.m_displayInfoID = static_cast<int32_t>(stats->m_displayId);
        rec.m_inventoryType = static_cast<int32_t>(stats->m_inventoryType);
        rec.m_sheatheType = static_cast<int32_t>(stats->m_sheatheType);
        return &rec;
    }

    // Stock (WowClientDB<T>::GetRecord).
    if (id < this->m_minID || id > this->m_maxID) {
        return nullptr;
    }

    return this->m_recordsById[id - this->m_minID];
}
