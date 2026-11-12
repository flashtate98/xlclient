#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// The description of a custom table (cdbc: a WDBC file of 4-byte columns), rich enough for an editor
// to be generated from it. A schema lists its fields in column order; most fields take one column,
// vectors, colours in float form and arrays take several.
//
//     Schema schema("GroundMaterial", "DBFilesClient\\GroundMaterial.cdbc");
//     schema.Id();
//     schema.String("Name");
//     schema.ForeignKey("Parent", "GroundMaterial").Value(0, "none").Tooltip("Material whose values a -1 field takes.");
//     schema.Float("RestMoisture").Range(0, 1).Value(-1, "inherit");
//
// Columns are only ever appended to a table: a file without the newest columns still loads, its rows
// read the field's Default there, and saving writes the full schema.
namespace SXC::Data {
    enum class FieldType {
        Id,          // the row id; the first field
        Int,         // signed
        Uint,
        Float,
        Bool,        // 0 / 1
        Flags,       // named bits (Bit)
        Enum,        // named values (Value), a dropdown; values without a name stay allowed
        String,      // one line
        Text,        // a string edited over several lines, wrapped
        Color,       // see ColorFormat
        ForeignKey,  // the id of a row in another table
        Path,        // a game file path (texture, model)
    };

    enum class ColorFormat {
        Rgb,     // one column, 0xRRGGBB
        Argb,    // one column, 0xAARRGGBB
        Float3,  // three float columns, 0..1
    };

    // A named value: an enum entry, or a special value of a number (-1 = "inherit"). For Flags, the bit mask.
    struct NamedValue {
        double value;
        std::string name;
    };

    struct Field {
        FieldType type = FieldType::Int;
        std::string name;
        uint32_t column = 0;  // the first column it takes
        uint32_t count = 1;   // columns it takes: vector size, array length, 3 for Float3 colours
        bool vector = false;  // the elements are x, y, z(, w), shown on one line

        std::string label;    // shown instead of the name; generated from the name when not set
        std::string tooltip;
        std::string group;    // fields of a group are shown together
        double min = 0.0;     // min < max: a slider in that range
        double max = 0.0;
        bool readOnly = false;
        bool hidden = false;
        double fallback = 0.0;  // Default
        std::vector<NamedValue> values;  // Value
        std::vector<NamedValue> bits;    // Bit
        ColorFormat colorFormat = ColorFormat::Rgb;
        std::string target;   // ForeignKey: the table
        std::string display;  // ForeignKey: the target's field shown beside the id

        Field& Label(const char* text);
        Field& Tooltip(const char* text);
        Field& Group(const char* name);
        Field& Range(double minimum, double maximum);
        Field& ReadOnly();
        Field& Hidden();
        // What rows read where the file is too old to have this column, and what new rows start with.
        Field& Default(double value);
        Field& Value(double value, const char* name);
        Field& Bit(uint32_t mask, const char* name);

        bool IsString() const { return type == FieldType::String || type == FieldType::Text || type == FieldType::Path; }
        bool IsFloat() const { return type == FieldType::Float || (type == FieldType::Color && colorFormat == ColorFormat::Float3); }
        bool IsSigned() const { return type == FieldType::Int || type == FieldType::Enum; }
        const char* NameOf(double value) const;  // the Value name, or null
    };

    class Schema {
    public:
        Schema(const char* name, const char* path);

        Schema& Description(const char* text);
        // The file is not reloaded when it changes on disk (only by the Reload button): for tables whose
        // consumers can't take new data mid-session.
        Schema& NoHotReload();
        // The file the table had under an older name: read while the new file doesn't exist yet (the
        // first save writes the new one; the old one is left as it is).
        Schema& FormerPath(const char* path);

        Field& Id(const char* name = "ID");
        Field& Int(const char* name);
        Field& Uint(const char* name);
        Field& Float(const char* name);
        Field& Bool(const char* name);
        Field& Flags(const char* name);
        Field& Enum(const char* name);
        Field& String(const char* name);
        Field& Text(const char* name);
        // Float3 takes three columns: name + R, G, B.
        Field& Color(const char* name, ColorFormat format = ColorFormat::Rgb);
        // `count` float columns: name + X, Y, Z, W.
        Field& Vector(const char* name, uint32_t count = 3);
        // `count` columns of one type: name + 1, 2, ...
        Field& Array(const char* name, FieldType type, uint32_t count);
        Field& ForeignKey(const char* name, const char* table, const char* display = "Name");
        Field& Path(const char* name);

        const std::string& Name() const { return m_name; }
        const std::string& FilePath() const { return m_path; }
        const std::string& GetFormerPath() const { return m_formerPath; }
        const std::string& GetDescription() const { return m_description; }
        bool HotReload() const { return m_hotReload; }
        uint32_t Columns() const { return m_columns; }
        size_t FieldCount() const { return m_fields.size(); }
        const Field& FieldAt(size_t index) const { return m_fields[index]; }
        // The field's index, or -1.
        int IndexOf(const char* name) const;
        // The column's own name (vector and array columns get their suffix).
        std::string ColumnName(size_t field, uint32_t element) const;

    private:
        Field& Add(FieldType type, const char* name, uint32_t count);

        std::string m_name;
        std::string m_path;
        std::string m_formerPath;
        std::string m_description;
        std::deque<Field> m_fields;  // a deque: the Field& handed out stay valid
        uint32_t m_columns = 0;
        bool m_hotReload = true;
    };
}
