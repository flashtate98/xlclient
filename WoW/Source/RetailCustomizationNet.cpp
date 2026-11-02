#if defined(WOWDECOMP_RETAIL_MODELS)

#include "WoW/Source/RetailCustomizationNet.h"
#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include <string>
#include <unordered_map>
#include "WoW/Common/ComponentCore/RetailCharacter.h"
#include "WoW/Source/Glue/CharacterCreation.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/ClientServices.h"
#include "WoW/Source/Object/ObjectClient/AlteredForm.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"

namespace {

constexpr uint32_t kMagic = 0x434C5452;      // "RTLC"
constexpr uint32_t kFormMagic = 0x464F5754;  // "TWOF"
constexpr uint8_t kMaxChoices = 64;
constexpr uint32_t kSavedFormOption = 0xFFFFFF01;  // not a Retail option: the character's Two Forms form

std::unordered_map<uint64_t, uint32_t> s_savedForms;
std::unordered_map<uint64_t, RetailChoiceList> s_formChoices;    // a form change's choices, waiting for its flag

// FlashWOW Two Forms: whether a player in the world still shows the other form than `form` (the server's form
// change sends the new form's choices before the field update that changes the form).
bool AwaitsForm(uint64_t guid, uint32_t form) {
    CGPlayer_C* player = static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(WOWGUID{ guid }, TYPEMASK_PLAYER, __FILE__, __LINE__));
    if (!player || !AlteredFormRace(player->m_unit->m_unitFieldBytes0[0])) return false;
    return ((player->m_unit->m_unitFieldFlags2 & kAlteredFormFlag2) != 0) != (form != 0);
}

uint32_t Remaining(const CDataStore* msg) {
    return msg->m_size > msg->m_read ? msg->m_size - msg->m_read : 0;
}

int32_t OnChoices(void* param, NETMESSAGE msgId, uint32_t time, CDataStore* msg) {
    if (Remaining(msg) < 2) return 1;
    uint16_t entries = 0;
    msg->Get(entries);
    RetailChoiceList choices;
    for (uint16_t entry = 0; entry < entries; ++entry) {
        if (Remaining(msg) < 9) break;
        uint64_t guid = 0;
        uint8_t count = 0;
        msg->Get(guid);
        msg->Get(count);
        if (Remaining(msg) < static_cast<uint32_t>(count) * 8) break;
        choices.clear();
        s_savedForms.erase(guid);     // the character list and form changes name the form; anything else means none known
        bool named = false;
        for (uint8_t i = 0; i < count; ++i) {
            uint32_t option = 0, choice = 0;
            msg->Get(option);
            msg->Get(choice);
            if (option == kSavedFormOption) {
                s_savedForms[guid] = choice;
                named = true;
                continue;
            }
            choices.emplace_back(option, choice);
        }
        if (named && AwaitsForm(guid, s_savedForms[guid])) {
            s_formChoices[guid] = choices;
            continue;
        }
        s_formChoices.erase(guid);
        RetailCustomizationStore(guid, choices);
    }
    return 1;
}

}

void RetailCustomizationNetInitialize() {
    ClientServices::SetMessageHandler(static_cast<NETMESSAGE>(CMSG_UNUSED6), OnChoices, nullptr);
}

void RetailCustomizationNetAppendCreate(CDataStore& msg) {
    RetailCustomizationNetAppendChoices(msg, CCharacterCreation::m_character);

    // FlashWOW Two Forms: the altered form's look, when the player edited it.
    uint8_t look[5] = {};
    RetailCustomizationExtras extras;
    if (!CharacterCreationAlteredFormLook(look, extras)) return;
    if (extras.size() > kMaxChoices) extras.resize(kMaxChoices);
    Blizzard::Log::Printf(1, "Logs/RetailCharacter.log", "Sending the altered form: %u %u %u %u %u, %u choices\n",
        look[0], look[1], look[2], look[3], look[4], static_cast<unsigned>(extras.size()));
    msg.Put(kFormMagic);
    for (uint8_t value : look) msg.Put(value);
    msg.Put(static_cast<uint8_t>(extras.size()));
    for (const auto& choice : extras) {
        msg.Put(choice.first);
        msg.Put(choice.second);
    }
}

void RetailCustomizationNetAppendChoices(CDataStore& msg, const CCharacterComponent* character) {
    RetailChoiceList choices;
    if (!character || !RetailCustomizationChoices(character, choices) || choices.empty()) return;
    std::string text;
    for (const auto& choice : choices) text += " " + std::to_string(choice.first) + "=" + std::to_string(choice.second);
    Blizzard::Log::Printf(1, "Logs/RetailCharacter.log", "Sending choices:%s\n", text.c_str());
    if (choices.size() > kMaxChoices) choices.resize(kMaxChoices);
    msg.Put(kMagic);
    msg.Put(static_cast<uint8_t>(choices.size()));
    for (const auto& choice : choices) {
        msg.Put(choice.first);
        msg.Put(choice.second);
    }
}

void RetailCustomizationNetFormChanged(uint64_t guid) {
    const auto found = s_formChoices.find(guid);
    if (found == s_formChoices.end()) return;
    const RetailChoiceList choices = std::move(found->second);
    s_formChoices.erase(found);
    RetailCustomizationStore(guid, choices);
}

uint32_t RetailCustomizationSavedForm(uint64_t guid) {
    const auto found = s_savedForms.find(guid);
    return found != s_savedForms.end() ? found->second : 0;
}

void RetailCustomizationNetBind(const CCharacterComponent* character, uint64_t guid, bool ask) {
    if (!RetailCustomizationBind(character, guid) || !ask) return;
    CDataStore msg;
    msg.Put(static_cast<uint32_t>(CMSG_UNUSED5));
    msg.Put(static_cast<uint8_t>(1));
    msg.Put(guid);
    msg.Finalize();
    ClientServices::SendPacket(&msg);
}

#endif
