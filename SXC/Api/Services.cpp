// The tables behind sxc.data, sxc.settings and sxc.ui (SXC/Api/*.h), and the executable's export
// that hands them out (sxc_get_service, found by the sxc.bridge mod).

#include "SXC/Api/sxc_data.h"
#include "SXC/Api/sxc_settings.h"
#include "SXC/Api/sxc_ui.h"
#include "SXC/Api/sxc_sky.h"
#include "SXC/Api/sxc_world.h"
#include "SXC/Api/sxc_fog.h"
#include "SXC/Api/sxc_cover.h"
#include "SXC/Api/sxc_lights.h"
#include "SXC/Api/sxc_grass.h"
#include "SXC/Api/sxc_profiler.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "SXC/Data/Data.h"
#include "SXC/Data/Editor.h"
#include "SXC/Settings/Settings.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "SXC/Ui/Ui.h"

namespace SXC::WorldApi {
    const sxc_world_api* Api();
}

namespace SXC::Sky {
    const sxc_sky_api* Api();
}

namespace SXC::Fog {
    const sxc_fog_api* Api();
}

namespace SXC::Cover {
    const sxc_cover_api* Api();
}

namespace SXC::Lights {
    const sxc_lights_api* Api();
}

namespace SXC::Grass {
    const sxc_grass_api* Api();
}

namespace SXC::Profiler {
    const sxc_profiler_api* Api();
}

namespace {
    using namespace SXC;

    wxl_str Str(const std::string& text) {
        return wxl_str{ text.data(), static_cast<uint32_t>(text.size()) };
    }

    // --- sxc.data ------------------------------------------------------------------------------------

    Data::Table* TableOf(sxc_table table) {
        return reinterpret_cast<Data::Table*>(static_cast<uintptr_t>(table));
    }

    sxc_table HandleOf(Data::Table* table) {
        return static_cast<sxc_table>(reinterpret_cast<uintptr_t>(table));
    }

    std::vector<std::unique_ptr<Data::Schema>> s_schemas;  // being built

    Data::Schema* SchemaOf(sxc_schema schema) {
        for (const std::unique_ptr<Data::Schema>& s : s_schemas) {
            if (reinterpret_cast<uintptr_t>(s.get()) == schema) {
                return s.get();
            }
        }

        return nullptr;
    }

    sxc_schema WXL_CALL SchemaBegin(const char* name, const char* path, const char* description, uint32_t flags) {
        if (!name || !*name || !path || !*path) {
            return 0;
        }

        s_schemas.emplace_back(new Data::Schema(name, path));
        Data::Schema* schema = s_schemas.back().get();
        schema->Description(description);

        if (flags & SXC_SCHEMA_NO_HOT_RELOAD) {
            schema->NoHotReload();
        }

        return reinterpret_cast<uintptr_t>(schema);
    }

    wxl_result WXL_CALL SchemaField(sxc_schema handle, const sxc_field_desc* desc, const sxc_named_value* values, uint32_t valueCount,
                                    const sxc_named_value* bits, uint32_t bitCount) {
        Data::Schema* schema = SchemaOf(handle);

        if (!schema || !desc || desc->struct_size < sizeof(sxc_field_desc) || !desc->name || desc->type < SXC_FIELD_ID || desc->type > SXC_FIELD_PATH) {
            return WXL_E_INVALID;
        }

        const uint32_t count = desc->count ? desc->count : 1;
        Data::Field* field;

        if (desc->type == SXC_FIELD_COLOR) {
            field = &schema->Color(desc->name, static_cast<Data::ColorFormat>(desc->color_format));
        } else if (desc->vector) {
            field = &schema->Vector(desc->name, count);
        } else if (desc->type == SXC_FIELD_FOREIGN_KEY) {
            field = &schema->ForeignKey(desc->name, desc->target, desc->display ? desc->display : "Name");
        } else {
            field = &schema->Array(desc->name, static_cast<Data::FieldType>(desc->type), count);
        }

        if (desc->label) field->Label(desc->label);
        if (desc->tooltip) field->Tooltip(desc->tooltip);
        if (desc->group) field->Group(desc->group);
        if (desc->min < desc->max) field->Range(desc->min, desc->max);
        if (desc->read_only) field->ReadOnly();
        if (desc->hidden) field->Hidden();
        if (desc->fallback != 0.0) field->Default(desc->fallback);

        for (uint32_t i = 0; values && i < valueCount; i++) {
            field->Value(values[i].value, values[i].name);
        }

        for (uint32_t i = 0; bits && i < bitCount; i++) {
            field->Bit(static_cast<uint32_t>(bits[i].value), bits[i].name);
        }

        return WXL_OK;
    }

