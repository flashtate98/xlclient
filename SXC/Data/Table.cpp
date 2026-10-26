#include "SXC/Data/Data.h"

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <utility>
#include <vector>
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "SXC/Log.h"
#include "SXC/Settings/Settings.h"

namespace {
    constexpr uint32_t kHeaderSize = 20;  // magic, record count, field count, record size, string block size
    constexpr DWORD kWatchIntervalMs = 1000;

    struct RowData {
        std::vector<uint32_t> cells;
        std::vector<std::string> texts;  // per column; used by string columns only
        int32_t origin = -1;             // its serial in Impl::original; -1: added since the last load / save
    };

    constexpr size_t kUndoSteps = 100;
    // Edits closer together than this are one undo step (a dragged slider changes every frame).
    constexpr DWORD kUndoGroupMs = 600;

    // What the watcher compares: the loose file's presence, time and size.
    struct Stamp {
        bool exists = false;
        FILETIME time = {};
        DWORD size = 0;

        bool operator==(const Stamp& other) const {
            return exists == other.exists && (!exists || (CompareFileTime(&time, &other.time) == 0 && size == other.size));
        }
    };

    Stamp StampOf(const std::string& path) {
        Stamp stamp;
        WIN32_FILE_ATTRIBUTE_DATA data;

        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data) && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            stamp.exists = true;
            stamp.time = data.ftLastWriteTime;
            stamp.size = data.nFileSizeLow;
        }

        return stamp;
    }

    uint32_t ReadU32(const uint8_t* p) {
        uint32_t value;
        memcpy(&value, p, sizeof(value));
        return value;
    }

    void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), p, p + 4);
    }

    uint32_t FloatBits(float value) {
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    float BitsFloat(uint32_t bits) {
        float value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    uint32_t Encode(const SXC::Data::Field& field, double value) {
        if (field.IsFloat()) {
            return FloatBits(static_cast<float>(value));
        }

        if (value < 0.0) {
            return static_cast<uint32_t>(static_cast<int32_t>(value));
        }

        return static_cast<uint32_t>(value);
    }

    bool ReadLoose(const std::string& path, std::vector<uint8_t>& out) {
        FILE* file = nullptr;

        if (fopen_s(&file, path.c_str(), "rb") != 0 || !file) {
            return false;
        }

        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        out.resize(size > 0 ? static_cast<size_t>(size) : 0);
        const bool ok = size > 0 && fread(out.data(), 1, out.size(), file) == out.size();
        fclose(file);
        return ok;
    }

    bool ReadArchives(const std::string& path, std::vector<uint8_t>& out) {
        void* data = nullptr;
        uint32_t size = 0;

        if (!SFile::LoadFile(path.c_str(), &data, &size, 0, nullptr) || !data) {
            return false;
        }

        out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        SFile::Unload(data);
        return true;
    }

    std::string Format(const char* format, ...) {
        char buffer[512];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        return buffer;
    }

    std::vector<std::unique_ptr<SXC::Data::Table>>& Tables() {
        static std::vector<std::unique_ptr<SXC::Data::Table>> s_tables;
        return s_tables;
    }

    const std::string kEmpty;
}

namespace SXC::Data {
    struct Table::Impl {
        Table* self;
        Schema schema;
        std::vector<int> columnField;    // the field each column belongs to
        std::vector<RowData> rows;
        uint32_t fileColumns = 0;        // as the file had them; more than the schema: saving is off
        bool loaded = false;
        bool loose = false;
        std::string status;
        uint32_t generation = 1;
        bool changes = false;
        bool changedOnDisk = false;
        bool backedUp = false;
        Stamp stamp;
        DWORD lastWatch = 0;
        std::unordered_map<uint32_t, uint32_t> idIndex;
        bool indexDirty = true;
        uint32_t duplicates = 0;
        std::vector<std::pair<Listener, void*>> listeners;
        // Each row's state in the file as loaded / last saved, by serial (Revert). Serials stay valid
        // across saves, so a row brought back by undo still finds its own.
        std::unordered_map<int32_t, RowData> original;
        int32_t nextSerial = 0;
        std::vector<RowData> committed;              // the rows before the current edit group
        std::vector<std::vector<RowData>> undo;
        std::vector<std::vector<RowData>> redo;
        DWORD lastEdit = 0;

        Impl(Table* table, Schema&& from) : self(table), schema(std::move(from)) {
            for (size_t i = 0; i < schema.FieldCount(); i++) {
                for (uint32_t e = 0; e < schema.FieldAt(i).count; e++) {
                    columnField.push_back(static_cast<int>(i));
                }
            }
        }

        uint32_t Columns() const {
            return (std::max)(schema.Columns(), fileColumns);
        }

        bool IsStringColumn(uint32_t column) const {
            return column < columnField.size() && schema.FieldAt(columnField[column]).IsString();
        }

        RowData NewRow() const {
            RowData row;
            row.cells.assign(Columns(), 0);
            row.texts.resize(Columns());

            for (uint32_t c = 0; c < schema.Columns(); c++) {
                const Field& field = schema.FieldAt(columnField[c]);

                if (!field.IsString()) {
                    row.cells[c] = Encode(field, field.fallback);
                }
            }

            return row;
        }

        void Notify(Change change) {
            const std::vector<std::pair<Listener, void*>> listenersNow = listeners;

            for (const auto& listener : listenersNow) {
                listener.first(*self, change, listener.second);
            }
        }

        void BuildIndex() {
            if (!indexDirty) {
                return;
            }

            indexDirty = false;
            idIndex.clear();
            duplicates = 0;

            for (uint32_t i = 0; i < rows.size(); i++) {
                if (!idIndex.emplace(rows[i].cells[0], i).second) {
                    duplicates++;
                }
            }
        }

        uint32_t NextId() const {
            uint32_t next = 1;

            for (const RowData& row : rows) {
                next = (std::max)(next, row.cells[0] + 1);
            }

            return next;
        }

        void Load() {
            rows.clear();
            fileColumns = 0;
            loaded = false;
            indexDirty = true;
            const std::string& path = schema.FilePath();
            std::vector<uint8_t> bytes;
            const char* source = "loose file";
            stamp = StampOf(path);
            loose = stamp.exists && ReadLoose(path, bytes);

            if (!loose) {
                source = "archives";

                if (!ReadArchives(path, bytes)) {
                    // A table renamed: its rows from the file under the old name until the first save.
                    const std::string& former = schema.GetFormerPath();

                    if (former.empty() || !((StampOf(former).exists && ReadLoose(former, bytes)) || ReadArchives(former, bytes))) {
                        status = "no file yet; saving creates " + path;
                        return;
                    }

                    source = "file under its former name (saving writes the new one)";
                    SXC::Log("data: %s: no %s yet; read %s", schema.Name().c_str(), path.c_str(), former.c_str());
                }
            }

            if (bytes.size() < kHeaderSize || memcmp(bytes.data(), "WDBC", 4) != 0) {
                status = Format("%s (%s) is not a WDBC file", path.c_str(), source);
                SXC::Log("data: %s: %s", schema.Name().c_str(), status.c_str());
                return;
            }

            const uint32_t count = ReadU32(bytes.data() + 4);
            const uint32_t columns = ReadU32(bytes.data() + 8);
            const uint32_t recordSize = ReadU32(bytes.data() + 12);
            const uint32_t stringSize = ReadU32(bytes.data() + 16);
            const uint64_t need = kHeaderSize + static_cast<uint64_t>(count) * recordSize + stringSize;

            if (columns == 0 || recordSize != columns * 4 || bytes.size() < need) {
                status = Format("%s (%s): %u columns, %u-byte records, %zu of %llu bytes: not a table of 4-byte columns, or cut short",
                                path.c_str(), source, columns, recordSize, bytes.size(), static_cast<unsigned long long>(need));
                SXC::Log("data: %s: %s", schema.Name().c_str(), status.c_str());
                return;
            }

            fileColumns = columns;
            const uint8_t* records = bytes.data() + kHeaderSize;
            const char* strings = reinterpret_cast<const char*>(records + static_cast<uint64_t>(count) * recordSize);
            uint32_t badOffsets = 0;
            rows.reserve(count);

            for (uint32_t r = 0; r < count; r++) {
                RowData row = NewRow();
                const uint8_t* record = records + static_cast<uint64_t>(r) * recordSize;

                for (uint32_t c = 0; c < columns; c++) {
                    row.cells[c] = ReadU32(record + c * 4);

                    if (IsStringColumn(c)) {
                        const uint32_t offset = row.cells[c];

                        if (offset < stringSize) {
                            row.texts[c].assign(strings + offset, strnlen(strings + offset, stringSize - offset));
                        } else {
                            badOffsets++;
                        }

                        row.cells[c] = 0;
                    }
                }

                rows.push_back(std::move(row));
            }

            loaded = true;
            status = Format("%u rows from the %s", count, source);

            if (columns < schema.Columns()) {
                status += Format("; the file has %u of %u columns (older), saving adds the rest", columns, schema.Columns());
            } else if (columns > schema.Columns()) {
                status += Format("; the file has %u columns, the schema %u (newer): saving is off", columns, schema.Columns());
            }

            if (badOffsets) {
                status += Format("; %u strings pointed outside the string block (read as empty)", badOffsets);
            }

            if (!loose) {
                status += "; saving writes a loose file";
            }
        }

        bool Save(std::string* error) {
            auto fail = [&](const std::string& reason) {
                if (error) {
                    *error = reason;
                }

                SXC::Log("data: %s: %s", schema.Name().c_str(), reason.c_str());
                return false;
            };

            if (fileColumns > schema.Columns()) {
                return fail(Format("not saved: the file has %u columns, the schema only %u; saving would drop the rest", fileColumns, schema.Columns()));
            }

            // Columns the file didn't have yet are written from now on.
            fileColumns = schema.Columns();

            for (RowData& row : rows) {
                row.cells.resize(fileColumns);
                row.texts.resize(fileColumns);
            }

            std::stable_sort(rows.begin(), rows.end(), [](const RowData& a, const RowData& b) { return a.cells[0] < b.cells[0]; });
            indexDirty = true;
            generation++;

            // The string block: offset 0 is the empty string; equal strings are stored once.
            std::vector<uint8_t> stringBlock(1, 0);
            std::unordered_map<std::string, uint32_t> offsets;
            std::vector<uint8_t> out;
            out.insert(out.end(), { 'W', 'D', 'B', 'C' });
            AppendU32(out, static_cast<uint32_t>(rows.size()));
            AppendU32(out, fileColumns);
            AppendU32(out, fileColumns * 4);
            const size_t stringSizeAt = out.size();
            AppendU32(out, 0);

            for (const RowData& row : rows) {
                for (uint32_t c = 0; c < fileColumns; c++) {
                    if (!IsStringColumn(c)) {
                        AppendU32(out, row.cells[c]);
                        continue;
                    }

                    const std::string& text = row.texts[c];
                    uint32_t offset = 0;

                    if (!text.empty()) {
                        auto found = offsets.find(text);

                        if (found != offsets.end()) {
                            offset = found->second;
                        } else {
                            offset = static_cast<uint32_t>(stringBlock.size());
                            stringBlock.insert(stringBlock.end(), text.begin(), text.end());
                            stringBlock.push_back(0);
                            offsets.emplace(text, offset);
                        }
                    }

                    AppendU32(out, offset);
                }
            }

            const uint32_t stringSize = static_cast<uint32_t>(stringBlock.size());
            memcpy(out.data() + stringSizeAt, &stringSize, 4);
            out.insert(out.end(), stringBlock.begin(), stringBlock.end());

            const std::string& path = schema.FilePath();
            const size_t slash = path.find_last_of("\\/");

            if (slash != std::string::npos) {
                CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
            }

            // The file as it was before this session's first save.
            if (!backedUp && StampOf(path).exists) {
                char suffix[32];
                const time_t now = time(nullptr);
                tm local;
                localtime_s(&local, &now);
                strftime(suffix, sizeof(suffix), ".%Y%m%d-%H%M%S.bak", &local);

                if (!CopyFileA(path.c_str(), (path + suffix).c_str(), FALSE)) {
                    return fail("not saved: could not keep a backup as " + path + suffix);
                }

                backedUp = true;
            }

            const std::string temp = path + ".tmp";
            FILE* file = nullptr;

            if (fopen_s(&file, temp.c_str(), "wb") != 0 || !file) {
                return fail("not saved: cannot write " + temp);
            }

            const bool written = fwrite(out.data(), 1, out.size(), file) == out.size();
            const bool closed = fclose(file) == 0;

            if (!written || !closed || !MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileA(temp.c_str());
                return fail("not saved: cannot replace " + path);
            }

            loaded = true;
            loose = true;
            changes = false;
            changedOnDisk = false;
            stamp = StampOf(path);
            Baseline(false);
            // A row saved and edited again since counts from the saved state.
            lastEdit = 0;
            status = Format("%zu rows, saved to the loose file", rows.size());
            return true;
        }

        void Watch() {
            const DWORD now = GetTickCount();

            if (now - lastWatch < kWatchIntervalMs) {
                return;
            }

            lastWatch = now;
            const Stamp current = StampOf(schema.FilePath());

            if (current == stamp) {
                return;
            }

            if (changes) {
                if (!changedOnDisk) {
                    changedOnDisk = true;
                    SXC::Log("data: %s changed on disk while it has unsaved edits; not reloaded", schema.Name().c_str());
                }

                return;
            }

            self->Reload();
        }

        // A new baseline: the file as it is now (after loading or saving). History starts over on load.
        void Baseline(bool loaded) {
            if (loaded) {
                original.clear();
                undo.clear();
                redo.clear();
            }

            for (RowData& row : rows) {
                if (loaded || row.origin < 0) {
                    row.origin = nextSerial++;
                }

                original[row.origin] = row;
            }

            committed = rows;
        }

        // `step`: always a new undo step (rows added, removed); otherwise edits in quick succession
        // join the current one.
        void Edited(bool step = false) {
            const DWORD now = GetTickCount();

            if (step || now - lastEdit > kUndoGroupMs) {
                undo.push_back(committed);
                redo.clear();

                if (undo.size() > kUndoSteps) {
                    undo.erase(undo.begin());
                }
            }

            lastEdit = step ? 0 : now;
            committed = rows;
            changes = true;
            indexDirty = true;
            generation++;
            Notify(Change::Edited);
        }

        const Field* FieldOf(int field) const {
            return field >= 0 && static_cast<size_t>(field) < schema.FieldCount() ? &schema.FieldAt(field) : nullptr;
        }

        // The cell's column, or -1.
        int Column(int field, uint32_t element) const {
            const Field* f = FieldOf(field);
            return f && element < f->count ? static_cast<int>(f->column + element) : -1;
        }
    };

    // --- Row -----------------------------------------------------------------------------------------

    Row::operator bool() const {
        return m_table && m_index < m_table->RowCount();
    }

    uint32_t Row::Id() const {
        return Raw(0);
    }

    uint32_t Row::Raw(int field, uint32_t element) const {
        if (!*this) {
            return 0;
        }

        const Table::Impl& impl = m_table->GetImpl();
        const int column = impl.Column(field, element);
        return column >= 0 ? impl.rows[m_index].cells[column] : 0;
    }

    int32_t Row::Int(int field, uint32_t element) const {
        return static_cast<int32_t>(Raw(field, element));
    }

    uint32_t Row::Uint(int field, uint32_t element) const {
        return Raw(field, element);
    }

    float Row::Float(int field, uint32_t element) const {
        return BitsFloat(Raw(field, element));
    }

    bool Row::Bool(int field, uint32_t element) const {
        return Raw(field, element) != 0;
    }

    const std::string& Row::String(int field, uint32_t element) const {
        if (!*this) {
            return kEmpty;
        }

        const Table::Impl& impl = m_table->GetImpl();
        const int column = impl.Column(field, element);
        return column >= 0 ? impl.rows[m_index].texts[column] : kEmpty;
    }

    int32_t Row::Int(const char* field, uint32_t element) const {
        return m_table ? Int(m_table->GetSchema().IndexOf(field), element) : 0;
    }

    uint32_t Row::Uint(const char* field, uint32_t element) const {
        return m_table ? Uint(m_table->GetSchema().IndexOf(field), element) : 0;
    }

    float Row::Float(const char* field, uint32_t element) const {
        return m_table ? Float(m_table->GetSchema().IndexOf(field), element) : 0.0f;
    }

    const std::string& Row::String(const char* field, uint32_t element) const {
        return m_table ? String(m_table->GetSchema().IndexOf(field), element) : kEmpty;
    }

    void Row::SetRaw(int field, uint32_t element, uint32_t value) {
        if (!*this) {
            return;
        }

        Table::Impl& impl = m_table->GetImpl();
        const int column = impl.Column(field, element);

        if (column < 0 || impl.IsStringColumn(column) || impl.rows[m_index].cells[column] == value) {
            return;
        }

        impl.rows[m_index].cells[column] = value;
        impl.Edited();
    }

    void Row::SetInt(int field, uint32_t element, int32_t value) {
        SetRaw(field, element, static_cast<uint32_t>(value));
    }

    void Row::SetUint(int field, uint32_t element, uint32_t value) {
        SetRaw(field, element, value);
    }

    void Row::SetFloat(int field, uint32_t element, float value) {
        SetRaw(field, element, FloatBits(value));
    }

    void Row::SetString(int field, uint32_t element, const char* value) {
        if (!*this) {
            return;
        }

        Table::Impl& impl = m_table->GetImpl();
        const int column = impl.Column(field, element);
        const char* text = value ? value : "";

        if (column < 0 || !impl.IsStringColumn(column) || impl.rows[m_index].texts[column] == text) {
            return;
        }

        impl.rows[m_index].texts[column] = text;
        impl.Edited();
    }

    // --- Table ---------------------------------------------------------------------------------------

    Table::Table(Schema schema) : m_impl(new Impl(this, std::move(schema))) {
    }

    Table::~Table() = default;

    const Schema& Table::GetSchema() const {
        return m_impl->schema;
    }

    const std::string& Table::Name() const {
        return m_impl->schema.Name();
    }

    bool Table::Loaded() const {
        return m_impl->loaded;
    }

    const std::string& Table::Status() const {
        return m_impl->status;
    }

    uint32_t Table::Generation() const {
        return m_impl->generation;
    }

    uint32_t Table::RowCount() const {
        return static_cast<uint32_t>(m_impl->rows.size());
    }

    Row Table::At(uint32_t index) {
        return index < RowCount() ? Row(this, index) : Row();
    }

    Row Table::Find(uint32_t id) {
        m_impl->BuildIndex();
        auto found = m_impl->idIndex.find(id);
        return found != m_impl->idIndex.end() ? Row(this, found->second) : Row();
    }

    Row Table::FindBy(int field, uint32_t value, Row after) {
        const int column = m_impl->Column(field, 0);

        if (column < 0) {
            return Row();
        }

        for (uint32_t i = after ? after.Index() + 1 : 0; i < RowCount(); i++) {
            if (m_impl->rows[i].cells[column] == value) {
                return Row(this, i);
            }
        }

        return Row();
    }

    uint32_t Table::DuplicateIds() const {
        m_impl->BuildIndex();
        return m_impl->duplicates;
    }

    Row Table::Add() {
        RowData row = m_impl->NewRow();
        row.cells[0] = m_impl->NextId();
        m_impl->rows.push_back(std::move(row));
        m_impl->Edited(true);
        return Row(this, RowCount() - 1);
    }

    Row Table::Clone(uint32_t index) {
        if (index >= RowCount()) {
            return Row();
        }

        RowData row = m_impl->rows[index];
        row.cells[0] = m_impl->NextId();
        row.origin = -1;
        m_impl->rows.insert(m_impl->rows.begin() + index + 1, std::move(row));
        m_impl->Edited(true);
        return Row(this, index + 1);
    }

    void Table::Remove(uint32_t index) {
        if (index >= RowCount()) {
            return;
        }

        m_impl->rows.erase(m_impl->rows.begin() + index);
        m_impl->Edited(true);
    }

    bool Table::Revert(uint32_t index) {
        if (index >= RowCount()) {
            return false;
        }

        auto found = m_impl->original.find(m_impl->rows[index].origin);

        if (found == m_impl->original.end()) {
            m_impl->rows.erase(m_impl->rows.begin() + index);
            m_impl->Edited(true);
            return false;
        }

        m_impl->rows[index] = found->second;
        m_impl->Edited(true);
        return true;
    }

    bool Table::CanUndo() const {
        return !m_impl->undo.empty();
    }

    bool Table::CanRedo() const {
        return !m_impl->redo.empty();
    }

    void Table::Undo() {
        Impl& impl = *m_impl;

        if (impl.undo.empty()) {
            return;
        }

        impl.redo.push_back(impl.rows);
        impl.rows = impl.undo.back();
        impl.undo.pop_back();
        impl.committed = impl.rows;
        impl.lastEdit = 0;
        impl.changes = true;
        impl.indexDirty = true;
        impl.generation++;
        impl.Notify(Change::Edited);
    }

    void Table::Redo() {
        Impl& impl = *m_impl;

        if (impl.redo.empty()) {
            return;
        }

        impl.undo.push_back(impl.rows);
        impl.rows = impl.redo.back();
        impl.redo.pop_back();
        impl.committed = impl.rows;
        impl.lastEdit = 0;
        impl.changes = true;
        impl.indexDirty = true;
        impl.generation++;
        impl.Notify(Change::Edited);
    }

    void Table::Touch() {
        m_impl->Edited();
    }

    bool Table::HasChanges() const {
        return m_impl->changes;
    }

    bool Table::ChangedOnDisk() const {
        return m_impl->changedOnDisk;
    }

    bool Table::Save(std::string* error) {
        return m_impl->Save(error);
    }

    void Table::Reload() {
        m_impl->Load();
        m_impl->Baseline(true);
        m_impl->changes = false;
        m_impl->changedOnDisk = false;
        m_impl->generation++;
        m_impl->Notify(Change::Reloaded);
    }

    void Table::Subscribe(Listener listener, void* user) {
        if (listener) {
            m_impl->listeners.emplace_back(listener, user);
        }
    }

    void Table::Unsubscribe(Listener listener, void* user) {
        auto& listeners = m_impl->listeners;
        listeners.erase(std::remove(listeners.begin(), listeners.end(), std::make_pair(listener, user)), listeners.end());
    }

    // --- the registry --------------------------------------------------------------------------------

    // Files can be read once the client's archives are up (Start).
    static bool s_started;

    Table* Register(Schema schema) {
        if (Table* existing = Get(schema.Name().c_str())) {
            return existing;
        }

        Tables().push_back(std::unique_ptr<Table>(new Table(std::move(schema))));
        Table* table = Tables().back().get();

        if (s_started) {
            table->Reload();
        }

        return table;
    }

    void Start() {
        if (s_started) {
            return;
        }

        s_started = true;

        for (const std::unique_ptr<Table>& table : Tables()) {
            table->Reload();
        }
    }

    Table* Get(const char* name) {
        for (const std::unique_ptr<Table>& table : Tables()) {
            if (name && _stricmp(table->Name().c_str(), name) == 0) {
                return table.get();
            }
        }

        return nullptr;
    }

    size_t TableCount() {
        return Tables().size();
    }

    Table* TableAt(size_t index) {
        return index < Tables().size() ? Tables()[index].get() : nullptr;
    }

    void Tick() {
        static SXC::Settings::Bool s_hotReload("Data", "HotReload", true, "Hot reload tables",
                                               "Reload a table when its loose file changes on disk (checked once a second). Tables with unsaved edits are only flagged.");

        if (!s_hotReload.Get()) {
            return;
        }

        for (const std::unique_ptr<Table>& table : Tables()) {
            if (table->GetSchema().HotReload()) {
                table->GetImpl().Watch();
            }
        }
    }
}
