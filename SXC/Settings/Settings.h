#pragma once

#include <string>

// SXC's settings: typed values in WTF\SXC.ini, editable in the overlay's "Settings" view.
//
// A setting is (section, key). Declaring one reads its value from the ini, or uses the fallback and
// writes it there (with the description as a comment), so the file lists everything that can be set.
// Two declarations of the same (section, key) share one value. Handles can be globals: the registry
// is built on first use.
//
// Saving: Set marks the file dirty; it is written shortly after the last change (Tick) and when the
// client exits. Only known keys are rewritten in place: comments, order and keys of settings not
// declared this session survive. Editing the ini while the client runs is overwritten by the next save.
//
// `label` is what the Settings view shows; null keeps the setting out of the view (internal state such
// as the Quick strip's picks). `description` is the tooltip and the ini comment (may be null).
// Strings are copied.
namespace SXC::Settings {
    class Bool {
    public:
        Bool(const char* section, const char* key, bool fallback, const char* label = nullptr, const char* description = nullptr);
        bool Get() const;
        void Set(bool value);

    private:
        void* m_entry;
    };

    // `min` < `max`: a slider in that range; the ini may still hold values outside it.
    class Int {
    public:
        Int(const char* section, const char* key, int fallback, int min, int max, const char* label = nullptr, const char* description = nullptr);
        int Get() const;
        void Set(int value);

    private:
        void* m_entry;
    };

    class Float {
    public:
        Float(const char* section, const char* key, float fallback, float min, float max, const char* label = nullptr, const char* description = nullptr);
        float Get() const;
        void Set(float value);

    private:
        void* m_entry;
    };

    // One line; leading and trailing spaces are not kept.
    class String {
    public:
        String(const char* section, const char* key, const char* fallback, const char* label = nullptr, const char* description = nullptr);
        const std::string& Get() const;
        void Set(const char* value);

    private:
        void* m_entry;
    };

    // Writes the file when something changed and nothing else did for a moment. Called every frame.
    void Tick();

    // Writes the file now if anything changed.
    void Save();

    // The Settings view's content: every labelled setting, a collapsible group per section. Inside an
    // ImGui window.
    void DrawEditor();
}