    sxc_table WXL_CALL SchemaRegister(sxc_schema handle) {
        for (size_t i = 0; i < s_schemas.size(); i++) {
            if (reinterpret_cast<uintptr_t>(s_schemas[i].get()) == handle) {
                Data::Table* table = Data::Register(std::move(*s_schemas[i]));
                s_schemas.erase(s_schemas.begin() + i);
                return HandleOf(table);
            }
        }

        return 0;
    }

    sxc_table WXL_CALL FindTable(const char* name) {
        return HandleOf(Data::Get(name));
    }

    wxl_bool WXL_CALL Loaded(sxc_table table) {
        return table && TableOf(table)->Loaded();
    }

    uint32_t WXL_CALL Generation(sxc_table table) {
        return table ? TableOf(table)->Generation() : 0;
    }

    wxl_str WXL_CALL Status(sxc_table table) {
        return table ? Str(TableOf(table)->Status()) : wxl_str{};
    }

    uint32_t WXL_CALL RowCount(sxc_table table) {
        return table ? TableOf(table)->RowCount() : 0;
    }

    int32_t WXL_CALL FindRow(sxc_table table, uint32_t id) {
        const Data::Row row = table ? TableOf(table)->Find(id) : Data::Row();
        return row ? static_cast<int32_t>(row.Index()) : -1;
    }

    int32_t WXL_CALL FieldIndex(sxc_table table, const char* name) {
        return table ? TableOf(table)->GetSchema().IndexOf(name) : -1;
    }

    uint32_t WXL_CALL GetRaw(sxc_table table, uint32_t row, int32_t field, uint32_t element) {
        return table ? TableOf(table)->At(row).Raw(field, element) : 0;
    }

    wxl_str WXL_CALL GetString(sxc_table table, uint32_t row, int32_t field, uint32_t element) {
        return table ? Str(TableOf(table)->At(row).String(field, element)) : wxl_str{};
    }

    wxl_result WXL_CALL SetRaw(sxc_table table, uint32_t row, int32_t field, uint32_t element, uint32_t value) {
        Data::Row r = table ? TableOf(table)->At(row) : Data::Row();

        if (!r) {
            return WXL_E_NOT_FOUND;
        }

        r.SetRaw(field, element, value);
        return WXL_OK;
    }

    wxl_result WXL_CALL SetString(sxc_table table, uint32_t row, int32_t field, uint32_t element, const char* value) {
        Data::Row r = table ? TableOf(table)->At(row) : Data::Row();

        if (!r) {
            return WXL_E_NOT_FOUND;
        }

        r.SetString(field, element, value);
        return WXL_OK;
    }

    // A mod's listener behind the table's own: the table's Listener has C++ types in its signature.
    struct Subscription {
        sxc_table table;
        sxc_table_listener listener;
        void* user;
    };

    std::vector<std::unique_ptr<Subscription>> s_subscriptions;

    void Forward(Data::Table& table, Data::Change change, void* user) {
        const Subscription* subscription = static_cast<Subscription*>(user);
        subscription->listener(subscription->table, static_cast<int32_t>(change), subscription->user);
    }

    wxl_result WXL_CALL Subscribe(sxc_table table, sxc_table_listener listener, void* user) {
        if (!table || !listener) {
            return WXL_E_INVALID;
        }

        s_subscriptions.emplace_back(new Subscription{ table, listener, user });
        TableOf(table)->Subscribe(Forward, s_subscriptions.back().get());
        return WXL_OK;
    }

    wxl_result WXL_CALL Unsubscribe(sxc_table table, sxc_table_listener listener, void* user) {
        for (size_t i = 0; i < s_subscriptions.size(); i++) {
            const Subscription& s = *s_subscriptions[i];

            if (s.table == table && s.listener == listener && s.user == user) {
                TableOf(table)->Unsubscribe(Forward, s_subscriptions[i].get());
                s_subscriptions.erase(s_subscriptions.begin() + i);
                return WXL_OK;
            }
        }

        return WXL_E_NOT_FOUND;
    }

    int32_t WXL_CALL AddRow(sxc_table table) {
        const Data::Row row = table ? TableOf(table)->Add() : Data::Row();
        return row ? static_cast<int32_t>(row.Index()) : -1;
    }

    int32_t WXL_CALL CloneRow(sxc_table table, uint32_t index) {
        if (!table || index >= TableOf(table)->RowCount()) {
            return -1;
        }

        const Data::Row row = TableOf(table)->Clone(index);
        return row ? static_cast<int32_t>(row.Index()) : -1;
    }

    wxl_result WXL_CALL RemoveRow(sxc_table table, uint32_t index) {
        if (!table || index >= TableOf(table)->RowCount()) {
            return WXL_E_NOT_FOUND;
        }

        TableOf(table)->Remove(index);
        return WXL_OK;
    }

