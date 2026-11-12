#include "SXC/Settings/Settings.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include "SXC/Log.h"
#include "SXC/ThirdParty/imgui/imgui.h"

namespace {
    constexpr const char* kPath = "WTF\\SXC.ini";
    constexpr const char* kTempPath = "WTF\\SXC.ini.tmp";
    // Quiet time after the last change before the file is written (a dragged slider changes every frame).
    constexpr DWORD kSaveDelayMs = 1000;

    enum class Type { Bool, Int, Float, String };

    struct Entry {
        Type type;
        std::string section;
        std::string key;
        std::string label;  // empty: not in the Settings view
        std::string description;
        bool b = false;
        bool bFallback = false;
        int i = 0;
        int iFallback = 0;
        int iMin = 0;
        int iMax = 0;
        float f = 0.0f;
        float fFallback = 0.0f;
        float fMin = 0.0f;
        float fMax = 0.0f;
        std::string s;
        std::string sFallback;
    };

    // One line of the ini as read, so saving keeps comments and order. `key` is empty for anything that
    // isn't a key line (comments, blank lines, section headers).
    struct Line {
        std::string text;
        std::string section;
        std::string key;
        std::string value;
    };

    std::string Trim(const std::string& text) {
        const size_t first = text.find_first_not_of(" \t");

        if (first == std::string::npos) {
            return std::string();
        }

        const size_t last = text.find_last_not_of(" \t");
        return text.substr(first, last - first + 1);
    }

