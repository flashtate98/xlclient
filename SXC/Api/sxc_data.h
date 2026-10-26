/*
 * sxc.data: custom tables (cdbc) with a schema, shown and edited in the overlay's Data view.
 * The C side of SXC/Data/Data.h; see that header for loading, hot reload and saving.
 *
 * A mod builds a schema field by field, registers it, and reads rows through the table handle.
 * Tables live until the client exits, so a table handle can be kept.
 */

#ifndef SXC_API_DATA_H
#define SXC_API_DATA_H

#include "SXC/Api/sxc.h"

#define SXC_DATA_SERVICE         "sxc.data"
#define SXC_DATA_SERVICE_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sxc_field_type
{
    SXC_FIELD_ID          = 0,
    SXC_FIELD_INT         = 1,
    SXC_FIELD_UINT        = 2,
    SXC_FIELD_FLOAT       = 3,
    SXC_FIELD_BOOL        = 4,
    SXC_FIELD_FLAGS       = 5,
    SXC_FIELD_ENUM        = 6,
    SXC_FIELD_STRING      = 7,
    SXC_FIELD_TEXT        = 8,
    SXC_FIELD_COLOR       = 9,
    SXC_FIELD_FOREIGN_KEY = 10,
    SXC_FIELD_PATH        = 11
} sxc_field_type;

typedef enum sxc_color_format
{
    SXC_COLOR_RGB    = 0,
    SXC_COLOR_ARGB   = 1,
    SXC_COLOR_FLOAT3 = 2
} sxc_color_format;

typedef enum sxc_table_change
{
    SXC_CHANGE_RELOADED = 0,
    SXC_CHANGE_EDITED   = 1
} sxc_table_change;

/// sxc_data_api::schema_begin flags.
#define SXC_SCHEMA_NO_HOT_RELOAD 0x1u

/// A schema being built; consumed by schema_register.
typedef uint64_t sxc_schema;
/// A registered table. Zero is never valid.
typedef uint64_t sxc_table;

/// A named value (an enum entry, a special value such as -1 = inherit), or a named flag bit.
typedef struct sxc_named_value
{
    double      value;
    const char* name;
} sxc_named_value;

/// One field. Zero it, set struct_size, type and name; NULL strings and zeros mean "not set".
typedef struct sxc_field_desc
{
    uint32_t    struct_size;
    int32_t     type;          ///< sxc_field_type
    const char* name;
    uint32_t    count;         ///< columns it takes (0 = 1); vectors, arrays
    wxl_bool    vector;        ///< float columns shown as x, y, z(, w)
    int32_t     color_format;  ///< sxc_color_format, for SXC_FIELD_COLOR
    const char* label;         ///< NULL: generated from the name
    const char* tooltip;
    const char* group;
    double      min;           ///< min < max: a slider
    double      max;
    wxl_bool    read_only;
    wxl_bool    hidden;
    double      fallback;      ///< the default: older files and new rows
    const char* target;        ///< SXC_FIELD_FOREIGN_KEY: the table
    const char* display;       ///< SXC_FIELD_FOREIGN_KEY: the target's field shown beside the id
} sxc_field_desc;

typedef void(WXL_CALL* sxc_table_listener)(sxc_table table, int32_t change, void* user);

