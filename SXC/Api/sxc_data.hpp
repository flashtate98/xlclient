// sxc.data for C++ mods: the same schema builder as the client's SXC/Data/Schema.h, compiled into the
// mod and turned into sxc_data_api calls by Register(). Header-only; needs the SDK's wxl/sdk.hpp
// (wxl::Host) and the sxc.data service (mod.json: requires sxc.data 1, loadAfter sxc.bridge).
//
//     sxc::data::Schema s("GroundMaterial", "DBFilesClient\\GroundMaterial.cdbc");
//     s.Id();
//     s.String("Name");
//     s.Float("Stiffness").Range(0, 1).Value(-1, "inherit").Default(-1);
//     sxc::data::Table table = s.Register();
//     float stiffness = table.Find(3).Float(table.Field("Stiffness"));

#pragma once

#include <cstddef>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>
#include "SXC/Api/sxc_data.h"
#include "wxl/sdk.hpp"

namespace sxc::data {
    enum class FieldType {
        Id = SXC_FIELD_ID,
        Int = SXC_FIELD_INT,
        Uint = SXC_FIELD_UINT,
        Float = SXC_FIELD_FLOAT,
        Bool = SXC_FIELD_BOOL,
        Flags = SXC_FIELD_FLAGS,
        Enum = SXC_FIELD_ENUM,
        String = SXC_FIELD_STRING,
        Text = SXC_FIELD_TEXT,
        Color = SXC_FIELD_COLOR,
        ForeignKey = SXC_FIELD_FOREIGN_KEY,
        Path = SXC_FIELD_PATH,
    };

    enum class ColorFormat {
        Rgb = SXC_COLOR_RGB,
        Argb = SXC_COLOR_ARGB,
        Float3 = SXC_COLOR_FLOAT3,
    };

    /// The service, or null when the client doesn't offer it (resolved once).
    inline const sxc_data_api* Api() {
        static const sxc_data_api* s_api =
            wxl::Host() ? static_cast<const sxc_data_api*>(wxl::Host()->get_service(SXC_DATA_SERVICE, SXC_DATA_SERVICE_VERSION)) : nullptr;
        return s_api;
    }

    class Field {
    public:
        Field& Label(const char* text) { m_label = text ? text : ""; m_hasLabel = true; return *this; }
        Field& Tooltip(const char* text) { m_tooltip = text ? text : ""; return *this; }
        Field& Group(const char* name) { m_group = name ? name : ""; return *this; }
        Field& Range(double minimum, double maximum) { m_min = minimum; m_max = maximum; return *this; }
        Field& ReadOnly() { m_readOnly = true; return *this; }
        Field& Hidden() { m_hidden = true; return *this; }
        Field& Default(double value) { m_fallback = value; return *this; }
        Field& Value(double value, const char* name) { m_values.push_back({ value, name ? name : "" }); return *this; }
        Field& Bit(uint32_t mask, const char* name) { m_bits.push_back({ static_cast<double>(mask), name ? name : "" }); return *this; }

    private:
        friend class Schema;

        struct Named {
            double value;
            std::string name;
        };

        FieldType m_type = FieldType::Int;
        std::string m_name;
        uint32_t m_count = 1;
        bool m_vector = false;
        ColorFormat m_colorFormat = ColorFormat::Rgb;
        std::string m_label;
        bool m_hasLabel = false;
        std::string m_tooltip;
        std::string m_group;
        double m_min = 0.0;
        double m_max = 0.0;
        bool m_readOnly = false;
        bool m_hidden = false;
        double m_fallback = 0.0;
        std::string m_target;
        std::string m_display;
        std::vector<Named> m_values;
        std::vector<Named> m_bits;
    };

    class Row;

    /// A registered table. Copyable; valid until the client exits.
    class Table {
    public:
        Table() = default;
        explicit Table(sxc_table handle) : m_handle(handle) {}