    wxl_bool WXL_CALL RevertRow(sxc_table table, uint32_t index) {
        return table && index < TableOf(table)->RowCount() && TableOf(table)->Revert(index);
    }

    wxl_bool WXL_CALL CanUndo(sxc_table table) {
        return table && TableOf(table)->CanUndo();
    }

    wxl_bool WXL_CALL CanRedo(sxc_table table) {
        return table && TableOf(table)->CanRedo();
    }

    void WXL_CALL Undo(sxc_table table) {
        if (table) TableOf(table)->Undo();
    }

    void WXL_CALL Redo(sxc_table table) {
        if (table) TableOf(table)->Redo();
    }

    wxl_bool WXL_CALL HasChanges(sxc_table table) {
        return table && TableOf(table)->HasChanges();
    }

    wxl_bool WXL_CALL ChangedOnDisk(sxc_table table) {
        return table && TableOf(table)->ChangedOnDisk();
    }

    wxl_bool WXL_CALL Save(sxc_table table, char* error, uint32_t errorSize) {
        std::string why;
        const bool ok = table && TableOf(table)->Save(&why);

        if (error && errorSize) {
            std::snprintf(error, errorSize, "%s", ok ? "" : (table ? why.c_str() : "no table"));
        }

        return ok;
    }

    void WXL_CALL Reload(sxc_table table) {
        if (table) TableOf(table)->Reload();
    }

    void WXL_CALL DrawRowForm(sxc_table table, uint32_t index) {
        if (!table || index >= TableOf(table)->RowCount()) {
            return;
        }

        Data::Table& t = *TableOf(table);
        const Data::Editor::Link link = Data::Editor::DrawRowForm(t, t.At(index));

        if (!link.table.empty()) {
            Data::Editor::OpenRowWindow(link.table, link.id);
        }
    }

    void WXL_CALL OpenRow(sxc_table table, uint32_t id) {
        if (table) Data::Editor::OpenRowWindow(TableOf(table)->Name(), id);
    }

    const sxc_data_api s_data = {
        sizeof(sxc_data_api), SchemaBegin, SchemaField, SchemaRegister, FindTable, Loaded, Generation, Status, RowCount, FindRow,
        FieldIndex, GetRaw, GetString, SetRaw, SetString, Subscribe, Unsubscribe,
        AddRow, CloneRow, RemoveRow, RevertRow, CanUndo, CanRedo, Undo, Redo, HasChanges, ChangedOnDisk, Save, Reload,
        DrawRowForm, OpenRow,
    };

    // --- sxc.settings --------------------------------------------------------------------------------

    struct Setting {
        int32_t type;
        std::unique_ptr<Settings::Bool> b;
        std::unique_ptr<Settings::Int> i;
        std::unique_ptr<Settings::Float> f;
        std::unique_ptr<Settings::String> s;
    };

    std::vector<std::unique_ptr<Setting>> s_settings;

    Setting* SettingOf(sxc_setting setting) {
        return reinterpret_cast<Setting*>(static_cast<uintptr_t>(setting));
    }

    sxc_setting WXL_CALL Declare(int32_t type, const char* section, const char* key, double fallback, const char* fallbackText, double min, double max,
                                 const char* label, const char* description) {
        if (!section || !key || type < SXC_SETTING_BOOL || type > SXC_SETTING_STRING) {
            return 0;
        }

        std::unique_ptr<Setting> setting(new Setting{ type });

        switch (type) {
            case SXC_SETTING_BOOL:
                setting->b.reset(new Settings::Bool(section, key, fallback != 0.0, label, description));
                break;

            case SXC_SETTING_INT:
                setting->i.reset(new Settings::Int(section, key, static_cast<int>(fallback), static_cast<int>(min), static_cast<int>(max), label, description));
                break;

            case SXC_SETTING_FLOAT:
                setting->f.reset(new Settings::Float(section, key, static_cast<float>(fallback), static_cast<float>(min), static_cast<float>(max), label, description));
                break;

            default:
                setting->s.reset(new Settings::String(section, key, fallbackText, label, description));
                break;
        }

        s_settings.push_back(std::move(setting));
        return reinterpret_cast<uintptr_t>(s_settings.back().get());
    }

    double WXL_CALL GetNumber(sxc_setting handle) {
        const Setting* setting = SettingOf(handle);

        if (!setting) return 0.0;
        if (setting->b) return setting->b->Get() ? 1.0 : 0.0;
        if (setting->i) return setting->i->Get();
        if (setting->f) return setting->f->Get();
        return 0.0;
    }

