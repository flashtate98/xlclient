// The underwater medium (SXC/Water/Medium.h).
#include "SXC/Water/Medium.h"
#include "SXC/Water/Ride.h"
#include "SXC/Water/Surface.h"
#include "SXC/Water/Water.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "SXC/Data/Data.h"
#include "SXC/Settings/Settings.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/World.h"

namespace SXC::Water {
    namespace {
        // One depth keyframe: how the water looks at this depth, along the distance from the camera.
        struct Key {
            float depth = 0.0f;           // yd below the surface
            float visibility = 0.0f;      // yd until things vanish into the glow; 0 the stock fog's end
            float loss[3] = { 15.0f, 45.0f, 90.0f };   // yd until red, green, blue are halved; 0 never
            float desaturate = 0.0f;      // yd until grey; 0 never
            ColourMode mode = ColourMode::Stock;
            float colour[3] = { 0.1f, 0.35f, 0.45f };  // the glow's colour (Hue, Replace)
            float brightness = 1.0f;      // x the glow
            float fadeInto = 1.0f;        // what far things fade into: 1 the glow, 0 darkness
            float light = 1.0f;           // how much of the sun and sky reach this depth
            float lightColour[3] = { 1.0f, 1.0f, 1.0f };
            bool lightByDepth = false;    // the tint from the channel losses over the depth (x lightColour)
            float lightLossScale = 10.0f; // by depth: the light from above loses its colour this many times slower than the view
            float darkFloor = 0.25f;      // the least of the ambient that is left
            float lampHalo = 0.03f;       // x our lamps lighting the water around them
        };

        // Without rows: the stock fog's reach and colour, the channels lost with distance, the light dimming
        // to dark (not black) by 600 yd.
        std::vector<Key> DefaultKeys() {
            Key a;
            Key b = a;
            b.depth = 100.0f;
            b.light = 0.5f;
            Key c = a;
            c.depth = 350.0f;
            c.light = 0.15f;
            c.fadeInto = 0.6f;
            Key d = a;
            d.depth = 600.0f;
            d.light = 0.05f;
            d.fadeInto = 0.35f;
            d.darkFloor = 0.15f;
            return { a, b, c, d };
        }