        explicit operator bool() const { return m_handle != 0; }
        sxc_table Handle() const { return m_handle; }

        bool Loaded() const { return m_handle && Api()->loaded(m_handle); }
        uint32_t Generation() const { return m_handle ? Api()->generation(m_handle) : 0; }
        std::string_view Status() const { return View(m_handle ? Api()->status(m_handle) : wxl_str{}); }
        uint32_t RowCount() const { return m_handle ? Api()->row_count(m_handle) : 0; }
        /// The field's index by name, or -1. Look it up once and keep it.
        int Field(const char* name) const { return m_handle ? Api()->field_index(m_handle, name) : -1; }

        Row At(uint32_t index) const;
        Row Find(uint32_t id) const;

        /// Called after every reload and edit.
        void Subscribe(sxc_table_listener listener, void* user = nullptr) const {
            if (m_handle) Api()->subscribe(m_handle, listener, user);
        }

        static Table Get(const char* name) { return Table(Api() ? Api()->find_table(name) : 0); }

    private:
        static std::string_view View(wxl_str text) { return text.data ? std::string_view(text.data, text.size) : std::string_view(); }

        sxc_table m_handle = 0;
    };

    class Row {
    public:
        Row() = default;
        Row(sxc_table table, uint32_t index) : m_table(table), m_index(index) {}

        explicit operator bool() const { return m_table && m_index < Api()->row_count(m_table); }
        uint32_t Index() const { return m_index; }
        uint32_t Id() const { return Raw(0); }

        uint32_t Raw(int field, uint32_t element = 0) const { return m_table ? Api()->get_raw(m_table, m_index, field, element) : 0; }
        int32_t Int(int field, uint32_t element = 0) const { return static_cast<int32_t>(Raw(field, element)); }
        uint32_t Uint(int field, uint32_t element = 0) const { return Raw(field, element); }
        bool Bool(int field, uint32_t element = 0) const { return Raw(field, element) != 0; }

        float Float(int field, uint32_t element = 0) const {
            const uint32_t bits = Raw(field, element);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        /// Valid until the table changes.
        std::string_view String(int field, uint32_t element = 0) const {
            const wxl_str text = m_table ? Api()->get_string(m_table, m_index, field, element) : wxl_str{};
            return text.data ? std::string_view(text.data, text.size) : std::string_view();
        }

        void SetRaw(int field, uint32_t element, uint32_t value) const {
            if (m_table) Api()->set_raw(m_table, m_index, field, element, value);
        }

        void SetFloat(int field, uint32_t element, float value) const {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            SetRaw(field, element, bits);
        }

        void SetString(int field, uint32_t element, const char* value) const {
            if (m_table) Api()->set_string(m_table, m_index, field, element, value);
        }

    private:
        sxc_table m_table = 0;
        uint32_t m_index = 0;
    };

    inline Row Table::At(uint32_t index) const {
        return index < RowCount() ? Row(m_handle, index) : Row();
    }

    inline Row Table::Find(uint32_t id) const {
        const int32_t index = m_handle ? Api()->find_row(m_handle, id) : -1;
        return index >= 0 ? Row(m_handle, static_cast<uint32_t>(index)) : Row();
    }

    class Schema {
    public:
        Schema(const char* name, const char* path) : m_name(name ? name : ""), m_path(path ? path : "") {}

        Schema& Description(const char* text) { m_description = text ? text : ""; return *this; }
        Schema& NoHotReload() { m_flags |= SXC_SCHEMA_NO_HOT_RELOAD; return *this; }
        /// A renamed table: its file under the former name, read until the first save writes the new one.
        Schema& FormerPath(const char* path) { m_formerPath = path ? path : ""; return *this; }