    void WXL_CALL SetNumber(sxc_setting handle, double value) {
        Setting* setting = SettingOf(handle);

        if (!setting) return;
        if (setting->b) setting->b->Set(value != 0.0);
        if (setting->i) setting->i->Set(static_cast<int>(value));
        if (setting->f) setting->f->Set(static_cast<float>(value));
    }

    wxl_str WXL_CALL GetText(sxc_setting handle) {
        const Setting* setting = SettingOf(handle);
        return setting && setting->s ? Str(setting->s->Get()) : wxl_str{};
    }

    void WXL_CALL SetText(sxc_setting handle, const char* value) {
        Setting* setting = SettingOf(handle);

        if (setting && setting->s) {
            setting->s->Set(value);
        }
    }

    const sxc_settings_api s_settingsApi = {
        sizeof(sxc_settings_api), Declare, GetNumber, SetNumber, GetText, SetText,
    };

    // --- sxc.ui --------------------------------------------------------------------------------------

    int32_t WXL_CALL AddModule(const char* name, const char* description) {
        return Ui::AddModule(name, description);
    }

    wxl_result WXL_CALL AddPanel(int32_t module, const char* title, sxc_draw_fn draw, void* user) {
        if (!title || !draw) {
            return WXL_E_INVALID;
        }

        Ui::AddPanel(module, title, reinterpret_cast<Ui::DrawFn>(draw), user);
        return WXL_OK;
    }

    wxl_result WXL_CALL AddQuick(int32_t module, const char* title, sxc_draw_fn draw, void* user) {
        if (!title || !draw) {
            return WXL_E_INVALID;
        }

        Ui::AddQuick(module, title, reinterpret_cast<Ui::DrawFn>(draw), user);
        return WXL_OK;
    }

    wxl_bool WXL_CALL Visible() {
        return Ui::Visible();
    }

    const char* WXL_CALL ImGuiVersion() {
        return IMGUI_VERSION;
    }

    void* WXL_CALL ImGuiContextOf() {
        return ImGui::GetCurrentContext();
    }

    void WXL_CALL ImGuiAllocators(sxc_imgui_alloc_fn* alloc, sxc_imgui_free_fn* free, void** user) {
        ImGuiMemAllocFunc allocFn;
        ImGuiMemFreeFunc freeFn;
        ImGui::GetAllocatorFunctions(&allocFn, &freeFn, user);
        *alloc = allocFn;
        *free = freeFn;
    }

    const sxc_ui_api s_ui = {
        sizeof(sxc_ui_api), AddModule, AddPanel, AddQuick, Visible, ImGuiVersion, ImGuiContextOf, ImGuiAllocators,
    };
}

extern "C" __declspec(dllexport) const void* WXL_CALL sxc_get_service(const char* name, uint32_t version) {
    if (!name) {
        return nullptr;
    }

    if (strcmp(name, SXC_DATA_SERVICE) == 0 && version == SXC_DATA_SERVICE_VERSION) {
        return &s_data;
    }

    if (strcmp(name, SXC_SETTINGS_SERVICE) == 0 && version == SXC_SETTINGS_SERVICE_VERSION) {
        return &s_settingsApi;
    }

    if (strcmp(name, SXC_UI_SERVICE) == 0 && version == SXC_UI_SERVICE_VERSION) {
        return &s_ui;
    }

    if (strcmp(name, SXC_WORLD_SERVICE) == 0 && version == SXC_WORLD_SERVICE_VERSION) {
        return SXC::WorldApi::Api();
    }

    if (strcmp(name, SXC_SKY_SERVICE) == 0 && version == SXC_SKY_SERVICE_VERSION) {
        return SXC::Sky::Api();
    }

    if (strcmp(name, SXC_FOG_SERVICE) == 0 && version == SXC_FOG_SERVICE_VERSION) {
        return SXC::Fog::Api();
    }

    if (strcmp(name, SXC_COVER_SERVICE) == 0 && version == SXC_COVER_SERVICE_VERSION) {
        return SXC::Cover::Api();
    }

    if (strcmp(name, SXC_LIGHTS_SERVICE) == 0 && version == SXC_LIGHTS_SERVICE_VERSION) {
        return SXC::Lights::Api();
    }

    if (strcmp(name, SXC_GRASS_SERVICE) == 0 && version == SXC_GRASS_SERVICE_VERSION) {
        return SXC::Grass::Api();
    }

    if (strcmp(name, SXC_PROFILER_SERVICE) == 0 && version == SXC_PROFILER_SERVICE_VERSION) {
        return SXC::Profiler::Api();
    }

    return nullptr;
}

namespace SXC::Api {
    // Referenced from SXC::Startup, so the linker keeps this object (and its export) in the executable.
    void Keep() {
    }
}
