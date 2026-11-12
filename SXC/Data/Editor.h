#pragma once

#include <cstdint>
#include <string>
#include "SXC/Data/Data.h"

// The data layer's editor widgets, shared by the Data view and the row windows (inside SXC/Data).
namespace SXC::Data::Editor {
    // One cell as text: named values by name, flags by their bits, references with the target's name.
    std::string FormatCell(Table& table, const Row& row, int field);

    // A colour field's swatch, followed by SameLine.
    void DrawSwatch(const Field& field, const Row& row, int index);

    // A reference the user followed in a form.
    struct Link {
        std::string table;  // empty: none
        uint32_t id = 0;
        bool newWindow = false;  // Ctrl+click, or from the Data view's form
    };

    // The editing form of one row: every visible field, ungrouped ones first, then a section per group.
    Link DrawRowForm(Table& table, Row row);

    void OpenRowWindow(const std::string& table, uint32_t id);
    void DrawRowWindows();
}