        Field& Id(const char* name = "ID") { return Add(FieldType::Id, name); }
        Field& Int(const char* name) { return Add(FieldType::Int, name); }
        Field& Uint(const char* name) { return Add(FieldType::Uint, name); }
        Field& Float(const char* name) { return Add(FieldType::Float, name); }
        Field& Bool(const char* name) { return Add(FieldType::Bool, name); }
        Field& Flags(const char* name) { return Add(FieldType::Flags, name); }
        Field& Enum(const char* name) { return Add(FieldType::Enum, name); }
        Field& String(const char* name) { return Add(FieldType::String, name); }
        Field& Text(const char* name) { return Add(FieldType::Text, name); }
        Field& Path(const char* name) { return Add(FieldType::Path, name); }

        Field& Color(const char* name, ColorFormat format = ColorFormat::Rgb) {
            Field& field = Add(FieldType::Color, name);
            field.m_colorFormat = format;
            field.m_count = format == ColorFormat::Float3 ? 3 : 1;
            return field;
        }

        Field& Vector(const char* name, uint32_t count = 3) {
            Field& field = Add(FieldType::Float, name);
            field.m_vector = true;
            field.m_count = count;
            return field;
        }

        Field& Array(const char* name, FieldType type, uint32_t count) {
            Field& field = Add(type, name);
            field.m_count = count;
            return field;
        }

        Field& ForeignKey(const char* name, const char* table, const char* display = "Name") {
            Field& field = Add(FieldType::ForeignKey, name);
            field.m_target = table ? table : "";
            field.m_display = display ? display : "";
            return field;
        }

        /// Registers and loads the table. An empty Table when the service is missing or the client
        /// refused the schema (its log says why).
        Table Register() const {
            const sxc_data_api* api = Api();

            if (!api) {
                return Table();
            }

            const sxc_schema schema = api->schema_begin(m_name.c_str(), m_path.c_str(), m_description.c_str(), m_flags);

            if (!m_formerPath.empty() && api->struct_size >= offsetof(sxc_data_api, schema_former_path) + sizeof(api->schema_former_path)) {
                api->schema_former_path(schema, m_formerPath.c_str());
            }

            for (const Field& f : m_fields) {
                sxc_field_desc desc = {};
                desc.struct_size = sizeof(desc);
                desc.type = static_cast<int32_t>(f.m_type);
                desc.name = f.m_name.c_str();
                desc.count = f.m_count;
                desc.vector = f.m_vector;
                desc.color_format = static_cast<int32_t>(f.m_colorFormat);
                desc.label = f.m_hasLabel ? f.m_label.c_str() : nullptr;
                desc.tooltip = f.m_tooltip.empty() ? nullptr : f.m_tooltip.c_str();
                desc.group = f.m_group.empty() ? nullptr : f.m_group.c_str();
                desc.min = f.m_min;
                desc.max = f.m_max;
                desc.read_only = f.m_readOnly;
                desc.hidden = f.m_hidden;
                desc.fallback = f.m_fallback;
                desc.target = f.m_target.empty() ? nullptr : f.m_target.c_str();
                desc.display = f.m_display.c_str();

                std::vector<sxc_named_value> values;
                std::vector<sxc_named_value> bits;

                for (const Field::Named& named : f.m_values) values.push_back({ named.value, named.name.c_str() });
                for (const Field::Named& named : f.m_bits) bits.push_back({ named.value, named.name.c_str() });

                api->schema_field(schema, &desc, values.data(), static_cast<uint32_t>(values.size()), bits.data(), static_cast<uint32_t>(bits.size()));
            }

            return Table(api->schema_register(schema));
        }

    private:
        Field& Add(FieldType type, const char* name) {
            m_fields.emplace_back();
            Field& field = m_fields.back();
            field.m_type = type;
            field.m_name = name ? name : "";
            return field;
        }

        std::string m_name;
        std::string m_path;
        std::string m_formerPath;
        std::string m_description;
        uint32_t m_flags = 0;
        std::deque<Field> m_fields;  // a deque: the Field& handed out stay valid
    };
}