        Data::Schema MakeSchema() {
            Data::Schema s("UnderwaterMedium", "DBFilesClient\\UnderwaterMedium.cdbc");
            s.Description("The water seen from inside, per place: depth keyframes (a row per depth) for a LiquidType (0: every water) and "
                          "Scope (LiquidSurface's rules: empty everywhere, a map's folder name, an area's name or \"area:<ID>\", an ADT's "
                          "\"Map_x_y\"). The most specific Scope with rows owns the whole curve; its rows blend by depth. The Water module's "
                          "Underwater panel shows the values at the camera.");
            s.Id();
            s.Uint("LiquidType").Group("Place").Tooltip("LiquidType.dbc's ID (1 Water, 2 Ocean, ...); 0: every water.");
            s.String("Scope").Group("Place").Tooltip("Empty: everywhere. A map's folder name, an area's name or \"area:<AreaTable ID>\" (with its "
                                                     "sub-areas), an ADT's name (\"Kalimdor_23_30\"). Any case.");
            s.Float("Depth").Group("Place").Default(0).Range(0, 3000).Tooltip("yd below the water's surface this row describes.");
            s.Float("Visibility").Group("Distance").Default(0).Range(0, 2000)
                .Tooltip("yd until things vanish into the water's glow (95%); also how far the world draws under water. 0: the stock "
                         "underwater fog's end.");
            s.Float("RedLoss").Group("Distance").Default(15).Range(0, 2000).Tooltip("yd until red is halved (0 never): far things turn green, then blue.");
            s.Float("GreenLoss").Group("Distance").Default(45).Range(0, 2000).Tooltip("yd until green is halved (0 never).");
            s.Float("BlueLoss").Group("Distance").Default(90).Range(0, 2000).Tooltip("yd until blue is halved (0 never).");
            s.Float("Desaturate").Group("Distance").Default(0).Range(0, 2000).Tooltip("yd until things are grey (0 never): the stylised loss of colour.");
            s.Enum("ColourMode").Group("Glow").Value(0, "Stock").Value(1, "Hue").Value(2, "Replace")
                .Tooltip("The glow's colour (what far things fade into). Stock: the stock underwater fog colour, through the day. Hue: its "
                         "brightness, our hue. Replace: our colour.");
            s.Color("Colour").Group("Glow").Default(0x1A5973).Tooltip("Hue, Replace.");
            s.Float("Brightness").Group("Glow").Default(1).Range(0, 4).Tooltip("x the glow.");
            s.Float("FadeInto").Group("Glow").Default(1).Range(0, 1)
                .Tooltip("What far things fade into: 1 the glow (the stock look), 0 darkness. Players can clamp it upwards (Liquid "
                         "MinUnderwaterGlow).");
            s.Float("LightFromAbove").Group("Light").Default(1).Range(0, 1).Tooltip("How much of the sun and sky reach this depth (the sun and ambient light).");
            s.Enum("LightColourMode").Group("Light").Value(0, "Colour").Value(1, "By depth")
                .Tooltip("The light from above's tint. Colour: LightColour. By depth: the light loses its colours on the way down like the "
                         "view does with distance (RedLoss, GreenLoss, BlueLoss, LightLossScale times slower), red first, x LightColour; "
                         "its brightness stays LightFromAbove's.");
            s.Color("LightColour").Group("Light").Default(0xFFFFFF).Tooltip("x the light from above: its tint at this depth.");
            s.Float("LightLossScale").Group("Light").Default(10).Range(1, 200)
                .Tooltip("By depth: how many times slower the light from above loses its colours than the view does with distance.");
            s.Float("DarkFloor").Group("Light").Default(0.25).Range(0, 1).Tooltip("The least of the ambient light that is left, however deep (darker, not black).");
            s.Text("Comment").Tooltip("Free text for the editor; never read.");
            // After the comment: added 2026-11-01, and older files are read by position (they lack these).
            s.Float("LampHalo").Group("Glow").Default(0.03).Range(0, 1)
                .Tooltip("Our lamps light the water around them (a halo, brighter looking toward the lamp): x their light scattered "
                         "by the water (1: 5% per yd; Seyri's 0.03 with today's strong lamps). 0 off.");
            return s;
        }

        SXC::Settings::Bool& MediumOn() {
            static SXC::Settings::Bool s_setting("Liquid", "Medium", true, "The underwater medium",
                                                 "Under our water: colours lost with distance (red first), the water's glow, the light dimming with "
                                                 "depth, per place (UnderwaterMedium). Off: the stock underwater fog.");
            return s_setting;
        }

        SXC::Settings::Bool& WaterlineOn() {
            static SXC::Settings::Bool s_setting("Liquid", "Waterline", true, "The waterline",
                                                 "The camera half in the water: below the moving surface the medium, above it the air, a line "
                                                 "between; a wet lens for a moment after surfacing.");
            return s_setting;
        }

        SXC::Settings::Float& MinGlow() {
            static SXC::Settings::Float s_setting("Liquid", "MinUnderwaterGlow", 0.0f, 0.0f, 1.0f, "Minimum underwater glow",
                                                  "Accessibility: far things under water fade into at least this much of the water's glow, "
                                                  "never into full darkness, wherever a place sets it darker.");
            return s_setting;
        }

        void Rgb(uint32_t rgb, float out[3]) {
            out[0] = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
            out[1] = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
            out[2] = static_cast<float>(rgb & 0xFF) / 255.0f;
        }

        std::string Lower(std::string text) {
            for (char& c : text) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            const size_t begin = text.find_first_not_of(" \t");
            return begin == std::string::npos ? std::string() : text.substr(begin, text.find_last_not_of(" \t") + 1 - begin);
        }

        struct Row {
            uint32_t liquidType = 0;
            std::string scope;   // lower case, trimmed
            Key key;
        };

        Data::Table* s_table = nullptr;
        uint32_t s_generation = ~0u;
        std::vector<Row> s_rows;

