#ifndef WOW_SOURCE_RETAILCUSTOMIZATIONNET_H
#define WOW_SOURCE_RETAILCUSTOMIZATIONNET_H

#include <cstdint>

class CDataStore;
class CCharacterComponent;

// Not in the original: carries Retail customization choices (RetailCharacter.h) between the client
// and an AzerothCore server running mod-retail-customization.
//
//   CMSG_CHAR_CREATE and CMSG_ALTER_APPEARANCE get a block after the stock fields:
//       u32 'RTLC' (0x434C5452), u8 count, count x (u32 optionId, u32 choiceId)
//   CMSG_UNUSED5 (0x4B8) asks for characters' choices, in world:
//       u8 count, count x u64 guid
//   0x4B9 (CMSG_UNUSED6's number, which the server never receives) answers, and also arrives with
//   the character list and after barber shop visits:
//       u16 entries, entries x (u64 guid, u8 count, count x (u32 optionId, u32 choiceId))
//
// A server without the module ignores the trailing block; it logs the query opcode as unhandled.

// Registers the 0x4B9 handler. Called once from InitializeClient.
void RetailCustomizationNetInitialize();

// Appends the create screen character's block to a CMSG_CHAR_CREATE (before Finalize). Nothing for
// stock bodies.
void RetailCustomizationNetAppendCreate(CDataStore& msg);

// Appends a component's block to a CMSG_ALTER_APPEARANCE (or any message the server reads it from).
void RetailCustomizationNetAppendChoices(CDataStore& msg, const CCharacterComponent* character);

// Binds a component to the character it shows and, when `ask` is set and its choices are not known,
// asks the server for them.
void RetailCustomizationNetBind(const CCharacterComponent* character, uint64_t guid, bool ask);

#endif