typedef struct sxc_data_api
{
    uint32_t struct_size;

    /// A new schema for the file at `path` (e.g. "DBFilesClient\\SkyBody.cdbc").
    sxc_schema(WXL_CALL* schema_begin)(const char* name, const char* path, const char* description, uint32_t flags);

    /// Appends a field (its columns follow the previous field's). `values` / `bits` may be NULL.
    wxl_result(WXL_CALL* schema_field)(sxc_schema schema, const sxc_field_desc* field, const sxc_named_value* values,
                                       uint32_t value_count, const sxc_named_value* bits, uint32_t bit_count);

    /// Registers and loads the table; the schema handle is consumed. A name registered before
    /// returns that table. Zero on a bad schema handle.
    sxc_table(WXL_CALL* schema_register)(sxc_schema schema);

    /// A registered table by name, or zero.
    sxc_table(WXL_CALL* find_table)(const char* name);

    wxl_bool(WXL_CALL* loaded)(sxc_table table);
    /// Changes with every reload and edit.
    uint32_t(WXL_CALL* generation)(sxc_table table);
    /// One line on the table's state. Valid until the table changes.
    wxl_str(WXL_CALL* status)(sxc_table table);

    uint32_t(WXL_CALL* row_count)(sxc_table table);
    /// The row index of `id`, or -1.
    int32_t(WXL_CALL* find_row)(sxc_table table, uint32_t id);
    /// The field index of `name`, or -1.
    int32_t(WXL_CALL* field_index)(sxc_table table, const char* name);

    /// A cell's 4 bytes (floats as their bits); 0 for anything out of range.
    uint32_t(WXL_CALL* get_raw)(sxc_table table, uint32_t row, int32_t field, uint32_t element);
    /// A string cell. Valid until the table changes.
    wxl_str(WXL_CALL* get_string)(sxc_table table, uint32_t row, int32_t field, uint32_t element);

    wxl_result(WXL_CALL* set_raw)(sxc_table table, uint32_t row, int32_t field, uint32_t element, uint32_t value);
    wxl_result(WXL_CALL* set_string)(sxc_table table, uint32_t row, int32_t field, uint32_t element, const char* value);

    /// Called after every reload and edit, on the main thread.
    wxl_result(WXL_CALL* subscribe)(sxc_table table, sxc_table_listener listener, void* user);
    wxl_result(WXL_CALL* unsubscribe)(sxc_table table, sxc_table_listener listener, void* user);

    // --- appended 2026-10-23: editing as the Data view does (check struct_size) ---------------------

    /// A new row (the next free id, every field's default) / a copy of row `index` with a new id: its
    /// index, or -1. Row positions change on add, remove and save; ids don't.
    int32_t(WXL_CALL* add_row)(sxc_table table);
    int32_t(WXL_CALL* clone_row)(sxc_table table, uint32_t index);
    wxl_result(WXL_CALL* remove_row)(sxc_table table, uint32_t index);
    /// The row as in the file (loaded or last saved); a row added since is removed (false then).
    wxl_bool(WXL_CALL* revert_row)(sxc_table table, uint32_t index);

    /// The table's undo history (shared with the Data view).
    wxl_bool(WXL_CALL* can_undo)(sxc_table table);
    wxl_bool(WXL_CALL* can_redo)(sxc_table table);
    void(WXL_CALL* undo)(sxc_table table);
    void(WXL_CALL* redo)(sxc_table table);

    /// Unsaved edits; the file changed on disk while there were.
    wxl_bool(WXL_CALL* has_changes)(sxc_table table);
    wxl_bool(WXL_CALL* changed_on_disk)(sxc_table table);
    /// Writes the loose file (the first save of a session keeps a dated .bak). False with the reason
    /// in `error` (may be null).
    wxl_bool(WXL_CALL* save)(sxc_table table, char* error, uint32_t error_size);
    /// Reads the file again; unsaved edits are lost.
    void(WXL_CALL* reload)(sxc_table table);

    /// Draws the Data view's editing form of row `index` into the current Dear ImGui window (a panel
    /// of sxc.ui: the context is shared). A reference followed in it opens a row window.
    void(WXL_CALL* draw_row_form)(sxc_table table, uint32_t index);
    /// Opens the row with `id` in a row window of the overlay.
    void(WXL_CALL* open_row)(sxc_table table, uint32_t id);

    // --- appended 2026-10-26 (check struct_size) ---------------------------------------------------

    /// A renamed table: the file under its former name, read while the new one doesn't exist yet (the
    /// first save writes the new one). Before schema_register.
    void(WXL_CALL* schema_former_path)(sxc_schema schema, const char* path);
} sxc_data_api;

#ifdef __cplusplus
}
#endif

#endif
