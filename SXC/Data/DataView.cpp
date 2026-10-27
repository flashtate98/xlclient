#include "SXC/Data/Data.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "SXC/Data/Editor.h"
#include "SXC/ThirdParty/imgui/imgui.h"

namespace {
    using namespace SXC::Data;

    const ImVec4 kWarning(1.0f, 0.75f, 0.3f, 1.0f);

    // The selected row per table: its id, and where it was last seen (ids can repeat).
    struct Selection {
        uint32_t id = 0;
        uint32_t index = 0;
        bool valid = false;
    };

    std::string s_selected;
    std::unordered_map<std::string, Selection> s_rowSelection;
    char s_filter[128];
    std::string s_saveError;
    std::unordered_map<std::string, std::vector<RowAction>> s_rowActions;
    std::unordered_map<std::string, std::string> s_actionNotes;  // the last action's note per table

    Row SelectedRow(Table& table) {
        Selection& selection = s_rowSelection[table.Name()];

        if (!selection.valid) {
            return Row();
        }

        Row row = table.At(selection.index);

        if (!row || row.Id() != selection.id) {
            row = table.Find(selection.id);
        }

        if (row) {
            selection.index = row.Index();
        }

        return row;
    }

    void Select(Table& table, const Row& row) {
        Selection& selection = s_rowSelection[table.Name()];
        selection.valid = static_cast<bool>(row);
        selection.id = row ? row.Id() : 0;
        selection.index = row ? row.Index() : 0;
    }

    bool Contains(const std::string& text, const char* filter) {
        const size_t length = strlen(filter);

        for (size_t i = 0; i + length <= text.size(); i++) {
            if (_strnicmp(text.c_str() + i, filter, length) == 0) {
                return true;
            }
        }

        return false;
    }

    void DrawTableList() {
        for (size_t i = 0; i < TableCount(); i++) {
            Table* table = TableAt(i);
            std::string label = table->Name() + (table->HasChanges() ? " *" : "");
            const bool problem = !table->Loaded() || table->ChangedOnDisk() || table->DuplicateIds();

            if (problem) {
                ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
            }

            if (ImGui::Selectable(label.c_str(), s_selected == table->Name())) {
                s_selected = table->Name();
                s_saveError.clear();
            }

            if (problem) {
                ImGui::PopStyleColor();
            }

            ImGui::SetItemTooltip("%s", table->Status().c_str());
        }
    }

    void DrawRows(Table& table, const Row& selected) {
        const Schema& schema = table.GetSchema();
        std::vector<int> shown;

        for (size_t f = 0; f < schema.FieldCount(); f++) {
            if (!schema.FieldAt(f).hidden) {
                shown.push_back(static_cast<int>(f));
            }
        }

        // The rows that match the filter, as text in any shown field.
        std::vector<uint32_t> matches;

        for (uint32_t r = 0; r < table.RowCount(); r++) {
            if (!*s_filter) {
                matches.push_back(r);
                continue;
            }

            for (int f : shown) {
                if (Contains(Editor::FormatCell(table, table.At(r), f), s_filter)) {
                    matches.push_back(r);
                    break;
                }
            }
        }

        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                                    | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;

        if (!ImGui::BeginTable("##rows", static_cast<int>(shown.size()), flags)) {
            return;
        }

        ImGui::TableSetupScrollFreeze(1, 1);

        for (int f : shown) {
            ImGui::TableSetupColumn(schema.FieldAt(f).label.c_str(), f == 0 ? ImGuiTableColumnFlags_NoHide : ImGuiTableColumnFlags_None);
        }

        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

        for (size_t c = 0; c < shown.size(); c++) {
            const Field& field = schema.FieldAt(shown[c]);
            ImGui::TableSetColumnIndex(static_cast<int>(c));
            ImGui::TableHeader(field.label.c_str());

            if (ImGui::IsItemHovered() && !field.tooltip.empty()) {
                ImGui::SetTooltip("%s\n%s", field.name.c_str(), field.tooltip.c_str());
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(matches.size()));

        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const Row row = table.At(matches[i]);
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(matches[i]));

                for (size_t c = 0; c < shown.size(); c++) {
                    const Field& field = schema.FieldAt(shown[c]);
                    ImGui::TableSetColumnIndex(static_cast<int>(c));
                    ImGui::PushID(static_cast<int>(c));

                    if (c == 0) {
                        // The row's selectable, spanning all columns.
                        const bool duplicate = table.Find(row.Id()).Index() != row.Index();

                        if (duplicate) {
                            ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
                        }

                        const bool isSelected = selected && selected.Index() == row.Index();

                        if (ImGui::Selectable(Editor::FormatCell(table, row, shown[c]).c_str(), isSelected,
                                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick)) {
                            Select(table, row);

                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                Editor::OpenRowWindow(table.Name(), row.Id());
                            }
                        }

                        if (duplicate) {
                            ImGui::PopStyleColor();
                            ImGui::SetItemTooltip("another row has this id too");
                        }
                    } else {
                        if (field.type == FieldType::Color) {
                            Editor::DrawSwatch(field, row, shown[c]);
                        }

                        ImGui::TextUnformatted(Editor::FormatCell(table, row, shown[c]).c_str());
                    }

                    ImGui::PopID();
                }

