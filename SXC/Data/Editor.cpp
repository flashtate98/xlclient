#include "SXC/Data/Editor.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "SXC/ThirdParty/imgui/imgui.h"

namespace {
    using namespace SXC::Data;
    using SXC::Data::Editor::Link;

    const ImVec4 kWarning(1.0f, 0.75f, 0.3f, 1.0f);

    // --- values ---------------------------------------------------------------------------------------

    double GetNumber(const Field& field, const Row& row, int index, uint32_t element) {
        if (field.IsFloat()) {
            return row.Float(index, element);
        }

        if (field.IsSigned() || field.type == FieldType::ForeignKey) {
            return row.Int(index, element);
        }

        return row.Uint(index, element);
    }

    void SetNumber(const Field& field, Row& row, int index, uint32_t element, double value) {
        if (field.IsFloat()) {
            row.SetFloat(index, element, static_cast<float>(value));
        } else if (value < 0.0) {
            row.SetInt(index, element, static_cast<int32_t>(value));
        } else {
            row.SetUint(index, element, static_cast<uint32_t>(value));
        }
    }

    std::string FormatNumber(double value) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%g", value);
        return buffer;
    }

    // A widget format that shows the value's name: "inherit (%g)". '%' in names is escaped.
    std::string NamedFormat(const Field& field, double value, const char* number) {
        const char* name = field.NameOf(value);

        if (!name) {
            return number;
        }

        std::string format;

        for (const char* c = name; *c; c++) {
            format += *c == '%' ? "%%" : std::string(1, *c);
        }

        return format + " (" + number + ")";
    }

    std::string FlagNames(const Field& field, uint32_t bits) {
        std::string names;

        for (const NamedValue& bit : field.bits) {
            if (bits & static_cast<uint32_t>(bit.value)) {
                names += (names.empty() ? "" : ", ") + bit.name;
            }
        }

        return names;
    }

    uint32_t NamedBits(const Field& field) {
        uint32_t mask = 0;

        for (const NamedValue& bit : field.bits) {
            mask |= static_cast<uint32_t>(bit.value);
        }

        return mask;
    }

    // The target row's display text: "Snow", or empty.
    std::string TargetName(const Field& field, int32_t id) {
        Table* target = Get(field.target.c_str());

        if (!target || field.display.empty()) {
            return std::string();
        }

        const Row row = target->Find(static_cast<uint32_t>(id));
        return row ? row.String(field.display.c_str()) : std::string();
    }

    int ResizeString(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            std::string* text = static_cast<std::string*>(data->UserData);
            text->resize(data->BufTextLen);
            data->Buf = text->data();
        }

        return 0;
    }

    bool InputString(const char* label, std::string& text, bool multiline) {
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;

        if (multiline) {
            return ImGui::InputTextMultiline(label, text.data(), text.capacity() + 1, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.5f),
                                             flags | ImGuiInputTextFlags_WordWrap, ResizeString, &text);
        }

        return ImGui::InputText(label, text.data(), text.capacity() + 1, flags, ResizeString, &text);
    }

    // --- widgets per type -----------------------------------------------------------------------------

    // The small arrow beside a number that lists its named values. True when one was picked.
    bool NamedValuesArrow(const Field& field, double current, double& picked) {
        if (field.values.empty()) {
            return false;
        }

        ImGui::SameLine(0.0f, 2.0f);
        bool chosen = false;

        if (ImGui::BeginCombo("##named", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
            for (const NamedValue& named : field.values) {
                const std::string label = named.name + " (" + FormatNumber(named.value) + ")";

                if (ImGui::Selectable(label.c_str(), current == named.value)) {
                    picked = named.value;
                    chosen = true;
                }
            }

            ImGui::EndCombo();
        }

        ImGui::SetItemTooltip("named values");
        return chosen;
    }

    float ArrowWidth(const Field& field) {
        return field.values.empty() ? 0.0f : ImGui::GetFrameHeight() + 2.0f;
    }

    void EditNumber(const Field& field, Row& row, int index, uint32_t element) {
        const double current = GetNumber(field, row, index, element);
        ImGui::SetNextItemWidth(-ArrowWidth(field) - FLT_MIN);
        const bool ranged = field.min < field.max;

        if (field.IsFloat()) {
            float value = static_cast<float>(current);
            const std::string format = NamedFormat(field, current, "%g");
            const bool changed = ranged ? ImGui::SliderFloat("##v", &value, static_cast<float>(field.min), static_cast<float>(field.max), format.c_str())
                                        : ImGui::InputFloat("##v", &value, 0.0f, 0.0f, format.c_str());

            if (changed) {
                row.SetFloat(index, element, value);
            }
        } else if (field.type == FieldType::Uint || field.type == FieldType::Id) {
            uint32_t value = static_cast<uint32_t>(current);
            const std::string format = NamedFormat(field, current, "%u");

            if (ImGui::InputScalar("##v", ImGuiDataType_U32, &value, nullptr, nullptr, format.c_str())) {
                row.SetUint(index, element, value);
            }
        } else {
            int value = static_cast<int>(current);
            const std::string format = NamedFormat(field, current, "%d");
            const bool changed = ranged ? ImGui::SliderInt("##v", &value, static_cast<int>(field.min), static_cast<int>(field.max), format.c_str())
                                        : ImGui::InputScalar("##v", ImGuiDataType_S32, &value, nullptr, nullptr, format.c_str());

            if (changed) {
                row.SetInt(index, element, value);
            }
        }

        double picked;

        if (NamedValuesArrow(field, current, picked)) {
            SetNumber(field, row, index, element, picked);
        }
    }

    void EditEnum(const Field& field, Row& row, int index, uint32_t element) {
        int value = row.Int(index, element);
        const char* name = field.NameOf(value);
        const std::string preview = name ? std::string(name) : std::to_string(value) + " (unnamed)";
        ImGui::SetNextItemWidth(-FLT_MIN);

        if (ImGui::BeginCombo("##v", preview.c_str())) {
            for (const NamedValue& named : field.values) {
                const std::string label = named.name + "##" + FormatNumber(named.value);

                if (ImGui::Selectable(label.c_str(), value == named.value)) {
                    row.SetInt(index, element, static_cast<int32_t>(named.value));
                }

                ImGui::SetItemTooltip("%s", FormatNumber(named.value).c_str());
            }

            ImGui::Separator();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);

            if (ImGui::InputInt("value", &value)) {
                row.SetInt(index, element, value);
            }

            ImGui::EndCombo();
        }
    }

    // Whether a flags dropdown shows all 32 bits, per widget; opened with them when unnamed bits are set.
    std::unordered_map<ImGuiID, bool> s_showAllBits;

    void EditFlags(const Field& field, Row& row, int index, uint32_t element) {
        uint32_t bits = row.Uint(index, element);
        const std::string names = FlagNames(field, bits);
        char preview[256];
        snprintf(preview, sizeof(preview), "0x%X%s%s%s", bits, names.empty() ? "" : " (", names.c_str(), names.empty() ? "" : ")");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const ImGuiID id = ImGui::GetID("##v");

        if (ImGui::BeginCombo("##v", preview, ImGuiComboFlags_HeightLarge)) {
            const uint32_t named = NamedBits(field);

            if (ImGui::IsWindowAppearing()) {
                s_showAllBits[id] = field.bits.empty() || (bits & ~named) != 0;
            }

            bool& showAll = s_showAllBits[id];
            uint32_t edited = bits;

            for (const NamedValue& bit : field.bits) {
                char label[160];
                snprintf(label, sizeof(label), "%s (0x%X)", bit.name.c_str(), static_cast<uint32_t>(bit.value));
                ImGui::CheckboxFlags(label, &edited, static_cast<uint32_t>(bit.value));
            }

            if (showAll) {
                if (!field.bits.empty()) {
                    ImGui::SeparatorText("unnamed bits");
                }

                for (int b = 0; b < 32; b++) {
                    const uint32_t mask = 1u << b;

                    if (named & mask) {
                        continue;
                    }

                    char label[48];
                    snprintf(label, sizeof(label), "bit %d (0x%X)", b, mask);
                    ImGui::CheckboxFlags(label, &edited, mask);
                }
            }

            ImGui::Separator();

            if (!field.bits.empty()) {
                ImGui::Checkbox("show all 32 bits", &showAll);
            }

            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
            ImGui::InputScalar("hex", ImGuiDataType_U32, &edited, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal);

            if (edited != bits) {
                row.SetUint(index, element, edited);
            }

            ImGui::EndCombo();
        }
    }

    void EditColor(const Field& field, Row& row, int index) {
        const double current = GetNumber(field, row, index, 0);
        ImGui::SetNextItemWidth(-ArrowWidth(field) - FLT_MIN);

        if (field.colorFormat == ColorFormat::Float3) {
            float color[3] = { row.Float(index, 0), row.Float(index, 1), row.Float(index, 2) };

            if (ImGui::ColorEdit3("##v", color, ImGuiColorEditFlags_Float)) {
                for (uint32_t e = 0; e < 3; e++) {
                    row.SetFloat(index, e, color[e]);
                }
            }
        } else {
            const uint32_t value = row.Uint(index);
            const bool alpha = field.colorFormat == ColorFormat::Argb;
            float color[4] = { ((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f, alpha ? ((value >> 24) & 0xFF) / 255.0f : 1.0f };
            const bool changed = alpha ? ImGui::ColorEdit4("##v", color, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)
                                       : ImGui::ColorEdit3("##v", color);

            if (changed) {
                auto byte = [](float f) { return static_cast<uint32_t>(std::lround((f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f) * 255.0f)); };
                const uint32_t packed = (alpha ? byte(color[3]) << 24 : (value & 0xFF000000u)) | byte(color[0]) << 16 | byte(color[1]) << 8 | byte(color[2]);
                row.SetUint(index, 0, packed);
            }
        }

        double picked;

        if (NamedValuesArrow(field, current, picked)) {
            for (uint32_t e = 0; e < field.count; e++) {
                SetNumber(field, row, index, e, picked);
            }
        }
    }

    void EditVector(const Field& field, Row& row, int index) {
        float values[4] = {};

        for (uint32_t e = 0; e < field.count && e < 4; e++) {
            values[e] = row.Float(index, e);
        }

        ImGui::SetNextItemWidth(-FLT_MIN);

        if (ImGui::InputScalarN("##v", ImGuiDataType_Float, values, static_cast<int>(field.count < 4 ? field.count : 4), nullptr, nullptr, "%g")) {
            for (uint32_t e = 0; e < field.count && e < 4; e++) {
                row.SetFloat(index, e, values[e]);
            }
        }
    }

    // Id, a dropdown of the target's rows, and ">" to open the target row.
    Link EditReference(const Field& field, Row& row, int index, uint32_t element) {
        Link link;
        int value = row.Int(index, element);
        Table* target = Get(field.target.c_str());
        const Row targetRow = target && value != 0 ? target->Find(static_cast<uint32_t>(value)) : Row();
        const float idWidth = ImGui::GetFontSize() * 5.0f;
        const float openWidth = ImGui::GetFrameHeight();

        ImGui::SetNextItemWidth(idWidth);

        if (ImGui::InputScalar("##id", ImGuiDataType_S32, &value)) {
            row.SetInt(index, element, value);
        }

        ImGui::SameLine(0.0f, 2.0f);
        std::string preview;
        const char* name = field.NameOf(value);

        if (name) {
            preview = name;
        } else if (!target) {
            preview = field.target + " is not registered";
        } else if (!targetRow) {
            preview = "(missing)";
        } else {
            preview = TargetName(field, value);

            if (preview.empty()) {
                preview = "#" + std::to_string(value);
            }
        }

        ImGui::SetNextItemWidth(-openWidth - 2.0f);
        const bool missing = target && !name && !targetRow;

        if (missing) {
            ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        }

        const bool open = ImGui::BeginCombo("##rows", preview.c_str(), ImGuiComboFlags_HeightLarge);

        if (missing) {
            ImGui::PopStyleColor();
        }

        if (open) {
            static char s_filter[64];

            if (ImGui::IsWindowAppearing()) {
                s_filter[0] = 0;
                ImGui::SetKeyboardFocusHere();
            }

            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##filter", "filter", s_filter, sizeof(s_filter));

            for (const NamedValue& named : field.values) {
                const std::string label = named.name + " (" + FormatNumber(named.value) + ")";

                if (ImGui::Selectable(label.c_str(), value == named.value)) {
                    row.SetInt(index, element, static_cast<int32_t>(named.value));
                }
            }

            if (target) {
                if (!field.values.empty()) {
                    ImGui::Separator();
                }

                for (uint32_t r = 0; r < target->RowCount(); r++) {
                    const Row candidate = target->At(r);
                    std::string label = std::to_string(candidate.Id());

                    if (!field.display.empty()) {
                        label += "  " + candidate.String(field.display.c_str());
                    }

                    if (*s_filter && !strstr(label.c_str(), s_filter)) {
                        std::string lower = label;
                        std::string needle = s_filter;

                        for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                        for (char& c : needle) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

                        if (lower.find(needle) == std::string::npos) {
                            continue;
                        }
                    }

                    ImGui::PushID(static_cast<int>(r));

                    if (ImGui::Selectable(label.c_str(), static_cast<uint32_t>(value) == candidate.Id())) {
                        row.SetInt(index, element, static_cast<int32_t>(candidate.Id()));
                    }

                    ImGui::PopID();
                }
            }

            ImGui::EndCombo();
        }

        ImGui::SameLine(0.0f, 2.0f);
        ImGui::BeginDisabled(!targetRow);

        if (ImGui::Button(">", ImVec2(openWidth, 0.0f))) {
            link.table = field.target;
            link.id = static_cast<uint32_t>(value);
            link.newWindow = ImGui::GetIO().KeyCtrl;
        }

        ImGui::EndDisabled();
        ImGui::SetItemTooltip("open %s #%d (Ctrl+click: in a new window)", field.target.c_str(), value);
        return link;
    }

    std::string FieldTooltip(const Table& table, const Field& field, int index) {
        std::string tip = field.name;

        if (field.count > 1) {
            tip += " (" + std::to_string(field.count) + " columns)";
        }

        if (!field.tooltip.empty()) {
            tip += "\n" + field.tooltip;
        }

        if (field.type == FieldType::ForeignKey) {
            tip += "\nreferences " + field.target;
        }

        if (field.min < field.max) {
            tip += "\nrange " + FormatNumber(field.min) + " .. " + FormatNumber(field.max);
        }

        if (!field.values.empty() && field.type != FieldType::Enum) {
            tip += "\nnamed values:";

            for (const NamedValue& named : field.values) {
                tip += "\n  " + FormatNumber(named.value) + " = " + named.name;
            }
        }

        if (!field.IsString() && field.type != FieldType::Id) {
            tip += "\ndefault " + (field.NameOf(field.fallback) ? std::string(field.NameOf(field.fallback)) : FormatNumber(field.fallback));
        }

        return tip;
    }

    // One field: the label cell and the widget cell of the form's table.
    Link DrawField(Table& table, Row& row, int index) {
        const Field& field = table.GetSchema().FieldAt(index);
        Link link;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(field.label.c_str());
        ImGui::SetItemTooltip("%s", FieldTooltip(table, field, index).c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(index);
        ImGui::BeginDisabled(field.readOnly);

        const bool perElement = !field.vector && field.type != FieldType::Color;

        for (uint32_t e = 0; e < (perElement ? field.count : 1); e++) {
            ImGui::PushID(static_cast<int>(e));

            if (perElement && field.count > 1) {
                ImGui::TextDisabled("%u", e + 1);
                ImGui::SameLine();
            }

            switch (field.type) {
                case FieldType::Bool: {
                    bool value = row.Bool(index, e);

                    if (ImGui::Checkbox("##v", &value)) {
                        row.SetUint(index, e, value ? 1 : 0);
                    }
                    break;
                }

                case FieldType::Enum:
                    EditEnum(field, row, index, e);
                    break;

                case FieldType::Flags:
                    EditFlags(field, row, index, e);
                    break;

                case FieldType::String:
                case FieldType::Path:
                case FieldType::Text: {
                    std::string text = row.String(index, e);

                    if (field.type != FieldType::Text) {
                        ImGui::SetNextItemWidth(-FLT_MIN);
                    }

                    if (InputString("##v", text, field.type == FieldType::Text)) {
                        row.SetString(index, e, text.c_str());
                    }
                    break;
                }

                case FieldType::Color:
                    EditColor(field, row, index);
                    break;

                case FieldType::ForeignKey: {
                    const Link followed = EditReference(field, row, index, e);

                    if (!followed.table.empty()) {
                        link = followed;
                    }
                    break;
                }

                default:
                    if (field.vector) {
                        EditVector(field, row, index);
                    } else {
                        EditNumber(field, row, index, e);
                    }
                    break;
            }

            ImGui::PopID();
        }

        ImGui::EndDisabled();
        ImGui::PopID();
        return link;
    }

    // --- row windows ----------------------------------------------------------------------------------

    struct Place {
        std::string table;
        uint32_t id;
    };

    struct RowWindow {
        int uid;
        Place place;
        std::vector<Place> back;
        std::vector<Place> forward;
        bool open = true;
    };

    std::vector<RowWindow> s_windows;
    int s_nextUid = 1;
}

namespace SXC::Data::Editor {
    std::string FormatCell(Table& table, const Row& row, int index) {
        const Field& field = table.GetSchema().FieldAt(index);
        char buffer[128];
        std::string text;

        for (uint32_t e = 0; e < field.count; e++) {
            if (e > 0) {
                text += ", ";
            }

            if (field.IsString()) {
                const std::string& value = row.String(index, e);
                const size_t newline = value.find('\n');
                text += newline == std::string::npos ? value : value.substr(0, newline) + " ...";
                continue;
            }

            const double value = GetNumber(field, row, index, e);

            if (field.IsFloat()) {
                snprintf(buffer, sizeof(buffer), "%g", value);
            } else if (field.type == FieldType::Flags || field.type == FieldType::Color) {
                snprintf(buffer, sizeof(buffer), field.colorFormat == ColorFormat::Argb ? "0x%08X" : "0x%06X", row.Uint(index, e));
            } else if (field.type == FieldType::Bool) {
                snprintf(buffer, sizeof(buffer), "%s", row.Bool(index, e) ? "yes" : "no");
            } else {
                snprintf(buffer, sizeof(buffer), "%.0f", value);
            }

            const char* name = field.NameOf(value);

            if (name && field.type == FieldType::Enum) {
                text += name;
                continue;
            }

            text += buffer;

            if (name) {
                text += std::string(" (") + name + ")";
            } else if (field.type == FieldType::Flags) {
                const std::string names = FlagNames(field, row.Uint(index, e));

                if (!names.empty()) {
                    text += " (" + names + ")";
                }
            } else if (field.type == FieldType::ForeignKey && value != 0.0 && Get(field.target.c_str())) {
                if (!Get(field.target.c_str())->Find(row.Uint(index, e))) {
                    text += " (missing)";
                } else {
                    const std::string target = TargetName(field, static_cast<int32_t>(value));

                    if (!target.empty()) {
                        text += " " + target;
                    }
                }
            }
        }

        return text;
    }

    void DrawSwatch(const Field& field, const Row& row, int index) {
        ImVec4 color;

        if (field.colorFormat == ColorFormat::Float3) {
            color = ImVec4(row.Float(index, 0), row.Float(index, 1), row.Float(index, 2), 1.0f);
        } else {
            const uint32_t value = row.Uint(index);
            color = ImVec4(((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f,
                           field.colorFormat == ColorFormat::Argb ? ((value >> 24) & 0xFF) / 255.0f : 1.0f);
        }

        const float size = ImGui::GetTextLineHeight();
        ImGui::ColorButton("##swatch", color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(size, size));
        ImGui::SameLine();
    }

    Link DrawRowForm(Table& table, Row row) {
        Link link;

        if (!row) {
            ImGui::TextDisabled("no row selected");
            return link;
        }

        const Schema& schema = table.GetSchema();
        const Row first = table.FindBy(0, row.Id());

        if (ImGui::Button("Revert row")) {
            table.Revert(row.Index());
            return link;
        }

        ImGui::SetItemTooltip("back to the row as in the file (loaded or last saved); a row added since is removed");

        if (first && table.FindBy(0, row.Id(), first)) {
            ImGui::SameLine();
            ImGui::TextColored(kWarning, "Another row has id %u too.", row.Id());
        }

        // Ungrouped fields first, in order, then the groups in the order they first appear.
        std::vector<std::string> groups(1);

        for (size_t f = 0; f < schema.FieldCount(); f++) {
            const std::string& group = schema.FieldAt(f).group;
            bool known = false;

            for (const std::string& seen : groups) {
                known = known || seen == group;
            }

            if (!known) {
                groups.push_back(group);
            }
        }

        for (const std::string& group : groups) {
            bool any = false;

            for (size_t f = 0; f < schema.FieldCount(); f++) {
                any = any || (schema.FieldAt(f).group == group && !schema.FieldAt(f).hidden);
            }

            if (!any || (!group.empty() && !ImGui::CollapsingHeader(group.c_str(), ImGuiTreeNodeFlags_DefaultOpen))) {
                continue;
            }

            if (!ImGui::BeginTable(("##form" + group).c_str(), 2, ImGuiTableFlags_SizingStretchProp)) {
                continue;
            }

            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

            for (size_t f = 0; f < schema.FieldCount(); f++) {
                const Field& field = schema.FieldAt(f);

                if (field.group != group || field.hidden) {
                    continue;
                }

                const Link followed = DrawField(table, row, static_cast<int>(f));

                if (!followed.table.empty()) {
                    link = followed;
                }
            }

            ImGui::EndTable();
        }

        return link;
    }

    void OpenRowWindow(const std::string& table, uint32_t id) {
        RowWindow window;
        window.uid = s_nextUid++;
        window.place = Place{ table, id };
        s_windows.push_back(window);
    }

    void DrawRowWindows() {
        for (size_t i = 0; i < s_windows.size(); i++) {
            RowWindow& window = s_windows[i];
            Table* table = Get(window.place.table.c_str());
            const Row row = table ? table->Find(window.place.id) : Row();
            std::string title = window.place.table + " #" + std::to_string(window.place.id);

            if (row && table->GetSchema().IndexOf("Name") > 0) {
                const std::string& name = row.String("Name");

                if (!name.empty()) {
                    title += " " + name;
                }
            }

            if (table && table->HasChanges()) {
                title += " *";
            }

            title += "###SxcRow" + std::to_string(window.uid);
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, ImGui::GetFontSize() * 34.0f), ImGuiCond_FirstUseEver);
            Link link;

            if (ImGui::Begin(title.c_str(), &window.open)) {
                ImGui::BeginDisabled(window.back.empty());

                if (ImGui::ArrowButton("##back", ImGuiDir_Left)) {
                    window.forward.push_back(window.place);
                    window.place = window.back.back();
                    window.back.pop_back();
                }

                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(window.forward.empty());

                if (ImGui::ArrowButton("##forward", ImGuiDir_Right)) {
                    window.back.push_back(window.place);
                    window.place = window.forward.back();
                    window.forward.pop_back();
                }

                ImGui::EndDisabled();

                if (table) {
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!table->HasChanges());

                    if (ImGui::Button("Save")) {
                        std::string error;
                        table->Save(&error);
                    }

                    ImGui::EndDisabled();
                }

                ImGui::Separator();

                if (!table) {
                    ImGui::TextDisabled("table %s is not registered", window.place.table.c_str());
                } else if (!row) {
                    ImGui::TextDisabled("no row with id %u (removed, or its id changed)", window.place.id);
                } else {
                    link = DrawRowForm(*table, row);
                }
            }

            ImGui::End();

            if (link.table.empty()) {
                continue;
            }

            if (link.newWindow) {
                OpenRowWindow(link.table, link.id);  // may reallocate s_windows: `window` is not used after this
            } else {
                window.back.push_back(window.place);
                window.forward.clear();
                window.place = Place{ link.table, link.id };
            }
        }

        for (size_t i = s_windows.size(); i-- > 0;) {
            if (!s_windows[i].open) {
                s_windows.erase(s_windows.begin() + i);
            }
        }
    }
}
