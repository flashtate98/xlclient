/*
 * sxc.settings: typed values in WTF\SXC.ini, shown in the overlay's Settings view.
 * The C side of SXC/Settings/Settings.h; see that header for how the file is kept.
 */

#ifndef SXC_API_SETTINGS_H
#define SXC_API_SETTINGS_H

#include "SXC/Api/sxc.h"

#define SXC_SETTINGS_SERVICE         "sxc.settings"
#define SXC_SETTINGS_SERVICE_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sxc_setting_type
{
    SXC_SETTING_BOOL   = 0,
    SXC_SETTING_INT    = 1,
    SXC_SETTING_FLOAT  = 2,
    SXC_SETTING_STRING = 3
} sxc_setting_type;

/// A declared setting. Lives until the client exits. Zero is never valid.
typedef uint64_t sxc_setting;

typedef struct sxc_settings_api
{
    uint32_t struct_size;

    /**
     * @brief Declares (section, key): read from the ini, or the fallback written there.
     * @param fallback       bool / int / float settings (bool: non-zero is true).
     * @param fallback_text  string settings.
     * @param min, max       int / float: min < max gives a slider.
     * @param label          shown in the Settings view; NULL keeps it out of the view.
     * @param description    tooltip and ini comment; may be NULL.
     */
    sxc_setting(WXL_CALL* declare)(int32_t type, const char* section, const char* key, double fallback, const char* fallback_text,
                                   double min, double max, const char* label, const char* description);

    /// Bool (0 / 1), int and float settings.
    double(WXL_CALL* get_number)(sxc_setting setting);
    void(WXL_CALL* set_number)(sxc_setting setting, double value);

    /// String settings. Valid until the setting changes.
    wxl_str(WXL_CALL* get_text)(sxc_setting setting);
    void(WXL_CALL* set_text)(sxc_setting setting, const char* value);
} sxc_settings_api;

#ifdef __cplusplus
}
#endif

#endif