        MediumFn s_render = nullptr;
        MediumStatusFn s_status = nullptr;

        // The curve where the player is: rebuilt a few times a second and when the table changes.
        std::vector<Key> s_keys = DefaultKeys();
        std::string s_keysFrom = "the defaults";
        uint32_t s_keysType = ~0u;
        uint32_t s_keysGeneration = ~0u;
        std::chrono::steady_clock::time_point s_keysChecked;

        // This frame's medium (MediumLighting), smoothed while under water; what the composite draws.
        bool s_active = false;
        bool s_wasActive = false;
        float s_depth = 0.0f;
        float s_visibility = 0.0f;   // yd, after the stock fallback
        MediumFrame s_frame;
        float s_lightTint[3] = { 1.0f, 1.0f, 1.0f };
        // The waterline: the lens's clock, whether the camera was under the moving surface, how wet the lens
        // still is (1 just surfaced); the last stock underwater fog (colour, end) for frames above the still
        // surface.
        float s_seconds = 0.0f;
        bool s_cameraWasUnder = false;
        float s_wet = 0.0f;
        float s_stock[3] = { 0.1f, 0.3f, 0.4f };
        float s_stockEnd = 0.0f;
        std::chrono::steady_clock::time_point s_lastFrame;

        // The panel: a depth to preview instead of the camera's, the debug view.
        bool s_preview = false;
        float s_previewDepth = 0.0f;
        int s_debug = 0;

        void ReadRows() {
            if (!s_table || s_table->Generation() == s_generation) {
                return;
            }

            s_generation = s_table->Generation();
            s_rows.clear();
            const Data::Schema& schema = s_table->GetSchema();
            const int type = schema.IndexOf("LiquidType"), scope = schema.IndexOf("Scope"), depth = schema.IndexOf("Depth"),
                      visibility = schema.IndexOf("Visibility"), red = schema.IndexOf("RedLoss"), green = schema.IndexOf("GreenLoss"),
                      blue = schema.IndexOf("BlueLoss"), desaturate = schema.IndexOf("Desaturate"), mode = schema.IndexOf("ColourMode"),
                      colour = schema.IndexOf("Colour"), brightness = schema.IndexOf("Brightness"), fade = schema.IndexOf("FadeInto"),
                      light = schema.IndexOf("LightFromAbove"), lightColour = schema.IndexOf("LightColour"), floor = schema.IndexOf("DarkFloor"),
                      lightMode = schema.IndexOf("LightColourMode"), lightScale = schema.IndexOf("LightLossScale"),
                      halo = schema.IndexOf("LampHalo");

            for (uint32_t i = 0; i < s_table->RowCount(); i++) {
                const Data::Row row = s_table->At(i);
                Row r;
                r.liquidType = row.Uint(type);
                r.scope = Lower(row.String(scope));
                Key& k = r.key;
                k.depth = row.Float(depth);
                k.visibility = row.Float(visibility);
                k.loss[0] = row.Float(red);
                k.loss[1] = row.Float(green);
                k.loss[2] = row.Float(blue);
                k.desaturate = row.Float(desaturate);
                const int32_t m = row.Int(mode);
                k.mode = m == 1 ? ColourMode::Hue : (m == 2 ? ColourMode::Replace : ColourMode::Stock);
                Rgb(row.Uint(colour), k.colour);
                k.brightness = row.Float(brightness);
                k.fadeInto = row.Float(fade);
                k.light = row.Float(light);
                Rgb(row.Uint(lightColour), k.lightColour);
                k.darkFloor = row.Float(floor);
                k.lightByDepth = row.Int(lightMode) == 1;
                k.lightLossScale = row.Float(lightScale);
                k.lampHalo = halo >= 0 ? row.Float(halo) : 0.03f;
                s_rows.push_back(std::move(r));
            }
        }