    bool Same(const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) == 0;
    }

    std::string Format(const Entry& entry) {
        char buffer[64];

        switch (entry.type) {
            case Type::Bool:
                return entry.b ? "1" : "0";

            case Type::Int:
                snprintf(buffer, sizeof(buffer), "%d", entry.i);
                return buffer;

            case Type::Float:
                snprintf(buffer, sizeof(buffer), "%g", entry.f);
                return buffer;

            default:
                return Trim(entry.s);
        }
    }

    struct Store {
        std::vector<std::unique_ptr<Entry>> entries;
        std::vector<Line> lines;
        bool loaded = false;
        bool dirty = false;
        DWORD changedAt = 0;

        ~Store() {
            Save();
        }

        void Load() {
            if (loaded) {
                return;
            }

            loaded = true;
            FILE* file = nullptr;

            if (fopen_s(&file, kPath, "rb") != 0 || !file) {
                return;
            }

            std::string section;
            char buffer[4096];

            while (fgets(buffer, sizeof(buffer), file)) {
                Line line;
                line.text = buffer;

                while (!line.text.empty() && (line.text.back() == '\n' || line.text.back() == '\r')) {
                    line.text.pop_back();
                }

                const std::string trimmed = Trim(line.text);

                if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
                    section = Trim(trimmed.substr(1, trimmed.size() - 2));
                } else if (!trimmed.empty() && trimmed.front() != ';' && trimmed.front() != '#') {
                    const size_t equals = trimmed.find('=');

                    if (equals != std::string::npos) {
                        line.key = Trim(trimmed.substr(0, equals));
                        line.value = Trim(trimmed.substr(equals + 1));
                    }
                }

                line.section = section;
                lines.push_back(line);
            }

            fclose(file);
        }

        Line* Find(const std::string& section, const std::string& key) {
            for (Line& line : lines) {
                if (!line.key.empty() && Same(line.section, section) && Same(line.key, key)) {
                    return &line;
                }
            }

            return nullptr;
        }

        void MarkDirty() {
            dirty = true;
            changedAt = GetTickCount();
        }

        // Where a new key of `section` goes: after the section's last non-blank line, or at the end
        // under a new header.
        size_t InsertionPoint(const std::string& section) {
            size_t at = SIZE_MAX;

            for (size_t i = 0; i < lines.size(); i++) {
                if (Same(lines[i].section, section) && !Trim(lines[i].text).empty()) {
                    at = i + 1;
                }
            }

            if (at != SIZE_MAX) {
                return at;
            }

            if (!lines.empty() && !Trim(lines.back().text).empty()) {
                lines.push_back(Line{ "", lines.back().section, "", "" });
            }

            lines.push_back(Line{ "[" + section + "]", section, "", "" });
            return lines.size();
        }

        void Save() {
            if (!dirty) {
                return;
            }

            dirty = false;

            for (const std::unique_ptr<Entry>& entry : entries) {
                const std::string value = Format(*entry);

                if (Line* line = Find(entry->section, entry->key)) {
                    line->value = value;
                    line->text = line->key + "=" + value;
                    continue;
                }

                size_t at = InsertionPoint(entry->section);

                if (!entry->description.empty()) {
                    lines.insert(lines.begin() + at++, Line{ "; " + entry->description, entry->section, "", "" });
                }

                lines.insert(lines.begin() + at, Line{ entry->key + "=" + value, entry->section, entry->key, value });
            }

            CreateDirectoryA("WTF", nullptr);
            FILE* file = nullptr;

            if (fopen_s(&file, kTempPath, "wb") != 0 || !file) {
                SXC::LogOnce("settings|write", "settings: cannot write %s", kTempPath);
                return;
            }

            for (const Line& line : lines) {
                fputs(line.text.c_str(), file);
                fputs("\r\n", file);
            }

            const bool written = fclose(file) == 0;

            if (!written || !MoveFileExA(kTempPath, kPath, MOVEFILE_REPLACE_EXISTING)) {
                SXC::LogOnce("settings|replace", "settings: cannot replace %s", kPath);
            }
        }
    };

    Store& GetStore() {
        static Store s_store;
        return s_store;
    }

    void LogBadValue(const Entry& entry, const std::string& value) {
        const std::string key = "settings|bad|" + entry.section + "|" + entry.key;
        SXC::LogOnce(key.c_str(), "settings: [%s] %s=%s is not a valid value; using %s", entry.section.c_str(), entry.key.c_str(), value.c_str(), Format(entry).c_str());
    }

    void ReadValue(Entry& entry, const std::string& value) {
        switch (entry.type) {
            case Type::Bool:
                if (value == "1" || Same(value, "true") || Same(value, "yes") || Same(value, "on")) {
                    entry.b = true;
                } else if (value == "0" || Same(value, "false") || Same(value, "no") || Same(value, "off")) {
                    entry.b = false;
                } else {
                    LogBadValue(entry, value);
                }
                break;

            case Type::Int: {
                char* end = nullptr;
                const long parsed = strtol(value.c_str(), &end, 0);

                if (end != value.c_str() && *end == '\0') {
                    entry.i = static_cast<int>(parsed);
                } else {
                    LogBadValue(entry, value);
                }
                break;
            }

            case Type::Float: {
                char* end = nullptr;
                const float parsed = strtof(value.c_str(), &end);

                if (end != value.c_str() && *end == '\0') {
                    entry.f = parsed;
                } else {
                    LogBadValue(entry, value);
                }
                break;
            }

            default:
                entry.s = value;
                break;
        }
    }

    // The entry for (section, key); a new one is filled from the ini, or marks the file dirty so the
    // key gets written. The first declaration's type, label and range win.
    Entry* Declare(Type type, const char* section, const char* key, const char* label, const char* description, std::unique_ptr<Entry> fresh) {
        Store& store = GetStore();
        store.Load();
        const std::string sectionName = section ? section : "General";
        const std::string keyName = key ? key : "";

        for (const std::unique_ptr<Entry>& entry : store.entries) {
            if (Same(entry->section, sectionName) && Same(entry->key, keyName)) {
                if (entry->type != type) {
                    SXC::LogOnce(("settings|type|" + sectionName + "|" + keyName).c_str(), "settings: [%s] %s is declared with two types", section, key);
                }

                return entry.get();
            }
        }

        Entry* entry = fresh.get();
        entry->type = type;
        entry->section = sectionName;
        entry->key = keyName;
        entry->label = label ? label : "";
        entry->description = description ? description : "";

        if (const Line* line = store.Find(sectionName, keyName)) {
            ReadValue(*entry, line->value);
        } else {
            store.MarkDirty();
        }

        store.entries.push_back(std::move(fresh));
        return entry;
    }

    Entry* As(void* entry) {
        return static_cast<Entry*>(entry);
    }

    // --- the Settings view ---------------------------------------------------------------------------

    int ResizeString(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            std::string* text = static_cast<std::string*>(data->UserData);
            text->resize(data->BufTextLen);
            data->Buf = text->data();
        }

        return 0;
    }

    void DrawEntry(Entry& entry) {
        Store& store = GetStore();
        bool changed = false;
        const char* label = entry.label.c_str();

        switch (entry.type) {
            case Type::Bool:
                changed = ImGui::Checkbox(label, &entry.b);
                break;

            case Type::Int:
                changed = entry.iMin < entry.iMax ? ImGui::SliderInt(label, &entry.i, entry.iMin, entry.iMax) : ImGui::InputInt(label, &entry.i);
                break;

            case Type::Float:
                changed = entry.fMin < entry.fMax ? ImGui::SliderFloat(label, &entry.f, entry.fMin, entry.fMax, "%.2f") : ImGui::InputFloat(label, &entry.f);
                break;

            default:
                changed = ImGui::InputText(label, entry.s.data(), entry.s.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeString, &entry.s);
                break;
        }

        if (!entry.description.empty()) {
            ImGui::SetItemTooltip("%s", entry.description.c_str());
        }

        if (ImGui::BeginPopupContextItem("##reset")) {
            Entry fallback = entry;
            fallback.b = entry.bFallback;
            fallback.i = entry.iFallback;
            fallback.f = entry.fFallback;
            fallback.s = entry.sFallback;
            const std::string item = "Reset to default (" + Format(fallback) + ")";

            if (ImGui::MenuItem(item.c_str())) {
                entry.b = fallback.b;
                entry.i = fallback.i;
                entry.f = fallback.f;
                entry.s = fallback.s;
                changed = true;
            }

            ImGui::TextDisabled("[%s] %s in %s", entry.section.c_str(), entry.key.c_str(), kPath);
            ImGui::EndPopup();
        }

        if (changed) {
            store.MarkDirty();
        }
    }
}