                ImGui::PopID();
            }
        }

        ImGui::EndTable();
    }

    void DrawToolbar(Table& table, const Row& selected) {
        if (ImGui::Button("Add")) {
            Select(table, table.Add());
        }

        ImGui::SetItemTooltip("a new row with the next free id and every field's default");
        ImGui::SameLine();
        ImGui::BeginDisabled(!selected);

        if (ImGui::Button("Clone")) {
            Select(table, table.Clone(selected.Index()));
        }

        ImGui::SetItemTooltip("a copy of the selected row with the next free id");
        ImGui::SameLine();

        if (ImGui::Button("Delete")) {
            ImGui::OpenPopup("##delete");
        }

        // The table's own actions (AddRowAction).
        const auto actions = s_rowActions.find(table.Name());

        if (actions != s_rowActions.end()) {
            for (const RowAction& action : actions->second) {
                ImGui::SameLine();

                if (ImGui::Button(action.label) && selected) {
                    std::string& note = s_actionNotes[table.Name()];
                    note.clear();
                    const Row row = action.run(table, selected, note);

                    if (row) {
                        Select(table, row);
                    }
                }

                if (action.tooltip) {
                    ImGui::SetItemTooltip("%s", action.tooltip);
                }
            }
        }

        ImGui::EndDisabled();

        const auto note = s_actionNotes.find(table.Name());

        if (note != s_actionNotes.end() && !note->second.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kWarning, "%s", note->second.c_str());
        }

        if (ImGui::BeginPopup("##delete")) {
            ImGui::Text("Delete row %u? (Reload brings it back until you save.)", selected.Id());

            if (ImGui::Button("Delete") && selected) {
                const uint32_t index = selected.Index();
                table.Remove(index);
                Select(table, table.At(index < table.RowCount() ? index : table.RowCount() - 1));
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("Keep")) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::BeginDisabled(!table.CanUndo());

        if (ImGui::Button("Undo")) {
            table.Undo();
        }

        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Ctrl+Z");
        ImGui::SameLine();
        ImGui::BeginDisabled(!table.CanRedo());

        if (ImGui::Button("Redo")) {
            table.Redo();
        }

        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Ctrl+Y");

        // Outside text fields, which have their own undo.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
                table.Undo();
            } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) {
                table.Redo();
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::Button(table.HasChanges() ? "Reload (drop edits)" : "Reload")) {
            table.Reload();
            s_saveError.clear();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!table.HasChanges());

        if (ImGui::Button("Save")) {
            s_saveError.clear();
            table.Save(&s_saveError);
        }

        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "filter rows", s_filter, sizeof(s_filter));
    }

    void DrawTable(Table& table) {
        const Schema& schema = table.GetSchema();
        ImGui::TextUnformatted(table.Name().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", schema.FilePath().c_str());

        if (!schema.GetDescription().empty()) {
            ImGui::TextWrapped("%s", schema.GetDescription().c_str());
        }

        ImGui::TextDisabled("%s", table.Status().c_str());

        if (!schema.HotReload()) {
            ImGui::TextDisabled("Not reloaded when the file changes; use Reload.");
        }

        if (table.DuplicateIds()) {
            ImGui::TextColored(kWarning, "%u rows share an id with another row", table.DuplicateIds());
        }

        if (table.ChangedOnDisk()) {
            ImGui::TextColored(kWarning, "The file changed on disk, and this table has unsaved edits.");
        }

        Row selected = SelectedRow(table);
        DrawToolbar(table, selected);
        selected = SelectedRow(table);

        if (!s_saveError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", s_saveError.c_str());
        }

        ImGui::BeginChild("##grid", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0.0f), ImGuiChildFlags_ResizeX);
        DrawRows(table, selected);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##form");
        const Editor::Link link = Editor::DrawRowForm(table, selected);
        ImGui::EndChild();

        if (!link.table.empty()) {
            Editor::OpenRowWindow(link.table, link.id);
        }
    }
}

namespace SXC::Data {
    void AddRowAction(const char* table, RowAction action) {
        s_rowActions[table].push_back(action);
    }

    void DrawBrowser() {
        if (TableCount() == 0) {
            ImGui::TextDisabled("no tables registered");
            return;
        }

        ImGui::BeginChild("##tables", ImVec2(ImGui::GetFontSize() * 13.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
        DrawTableList();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##table");
        Table* table = Get(s_selected.c_str());

        if (table) {
            DrawTable(*table);
        } else {
            ImGui::TextDisabled("pick a table");
        }

        ImGui::EndChild();
    }

    void DrawWindows() {
        Editor::DrawRowWindows();
    }
}
