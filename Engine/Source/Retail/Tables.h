#ifndef ENGINE_SOURCE_RETAIL_TABLES_H
#define ENGINE_SOURCE_RETAIL_TABLES_H

#include <cstdint>
#include <string>
#include <string_view>

namespace Retail::Db2 { class Table; }

// The Retail customization tables (ChrModel, ChrCustomization*, ChrModelTextureLayer, ...), read
// once from DBFilesClient\<name>.db2 in the client's own data (loose or patch MPQs). They load on
// first use, and every table's row count, time or error is written to Logs\RetailTables.log.
namespace Retail::Db2::tables
{
    struct Status {
        bool loaded = false;   // every table decoded
        uint32_t tables = 0;   // tables decoded
        uint32_t failed = 0;
        uint32_t milliseconds = 0;
        std::string error;     // the first failure
    };

    const Status& GetStatus();
    // A loaded table by its lower-case name ("chrmodel", "chrcustomizationchoice", ...), or null
    // when it is unknown or failed to load.
    const Table* Get(std::string_view name);
    // Console command "retailTables": loads the tables if needed and prints what loaded.
    int32_t CCommand_RetailTables(const char* command, const char* arguments);
}

#endif