namespace SXC::Settings {
    Bool::Bool(const char* section, const char* key, bool fallback, const char* label, const char* description) {
        std::unique_ptr<Entry> fresh(new Entry());
        fresh->b = fresh->bFallback = fallback;
        m_entry = Declare(Type::Bool, section, key, label, description, std::move(fresh));
    }

    bool Bool::Get() const {
        return As(m_entry)->b;
    }

    void Bool::Set(bool value) {
        if (As(m_entry)->b != value) {
            As(m_entry)->b = value;
            GetStore().MarkDirty();
        }
    }

    Int::Int(const char* section, const char* key, int fallback, int min, int max, const char* label, const char* description) {
        std::unique_ptr<Entry> fresh(new Entry());
        fresh->i = fresh->iFallback = fallback;
        fresh->iMin = min;
        fresh->iMax = max;
        m_entry = Declare(Type::Int, section, key, label, description, std::move(fresh));
    }

    int Int::Get() const {
        return As(m_entry)->i;
    }

    void Int::Set(int value) {
        if (As(m_entry)->i != value) {
            As(m_entry)->i = value;
            GetStore().MarkDirty();
        }
    }

    Float::Float(const char* section, const char* key, float fallback, float min, float max, const char* label, const char* description) {
        std::unique_ptr<Entry> fresh(new Entry());
        fresh->f = fresh->fFallback = fallback;
        fresh->fMin = min;
        fresh->fMax = max;
        m_entry = Declare(Type::Float, section, key, label, description, std::move(fresh));
    }

    float Float::Get() const {
        return As(m_entry)->f;
    }

    void Float::Set(float value) {
        if (As(m_entry)->f != value) {
            As(m_entry)->f = value;
            GetStore().MarkDirty();
        }
    }

    String::String(const char* section, const char* key, const char* fallback, const char* label, const char* description) {
        std::unique_ptr<Entry> fresh(new Entry());
        fresh->s = fresh->sFallback = Trim(fallback ? fallback : "");
        m_entry = Declare(Type::String, section, key, label, description, std::move(fresh));
    }

    const std::string& String::Get() const {
        return As(m_entry)->s;
    }

    void String::Set(const char* value) {
        const std::string trimmed = Trim(value ? value : "");

        if (As(m_entry)->s != trimmed) {
            As(m_entry)->s = trimmed;
            GetStore().MarkDirty();
        }
    }

    void Tick() {
        Store& store = GetStore();

        if (store.dirty && GetTickCount() - store.changedAt >= kSaveDelayMs) {
            store.Save();
        }
    }

    void Save() {
        GetStore().Save();
    }

    void DrawEditor() {
        Store& store = GetStore();
        std::vector<std::string> sections;

        for (const std::unique_ptr<Entry>& entry : store.entries) {
            if (entry->label.empty()) {
                continue;
            }

            bool known = false;

            for (const std::string& section : sections) {
                known = known || Same(section, entry->section);
            }

            if (!known) {
                sections.push_back(entry->section);
            }
        }

        if (sections.empty()) {
            ImGui::TextDisabled("nothing to set yet");
        }

        for (const std::string& section : sections) {
            if (!ImGui::CollapsingHeader(section.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                continue;
            }

            for (const std::unique_ptr<Entry>& entry : store.entries) {
                if (entry->label.empty() || !Same(entry->section, section)) {
                    continue;
                }

                ImGui::PushID(entry.get());
                DrawEntry(*entry);
                ImGui::PopID();
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Saved to %s. Right-click a setting to reset it.", kPath);
    }
}
