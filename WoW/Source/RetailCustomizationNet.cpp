#if defined(WOWDECOMP_RETAIL_MODELS)

#include "WoW/Source/RetailCustomizationNet.h"
#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include <string>
#include "WoW/Common/ComponentCore/RetailCharacter.h"
#include "WoW/Source/Glue/CharacterCreation.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/ClientServices.h"

namespace {

constexpr uint32_t kMagic = 0x434C5452;      // "RTLC"
constexpr uint8_t kMaxChoices = 64;

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
        for (uint8_t i = 0; i < count; ++i) {
            uint32_t option = 0, choice = 0;
            msg->Get(option);
            msg->Get(choice);
            choices.emplace_back(option, choice);
        }
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