        // The rows of the most specific Scope that applies here for a LiquidType, by depth; false: none.
        bool KeysFor(uint32_t liquidType, std::vector<Key>& keys, std::string& from) {
            int32_t bestRank = -1;
            const Row* best = nullptr;

            for (const Row& r : s_rows) {
                if (r.liquidType != liquidType) {
                    continue;
                }

                const int32_t rank = ScopeRankHere(r.scope);

                if (rank > bestRank) {
                    bestRank = rank;
                    best = &r;
                }
            }

            if (!best) {
                return false;
            }

            keys.clear();

            for (const Row& r : s_rows) {
                if (r.liquidType == liquidType && r.scope == best->scope) {
                    keys.push_back(r.key);
                }
            }

            std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.depth < b.depth; });
            char text[160];
            std::snprintf(text, sizeof text, "%s%s (LiquidType %u, %u rows)", best->scope.empty() ? "everywhere" : "scope ",
                          best->scope.c_str(), liquidType, static_cast<unsigned>(keys.size()));
            from = text;
            return true;
        }

        void UpdateKeys(uint32_t liquidType) {
            ReadRows();
            const auto now = std::chrono::steady_clock::now();

            if (s_keysType == liquidType && s_keysGeneration == s_generation && now - s_keysChecked < std::chrono::milliseconds(250)) {
                return;
            }

            s_keysChecked = now;
            s_keysType = liquidType;
            s_keysGeneration = s_generation;

            if (!KeysFor(liquidType, s_keys, s_keysFrom) && !(liquidType != 0 && KeysFor(0, s_keys, s_keysFrom))) {
                s_keys = DefaultKeys();
                s_keysFrom = "the defaults (no rows)";
            }
        }

        float Mix(float a, float b, float t) {
            return a + (b - a) * t;
        }

        // A keyframe as it blends: distances as rates per yd (0 never), so "never" blends towards a number through
        // slow rates, not through a halving every half yard (black); the stock visibility resolved.
        struct MediumLook {
            float visibility = 100.0f;  // yd
            bool stockVisibility = true;
            float rate[3] = {};         // per yd: log2 of each channel's loss
            float greyRate = 0.0f;      // per yd towards grey
            ColourMode mode = ColourMode::Stock;
            float colour[3] = {};
            float brightness = 1.0f;
            float fadeInto = 1.0f;
            float light = 1.0f;
            float lightColour[3] = { 1.0f, 1.0f, 1.0f };
            bool lightByDepth = false;
            float lightLossScale = 10.0f;
            float darkFloor = 0.25f;
            float lampHalo = 0.03f;
        };

        MediumLook s_now;   // this frame's, smoothed (MediumLighting)

        MediumLook MediumLookOf(const Key& k, float stockVisibility) {
            MediumLook l;
            l.stockVisibility = !(k.visibility > 0.0f);
            l.visibility = l.stockVisibility ? stockVisibility : k.visibility;

            for (int c = 0; c < 3; c++) {
                l.rate[c] = k.loss[c] > 0.0f ? 1.0f / k.loss[c] : 0.0f;
                l.colour[c] = k.colour[c];
                l.lightColour[c] = k.lightColour[c];
            }

            l.greyRate = k.desaturate > 0.0f ? 1.0f / k.desaturate : 0.0f;
            l.mode = k.mode;
            l.brightness = k.brightness;
            l.fadeInto = k.fadeInto;
            l.light = k.light;
            l.lightByDepth = k.lightByDepth;
            l.lightLossScale = k.lightLossScale;
            l.darkFloor = k.darkFloor;
            l.lampHalo = k.lampHalo;
            return l;
        }

        // Between two looks; what can't blend (the modes) is `discrete`'s.
        MediumLook Blend(const MediumLook& a, const MediumLook& b, float t, const MediumLook& discrete) {
            MediumLook l = discrete;
            l.visibility = Mix(a.visibility, b.visibility, t);

            for (int c = 0; c < 3; c++) {
                l.rate[c] = Mix(a.rate[c], b.rate[c], t);
                l.colour[c] = Mix(a.colour[c], b.colour[c], t);
                l.lightColour[c] = Mix(a.lightColour[c], b.lightColour[c], t);
            }

            l.greyRate = Mix(a.greyRate, b.greyRate, t);
            l.brightness = Mix(a.brightness, b.brightness, t);
            l.fadeInto = Mix(a.fadeInto, b.fadeInto, t);
            l.light = Mix(a.light, b.light, t);
            l.lightLossScale = Mix(a.lightLossScale, b.lightLossScale, t);
            l.darkFloor = Mix(a.darkFloor, b.darkFloor, t);
            l.lampHalo = Mix(a.lampHalo, b.lampHalo, t);
            return l;
        }

        MediumLook At(const std::vector<Key>& keys, float depth, float stockVisibility) {
            if (keys.empty()) {
                return MediumLookOf(Key(), stockVisibility);
            }

            if (depth <= keys.front().depth) {
                return MediumLookOf(keys.front(), stockVisibility);
            }

            for (size_t i = 1; i < keys.size(); i++) {
                if (depth <= keys[i].depth) {
                    const float span = keys[i].depth - keys[i - 1].depth;
                    const float t = span > 0.0f ? (depth - keys[i - 1].depth) / span : 1.0f;
                    const MediumLook a = MediumLookOf(keys[i - 1], stockVisibility), b = MediumLookOf(keys[i], stockVisibility);
                    return Blend(a, b, t, t < 0.5f ? a : b);
                }
            }

            return MediumLookOf(keys.back(), stockVisibility);
        }

        // Is the camera in a water our shader draws, with the medium on?
        bool CameraInOurWater(uint32_t& liquidType, float& depth) {
            depth = 0.0f;
            liquidType = static_cast<uint32_t>(World::SceneCamLiquidStatus(&depth));

            if (!liquidType || !s_render || !MediumOn().Get() || !SXC::Water::Active()) {
                return false;
            }

            const LiquidTypeRec* rec = g_liquidTypeDB.GetRecord(static_cast<int32_t>(liquidType));
            return rec && (rec->m_materialID == 1 || rec->m_materialID == 3);
        }

        void Scale(CImVector& c, const float by[3]) {
            auto one = [](uint8_t v, float f) {
                const float x = static_cast<float>(v) * (f < 0.0f ? 0.0f : f) + 0.5f;
                return static_cast<uint8_t>(x > 255.0f ? 255.0f : x);
            };
            c.r = one(c.r, by[0]);
            c.g = one(c.g, by[1]);
            c.b = one(c.b, by[2]);
        }
    }

    void SetMediumRenderer(MediumFn render, MediumStatusFn status) {
        s_render = render;
        s_status = status;
    }

    void RegisterMedium() {
        if (!s_table) {
            s_table = Data::Register(MakeSchema());
        }
    }

    bool MediumTakesLight() {
        uint32_t type = 0;
        float depth = 0.0f;
        return CameraInOurWater(type, depth);
    }

    void MediumLighting() {
        uint32_t liquidType = 0;
        float depth = 0.0f;
        s_wasActive = s_active;
        const bool under = CameraInOurWater(liquidType, depth);
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - s_lastFrame).count();
        s_lastFrame = now;
        s_seconds = std::fmod(s_seconds + (dt > 0.0f && dt < 1.0f ? dt : 0.0f), 600.0f);

        // The waterline: the moving surface at the camera (terrain water), so the view can be half in. Near
        // it the medium runs above the still surface too (a crest over the lens); just out of it, the lens
        // stays wet a moment.
        float surface = 0.0f, slopeX = 0.0f, slopeY = 0.0f, still = 0.0f, reach = 0.0f;
        uint32_t lineType = 0;
        const CGWorldFrame* frame = CGWorldFrame::s_currentWorldFrame;
        const bool haveCamera = frame && frame->m_camera;
        const C3Vector camera = haveCamera ? frame->m_camera->m_position : C3Vector();
        const bool usable = s_render && MediumOn().Get() && SXC::Water::Active();
        const bool line = usable && WaterlineOn().Get() && haveCamera && CameraWater(surface, slopeX, slopeY, lineType, &still, &reach);
        const bool cameraUnder = line ? camera.z < surface : under;

        if (s_cameraWasUnder && !cameraUnder) {
            s_wet = 1.0f;
        }

        s_cameraWasUnder = cameraUnder;
        s_wet = dt > 0.0f && dt < 1.0f ? s_wet * std::exp(-dt / 1.5f) : 0.0f;
        // The band where the waterline can cross the view: the still surface, give or take how far the waves
        // there have lately reached, and a margin.
        const bool band = line && std::fabs(camera.z - still) < reach + 0.5f;
        // (A debug view keeps it on near the water: it went blank whenever the camera left the band.)
        s_active = usable && (under || band || s_wet > 0.02f || (s_debug != 0 && line));

        if (!s_active) {
            return;
        }

        if (!under && liquidType == 0) {
            liquidType = lineType;
        }

        DayNight::DNInfo& info = *DayNight::GetInfo();
        s_depth = s_preview ? s_previewDepth : (under ? std::max(depth, 0.0f) : 0.0f);
        UpdateKeys(liquidType);
        const float stockEnd = under && info.m_fog.m_end > 1.0f ? info.m_fog.m_end : (s_stockEnd > 1.0f ? s_stockEnd : 100.0f);
        const MediumLook target = At(s_keys, s_depth, stockEnd);

        // Smoothed, so moving between places (another curve) or diving fast fades over a second or so; the
        // modes switch at once (a smoothing step never reaches halfway); coming into the water takes it all
        // at once.
        if (!s_wasActive || dt <= 0.0f || dt > 1.0f) {
            s_now = target;
        } else {
            s_now = Blend(s_now, target, 1.0f - std::exp(-dt / 0.6f), target);
        }

        const MediumLook& k = s_now;

        // The glow: the stock underwater fog's colour (this frame's, before we touch the fog), its brightness
        // with our hue, or ours. Above the still surface (the stock fog is the air's) the last one under water.
        if (under) {
            s_stock[0] = info.m_fog.m_color.r / 255.0f;
            s_stock[1] = info.m_fog.m_color.g / 255.0f;
            s_stock[2] = info.m_fog.m_color.b / 255.0f;
            s_stockEnd = info.m_fog.m_end;
        } else if (s_stockEnd <= 0.0f) {
            // Never under yet (straight into the waterline's band): the light data's deep ocean colour, darker.
            const CImVector& deep = info.m_bands.m_waterColors[3];
            s_stock[0] = deep.r / 255.0f * 0.6f;
            s_stock[1] = deep.g / 255.0f * 0.6f;
            s_stock[2] = deep.b / 255.0f * 0.6f;
        }

        const float* stock = s_stock;
        float glow[3] = { stock[0], stock[1], stock[2] };

        if (k.mode == ColourMode::Replace) {
            for (int c = 0; c < 3; c++) {
                glow[c] = k.colour[c];
            }
        } else if (k.mode == ColourMode::Hue) {
            const float stockLuma = 0.299f * stock[0] + 0.587f * stock[1] + 0.114f * stock[2];
            const float ourLuma = 0.299f * k.colour[0] + 0.587f * k.colour[1] + 0.114f * k.colour[2];
            const float scale = ourLuma > 0.001f ? stockLuma / ourLuma : 0.0f;

            for (int c = 0; c < 3; c++) {
                glow[c] = k.colour[c] * scale;
            }
        }

        const float fadeInto = std::max(std::clamp(k.fadeInto, 0.0f, 1.0f), MinGlow().Get());
        s_visibility = std::max(k.visibility, 1.0f);

        for (int c = 0; c < 3; c++) {
            s_frame.glow[c] = glow[c] * k.brightness * fadeInto;
            s_frame.loss[c] = k.rate[c];
        }

        s_frame.desaturate = k.greyRate > 1e-6f ? 1.0f / k.greyRate : 0.0f;
        s_frame.fade = 3.0f / s_visibility;   // 95% at the visibility
        s_frame.debug = s_debug;
        s_frame.halo = std::max(k.lampHalo, 0.0f) * 0.05f;
        s_frame.haloRange = std::clamp(s_visibility, 24.0f, 128.0f);
        s_frame.haloGlow = 0.5f;

        // The waterline across the view: the surface relative to the camera, its slopes; out of the band the
        // surface is far below (all dry, the lens wet), or, under water without it, everywhere above.
        if (line) {
            s_frame.line[0] = band || !cameraUnder ? surface - camera.z : 1000.0f;
            s_frame.line[1] = slopeX;
            s_frame.line[2] = slopeY;
            s_frame.line[3] = 1.0f;
        } else {
            s_frame.line[0] = under ? 1000.0f : -1000.0f;
            s_frame.line[1] = s_frame.line[2] = 0.0f;
            s_frame.line[3] = 1.0f;
        }

        s_frame.still = still;
        s_frame.lens[0] = s_seconds;
        s_frame.lens[1] = s_wet;

        // Above the still surface the stock fog and light are the air's: left as they are.
        if (!under) {
            return;
        }

        // The stock fog out past where the medium has hidden everything (models' and particles' own fog, and
        // how far the world draws).
        info.m_fog.m_end = std::max(s_visibility * 1.25f, 10.0f);
        info.m_fog.m_start = info.m_fog.m_end * 0.9f;

        // The light from above (instead of the stock darkening): the sun by it, the ambient no lower than the
        // dark floor.
        float tint[3] = { k.lightColour[0], k.lightColour[1], k.lightColour[2] };

        // By depth: what's left of each channel after the way down, scaled so the brightest stays 1.
        if (k.lightByDepth) {
            float left[3], most = 0.0f;

            for (int c = 0; c < 3; c++) {
                left[c] = std::exp2(-s_depth * k.rate[c] / std::max(k.lightLossScale, 1.0f));
                most = std::max(most, left[c]);
            }

            for (int c = 0; c < 3; c++) {
                tint[c] *= most > 1e-6f ? left[c] / most : 0.0f;
            }
        }

        s_lightTint[0] = tint[0];
        s_lightTint[1] = tint[1];
        s_lightTint[2] = tint[2];
        float sun[3], ambient[3];

        for (int c = 0; c < 3; c++) {
            sun[c] = std::clamp(k.light, 0.0f, 1.0f) * tint[c];
            ambient[c] = std::max(sun[c], std::clamp(k.darkFloor, 0.0f, 1.0f));
        }

        Scale(info.m_light1.m_diffuse, sun);
        Scale(info.m_light1.m_ambient, ambient);
    }

    bool WaterlineHoldsAir(float cameraZ) {
        if (!s_render || !MediumOn().Get() || !WaterlineOn().Get() || !SXC::Water::Active()) {
            return false;
        }

        float surface = 0.0f, slopeX = 0.0f, slopeY = 0.0f, still = 0.0f, reach = 0.0f;
        uint32_t type = 0;
        return CameraWater(surface, slopeX, slopeY, type, &still, &reach) && still - cameraZ < reach + 0.5f;
    }

    bool MediumGlow(float out[4]) {
        out[0] = s_frame.glow[0];
        out[1] = s_frame.glow[1];
        out[2] = s_frame.glow[2];
        out[3] = s_active ? 1.0f : 0.0f;
        return s_active;
    }

    void MediumComposite() {
        const CGWorldFrame* frame = CGWorldFrame::s_currentWorldFrame;

        if (!s_active || !s_render || !frame || !frame->m_camera) {
            return;
        }

        const C3Vector& camera = frame->m_camera->m_position;
        s_frame.origin[0] = camera.x;
        s_frame.origin[1] = camera.y;
        s_frame.origin[2] = camera.z;
        s_render(s_frame);
    }

    void DrawMediumPanel(void*) {
        bool on = MediumOn().Get();

        if (ImGui::Checkbox("The underwater medium", &on)) {
            MediumOn().Set(on);
        }

        ImGui::TextDisabled("device: %s", s_status ? s_status() : "not available (D3D9)");
        bool waterline = WaterlineOn().Get();

        if (ImGui::Checkbox("The waterline", &waterline)) {
            WaterlineOn().Set(waterline);
        }

        ImGui::SetItemTooltip("%s", "The camera half in the water (terrain water; needs riding the waves on: it asks the surface at the camera).");
        ImGui::SameLine();
        ImGui::TextDisabled("lens wet %.2f", s_wet);
        float minGlow = MinGlow().Get();

        if (ImGui::SliderFloat("Minimum underwater glow", &minGlow, 0.0f, 1.0f)) {
            MinGlow().Set(minGlow);
        }

        ImGui::SetItemTooltip("%s", "Accessibility: far things under water fade into at least this much of the water's glow, never into full darkness.");
        ImGui::Separator();

        if (!s_active) {
            ImGui::TextWrapped("Not under our water now (the camera in a water material 1 or 3, our water on).");
        } else {
            ImGui::Text("Depth %.1f yd, curve from %s", s_depth, s_keysFrom.c_str());
            const MediumLook& k = s_now;
            auto yd = [](float rate) { return rate > 1e-6f ? 1.0f / rate : 0.0f; };
            ImGui::Text("Visibility %.0f yd%s, colour halved at %.0f / %.0f / %.0f yd (r / g / b; 0 never)", s_visibility, k.stockVisibility ? " (stock)" : "",
                        yd(k.rate[0]), yd(k.rate[1]), yd(k.rate[2]));
            ImGui::Text("Grey at %.0f yd (0 never), fade into %.2f, light from above %.2f, dark floor %.2f", yd(k.greyRate), k.fadeInto, k.light, k.darkFloor);
            ImGui::Text("Lamps in the water x%.2f, gathered out to %.0f yd", k.lampHalo, s_frame.haloRange);
            ImGui::ColorButton("glow", ImVec4(s_frame.glow[0], s_frame.glow[1], s_frame.glow[2], 1.0f));
            ImGui::SameLine();
            ImGui::TextUnformatted("the glow far things fade into");
            ImGui::SameLine();
            ImGui::ColorButton("light", ImVec4(s_lightTint[0], s_lightTint[1], s_lightTint[2], 1.0f));
            ImGui::SameLine();
            ImGui::Text("the light's tint (%s)", k.lightByDepth ? "by depth" : "colour");
        }

        ImGui::Checkbox("Preview a depth", &s_preview);
        ImGui::SetItemTooltip("%s", "Look at the curve at another depth without diving there (still needs the camera under water).");

        if (s_preview) {
            ImGui::SliderFloat("Depth (yd)", &s_previewDepth, 0.0f, 2000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        }

        ImGui::Combo("Medium debug", &s_debug, "Off\0The fade (white: all glow)\0Distance (a band every 10 yd)\0Our lamps in the water alone\0The waterline (red under, green above, blue by height)\0");
        ImGui::SetItemTooltip("%s", "The waterline: red and green where a water surface was drawn along the pixel's ray (the surface's own side), "
                                    "blue where none was and the surface's height decided (bright: under), grey without a waterline.");

        // The curve's keyframes, as bands by depth.
        if (ImGui::CollapsingHeader("The curve here", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("Rows in UnderwaterMedium (the Data view); one per depth, the most specific Scope wins.");

            if (ImGui::BeginTable("keys", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Depth");
                ImGui::TableSetupColumn("Visibility");
                ImGui::TableSetupColumn("Halved r/g/b");
                ImGui::TableSetupColumn("Grey");
                ImGui::TableSetupColumn("Fade into");
                ImGui::TableSetupColumn("Light");
                ImGui::TableSetupColumn("Floor");
                ImGui::TableHeadersRow();

                for (const Key& k : s_keys) {
                    const bool here = s_active && std::fabs(k.depth - s_depth) < 1e-3f;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text(here ? "> %.0f" : "%.0f", k.depth);
                    ImGui::TableNextColumn();
                    k.visibility > 0.0f ? ImGui::Text("%.0f", k.visibility) : ImGui::TextDisabled("stock");
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f / %.0f / %.0f", k.loss[0], k.loss[1], k.loss[2]);
                    ImGui::TableNextColumn();
                    k.desaturate > 0.0f ? ImGui::Text("%.0f", k.desaturate) : ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", k.fadeInto);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", k.light);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", k.darkFloor);
                }

                ImGui::EndTable();
            }
        }
    }
}
