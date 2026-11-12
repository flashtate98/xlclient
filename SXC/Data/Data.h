#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "SXC/Data/Schema.h"

// SXC's data layer: custom tables (cdbc) registered with a schema (SXC/Data/Schema.h), loaded, watched
// and saved here, browsed and edited in the overlay's "Data" view.
//
// Loading: the loose file on disk first (it is always current; the client's archive view is fixed at
// startup), then the archives. A missing file is an empty table; saving creates it.
//
// Hot reload: loose files are watched (Tick). A file changed on disk is reloaded, unless the table has
// unsaved edits: then it is only flagged (ChangedOnDisk) and the user decides.
//
// Saving: loose files only, rows sorted by id. The first save of a session keeps the file as it was in
// "<file>.<yyyymmdd-hhmmss>.bak".
//
// Subscribers hear about reloads and edits, so consumers can rebuild what they derived from a table.
// Fields are addressed by their index in the schema (declaration order) or by name. Rows are addressed
// by position; positions change when rows are added, removed or saved (sorted), ids don't.
namespace SXC::Data {
    class Table;

    enum class Change {
        Reloaded,  // read again from disk (or the first load)
        Edited,    // a value, a row added or removed
    };

    using Listener = void (*)(Table& table, Change change, void* user);

    class Row {
    public:
        Row() = default;
        Row(Table* table, uint32_t index) : m_table(table), m_index(index) {}

        explicit operator bool() const;
        uint32_t Index() const { return m_index; }
        uint32_t Id() const;

        uint32_t Raw(int field, uint32_t element = 0) const;
        int32_t Int(int field, uint32_t element = 0) const;
        uint32_t Uint(int field, uint32_t element = 0) const;
        float Float(int field, uint32_t element = 0) const;
        bool Bool(int field, uint32_t element = 0) const;
        const std::string& String(int field, uint32_t element = 0) const;

        // By field name (a lookup each call). A name the schema doesn't have reads 0 / "".
        int32_t Int(const char* field, uint32_t element = 0) const;
        uint32_t Uint(const char* field, uint32_t element = 0) const;
        float Float(const char* field, uint32_t element = 0) const;
        const std::string& String(const char* field, uint32_t element = 0) const;

        void SetRaw(int field, uint32_t element, uint32_t value);
        void SetInt(int field, uint32_t element, int32_t value);
        void SetUint(int field, uint32_t element, uint32_t value);
        void SetFloat(int field, uint32_t element, float value);
        void SetString(int field, uint32_t element, const char* value);

    private:
        Table* m_table = nullptr;
        uint32_t m_index = 0;
    };

    class Table {
    public:
        explicit Table(Schema schema);
        ~Table();

        const Schema& GetSchema() const;
        const std::string& Name() const;

        // Whether a file was read; false: an empty table (no file yet, or it could not be read: Status).
        bool Loaded() const;
        // One line on the state: rows, where from, problems.
        const std::string& Status() const;
        // Changes with every reload and edit: cache what you derive from the table by it.
        uint32_t Generation() const;

        uint32_t RowCount() const;
        Row At(uint32_t index);
        Row Find(uint32_t id);
        // The first row (from `after`'s position on) whose field holds `value`; an empty Row if none.
        Row FindBy(int field, uint32_t value, Row after = Row());
        // Ids used by more than one row (the editor warns about them).
        uint32_t DuplicateIds() const;

        // Editing. The new row gets the next free id (one past the highest) and every field's Default.
        Row Add();
        Row Clone(uint32_t index);
        void Remove(uint32_t index);
        // The row as in the file (loaded or last saved). A row added since is removed: false then.
        bool Revert(uint32_t index);

        // Edits in quick succession (a dragged slider) are one step. Reload starts the history over.
        bool CanUndo() const;
        bool CanRedo() const;
        void Undo();
        void Redo();
        // Marks the table edited and tells subscribers (Row's setters and the calls above do it themselves).
        void Touch();

        bool HasChanges() const;
        bool ChangedOnDisk() const;
        // Writes the loose file. False with the reason in `error` (may be null).
        bool Save(std::string* error = nullptr);
        // Reads the file again; unsaved edits are lost.
        void Reload();

        // `user` identifies the subscription for Unsubscribe.
        void Subscribe(Listener listener, void* user = nullptr);
        void Unsubscribe(Listener listener, void* user = nullptr);

        struct Impl;
        Impl& GetImpl() { return *m_impl; }

    private:
        std::unique_ptr<Impl> m_impl;
    };

    // Registers a table and loads it (a table registered before Start loads then). A name registered
    // before returns that table (the first schema stays). The table lives until the client exits.
    Table* Register(Schema schema);

    // The client's archives are up: load what was registered so far (SXC::Startup).
    void Start();
    Table* Get(const char* name);
    size_t TableCount();
    Table* TableAt(size_t index);

    // Watches the loose files. Called every frame (SXC::Startup registers it).
    void Tick();

    // A button of a table's own beside Add / Clone / Delete in the Data view, for the selected row.
    // `run` returns the row to select (or an empty Row) and may set `note`, shown beside the buttons.
    struct RowAction {
        const char* label;
        const char* tooltip;
        Row (*run)(Table& table, Row selected, std::string& note);
    };
    void AddRowAction(const char* table, RowAction action);

    // The "Data" view's content: the registered tables, the selected one's rows and the selected row's
    // form. Inside an ImGui window.
    void DrawBrowser();

    // The row windows opened from the editor (references, double-clicked rows). Every overlay frame.
    void DrawWindows();
}
