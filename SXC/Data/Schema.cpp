#include "SXC/Data/Schema.h"

#include <cctype>
#include <cstring>
#include "SXC/Log.h"

namespace {
    // "GroundEffectID" -> "Ground effect ID", "ZOffset" -> "Z offset": a space before each capital that
    // starts a new word, words after the first in lower case unless they are an acronym.
    std::string LabelFromName(const std::string& name) {
        std::vector<std::string> words;
        std::string word;

        for (size_t i = 0; i < name.size(); i++) {
            const char c = name[i];
            const bool upper = isupper(static_cast<unsigned char>(c)) != 0;
            const bool prevLower = i > 0 && (islower(static_cast<unsigned char>(name[i - 1])) || isdigit(static_cast<unsigned char>(name[i - 1])));
            const bool nextLower = i + 1 < name.size() && islower(static_cast<unsigned char>(name[i + 1]));
            const bool prevUpper = i > 0 && isupper(static_cast<unsigned char>(name[i - 1]));

            if (!word.empty() && upper && (prevLower || (prevUpper && nextLower))) {
                words.push_back(word);
                word.clear();
            }

            word += c;
        }

        if (!word.empty()) {
            words.push_back(word);
        }

        std::string label;

        for (size_t i = 0; i < words.size(); i++) {
            std::string part = words[i];
            const bool acronym = part.size() > 1 && isupper(static_cast<unsigned char>(part[1]));

            if (i > 0 && !acronym) {
                part[0] = static_cast<char>(tolower(static_cast<unsigned char>(part[0])));
            }

            label += (i > 0 ? " " : "") + part;
        }

        return label;
    }
}

namespace SXC::Data {
    Field& Field::Label(const char* text) {
        label = text ? text : "";
        return *this;
    }

    Field& Field::Tooltip(const char* text) {
        tooltip = text ? text : "";
        return *this;
    }

    Field& Field::Group(const char* name) {
        group = name ? name : "";
        return *this;
    }

    Field& Field::Range(double minimum, double maximum) {
        min = minimum;
        max = maximum;
        return *this;
    }

    Field& Field::ReadOnly() {
        readOnly = true;
        return *this;
    }

    Field& Field::Hidden() {
        hidden = true;
        return *this;
    }

    Field& Field::Default(double value) {
        fallback = value;
        return *this;
    }

    Field& Field::Value(double value, const char* name) {
        values.push_back(NamedValue{ value, name ? name : "" });
        return *this;
    }

    Field& Field::Bit(uint32_t mask, const char* name) {
        bits.push_back(NamedValue{ static_cast<double>(mask), name ? name : "" });
        return *this;
    }

    const char* Field::NameOf(double value) const {
        for (const NamedValue& named : values) {
            if (named.value == value) {
                return named.name.c_str();
            }
        }

        return nullptr;
    }

    Schema::Schema(const char* name, const char* path)
        : m_name(name ? name : ""), m_path(path ? path : "") {
    }

    Schema& Schema::Description(const char* text) {
        m_description = text ? text : "";
        return *this;
    }

    Schema& Schema::NoHotReload() {
        m_hotReload = false;
        return *this;
    }

    Schema& Schema::FormerPath(const char* path) {
        m_formerPath = path ? path : "";
        return *this;
    }

    Field& Schema::Add(FieldType type, const char* name, uint32_t count) {
        Field field;
        field.type = type;
        field.name = name ? name : "";
        field.label = LabelFromName(field.name);
        field.column = m_columns;
        field.count = count ? count : 1;

        if (type == FieldType::Id && m_columns != 0) {
            SXC::LogOnce(("data|id|" + m_name).c_str(), "data: %s: the id must be the first field", m_name.c_str());
        }

        m_columns += field.count;
        m_fields.push_back(field);
        return m_fields.back();
    }

    Field& Schema::Id(const char* name) {
        return Add(FieldType::Id, name, 1);
    }

    Field& Schema::Int(const char* name) {
        return Add(FieldType::Int, name, 1);
    }

    Field& Schema::Uint(const char* name) {
        return Add(FieldType::Uint, name, 1);
    }

    Field& Schema::Float(const char* name) {
        return Add(FieldType::Float, name, 1);
    }

    Field& Schema::Bool(const char* name) {
        return Add(FieldType::Bool, name, 1);
    }

    Field& Schema::Flags(const char* name) {
        return Add(FieldType::Flags, name, 1);
    }

    Field& Schema::Enum(const char* name) {
        return Add(FieldType::Enum, name, 1);
    }

    Field& Schema::String(const char* name) {
        return Add(FieldType::String, name, 1);
    }

    Field& Schema::Text(const char* name) {
        return Add(FieldType::Text, name, 1);
    }

    Field& Schema::Color(const char* name, ColorFormat format) {
        Field& field = Add(FieldType::Color, name, format == ColorFormat::Float3 ? 3 : 1);
        field.colorFormat = format;

        if (format == ColorFormat::Argb) {
            field.fallback = static_cast<double>(0xFFFFFFFFu);
        }

        return field;
    }

    Field& Schema::Vector(const char* name, uint32_t count) {
        Field& field = Add(FieldType::Float, name, count);
        field.vector = true;
        return field;
    }

    Field& Schema::Array(const char* name, FieldType type, uint32_t count) {
        return Add(type, name, count);
    }

    Field& Schema::ForeignKey(const char* name, const char* table, const char* display) {
        Field& field = Add(FieldType::ForeignKey, name, 1);
        field.target = table ? table : "";
        field.display = display ? display : "";
        return field;
    }

    Field& Schema::Path(const char* name) {
        return Add(FieldType::Path, name, 1);
    }

    int Schema::IndexOf(const char* name) const {
        for (size_t i = 0; i < m_fields.size(); i++) {
            if (name && _stricmp(m_fields[i].name.c_str(), name) == 0) {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    std::string Schema::ColumnName(size_t field, uint32_t element) const {
        const Field& f = m_fields[field];

        if (f.count == 1) {
            return f.name;
        }

        if (f.type == FieldType::Color) {
            return f.name + "RGB"[element % 3];
        }

        if (f.vector) {
            return f.name + "XYZW"[element % 4];
        }

        return f.name + std::to_string(element + 1);
    }
}
