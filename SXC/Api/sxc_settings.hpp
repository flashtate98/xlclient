// sxc.settings for C++ mods: the same handles as the client's SXC/Settings/Settings.h. Header-only;
// needs the SDK's wxl/sdk.hpp and the sxc.settings service. Declare settings at WXL_STAGE_READY or
// later (the client reads WTF\SXC.ini relative to its folder).
//
//     sxc::settings::Bool sky("ImmersiveAzeroth", "SkyBodies", true, "Sky bodies", "Draw the sky bodies from the tables.");
//     if (sky.Get()) ...

#pragma once

#include <string_view>
#include "SXC/Api/sxc_settings.h"
#include "wxl/sdk.hpp"

namespace sxc::settings {
    inline const sxc_settings_api* Api() {
        static const sxc_settings_api* s_api =
            wxl::Host() ? static_cast<const sxc_settings_api*>(wxl::Host()->get_service(SXC_SETTINGS_SERVICE, SXC_SETTINGS_SERVICE_VERSION)) : nullptr;
        return s_api;
    }

    namespace detail {
        inline sxc_setting Declare(int32_t type, const char* section, const char* key, double fallback, const char* text, double min, double max,
                                   const char* label, const char* description) {
            return Api() ? Api()->declare(type, section, key, fallback, text, min, max, label, description) : 0;
        }
    }

    class Bool {
    public:
        Bool(const char* section, const char* key, bool fallback, const char* label = nullptr, const char* description = nullptr)
            : m_fallback(fallback), m_handle(detail::Declare(SXC_SETTING_BOOL, section, key, fallback ? 1.0 : 0.0, nullptr, 0, 0, label, description)) {}

        bool Get() const { return m_handle ? Api()->get_number(m_handle) != 0.0 : m_fallback; }
        void Set(bool value) const { if (m_handle) Api()->set_number(m_handle, value ? 1.0 : 0.0); }

    private:
        bool m_fallback;
        sxc_setting m_handle;
    };

    class Int {
    public:
        Int(const char* section, const char* key, int fallback, int min, int max, const char* label = nullptr, const char* description = nullptr)
            : m_fallback(fallback), m_handle(detail::Declare(SXC_SETTING_INT, section, key, fallback, nullptr, min, max, label, description)) {}

        int Get() const { return m_handle ? static_cast<int>(Api()->get_number(m_handle)) : m_fallback; }
        void Set(int value) const { if (m_handle) Api()->set_number(m_handle, value); }

    private:
        int m_fallback;
        sxc_setting m_handle;
    };

    class Float {
    public:
        Float(const char* section, const char* key, float fallback, float min, float max, const char* label = nullptr, const char* description = nullptr)
            : m_fallback(fallback), m_handle(detail::Declare(SXC_SETTING_FLOAT, section, key, fallback, nullptr, min, max, label, description)) {}

        float Get() const { return m_handle ? static_cast<float>(Api()->get_number(m_handle)) : m_fallback; }
        void Set(float value) const { if (m_handle) Api()->set_number(m_handle, value); }

    private:
        float m_fallback;
        sxc_setting m_handle;
    };

    class String {
    public:
        String(const char* section, const char* key, const char* fallback, const char* label = nullptr, const char* description = nullptr)
            : m_fallback(fallback ? fallback : ""), m_handle(detail::Declare(SXC_SETTING_STRING, section, key, 0, fallback, 0, 0, label, description)) {}

        /// Valid until the setting changes.
        std::string_view Get() const {
            if (!m_handle) return m_fallback;
            const wxl_str text = Api()->get_text(m_handle);
            return text.data ? std::string_view(text.data, text.size) : std::string_view();
        }

        void Set(const char* value) const { if (m_handle) Api()->set_text(m_handle, value); }

    private:
        std::string_view m_fallback;
        sxc_setting m_handle;
    };
}
