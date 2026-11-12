#include "SXC/Ui/RayTool.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "SXC/Api/sxc_world.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "SXC/World/World.h"
#include "WoW/Source/WorldClient/World.h"

namespace SXC::WorldApi {
    const sxc_world_api* Api();
}

namespace {
    constexpr float kReach = 3000.0f;            // how far the ray looks
    constexpr uint32_t kSolid = 0x100171;        // the camera's collision mask: terrain, WMOs, doodads with collision
    constexpr uint32_t kWater = 0x120171;        // the same, stopping at the water's surface
    constexpr float kTile = 533.33333f;          // an ADT tile, yd
    constexpr float kChunk = kTile / 16.0f;
    constexpr size_t kMaxPins = 8;

    struct Pick {
        bool valid = false;
        float pos[3] = {};
        float fromCamera = 0.0f;   // along the ray
        std::string what;          // what was hit
        bool water = false;        // the water's surface came first
        float waterDepth = 0.0f;   // from that surface down the ray to the ground behind it
    };

    struct Pin {
        Pick pick;
        int number = 0;
    };

    bool g_live = true;        // read what lies under the mouse every frame
    bool g_armed = false;      // the next left click in the world pins a spot
    bool g_keepArmed = false;
    float g_ring = 0.0f;       // a ring around the character at this radius (0: none)
    Pick g_hover;
    std::vector<Pin> g_pins;
    int g_nextNumber = 1;

    float Length(float x, float y, float z) {
        return std::sqrt(x * x + y * y + z * z);
    }

    bool Player(float out[3]) {
        const sxc_world_api* api = SXC::WorldApi::Api();
        sxc_world_snapshot s{};
        s.struct_size = sizeof(s);
        api->snapshot(&s);

        if (!s.in_world) {
            return false;
        }

        out[0] = s.player_position.x, out[1] = s.player_position.y, out[2] = s.player_position.z;
        return true;
    }

    std::string Describe(const World::HitTestDebug& debug, const float pos[3]) {
        char text[256];

        if (debug.m_numModels > 0 && debug.m_models[0].m_name) {
            const World::HitTestDebug::Model& m = debug.m_models[0];

            if (debug.m_type == 4) {
                std::snprintf(text, sizeof(text), "WMO %s, group %u%s%s", m.m_name, m.m_groupNum, m.m_groupName ? " " : "", m.m_groupName ? m.m_groupName : "");
            } else {
                std::snprintf(text, sizeof(text), "%s %s", debug.m_type == 1 ? "doodad" : "model", m.m_name);
            }

            return text;
        }

        // ADT tiles are named map_X_Y with X from the world's y.
        const int tileX = static_cast<int>(std::floor(32.0f - pos[1] / kTile));
        const int tileY = static_cast<int>(std::floor(32.0f - pos[0] / kTile));
        const int chunkX = static_cast<int>(std::floor((32.0f * kTile - pos[1]) / kChunk)) % 16;
        const int chunkY = static_cast<int>(std::floor((32.0f * kTile - pos[0]) / kChunk)) % 16;
        std::snprintf(text, sizeof(text), "terrain, tile %d_%d, chunk %d,%d", tileX, tileY, chunkX, chunkY);
        return text;
    }

    // What the ray from the camera through a window point meets first.
    Pick Cast(float windowX, float windowY) {
        Pick pick;
        float o[3], d[3];

        if (!SXC::WorldApi::FromWindow(windowX, windowY, o, d)) {
            return pick;
        }

        const C3Vector start(o[0], o[1], o[2]);
        const C3Vector end(o[0] + d[0] * kReach, o[1] + d[1] * kReach, o[2] + d[2] * kReach);
        World::HitTestDebug debug{};
        C3Vector hit;
        float solid = 1.0f;

        if (!World::Intersect(start, end, &hit, &solid, kSolid, &debug)) {
            return pick;
        }

        C3Vector wet;
        float water = 1.0f;
        pick.water = World::Intersect(start, end, &wet, &water, kWater, nullptr) && water < solid - 1e-5f;
        pick.valid = true;
        pick.pos[0] = pick.water ? wet.x : hit.x;
        pick.pos[1] = pick.water ? wet.y : hit.y;
        pick.pos[2] = pick.water ? wet.z : hit.z;
        pick.fromCamera = (pick.water ? water : solid) * kReach;
        pick.waterDepth = pick.water ? (solid - water) * kReach : 0.0f;
        pick.what = pick.water ? "water surface; behind it " + Describe(debug, &hit.x) : Describe(debug, &hit.x);
        return pick;
    }

    void Readout(const Pick& pick, const float player[3], bool hasPlayer) {
        ImGui::TextWrapped("%s", pick.what.c_str());
        ImGui::Text("at %.1f, %.1f, %.1f", pick.pos[0], pick.pos[1], pick.pos[2]);
        ImGui::Text("camera  %.1f yd", pick.fromCamera);

        if (hasPlayer) {
            const float dx = pick.pos[0] - player[0], dy = pick.pos[1] - player[1], dz = pick.pos[2] - player[2];
            ImGui::Text("you     %.1f yd (%.1f flat, %+.1f up)", Length(dx, dy, dz), std::sqrt(dx * dx + dy * dy), dz);
        }

        if (pick.water) {
            ImGui::Text("water to the ground behind: %.1f yd along the ray", pick.waterDepth);
        }
    }

    // A marker over the world: a dot and a label.
    void Mark(ImDrawList* draw, const float pos[3], ImU32 colour, const char* label) {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        float x, y;

        if (!SXC::WorldApi::ToWindow(pos[0], pos[1], pos[2], x, y)) {
            return;
        }

        const ImVec2 at(x * size.x, y * size.y);
        draw->AddCircleFilled(at, 4.0f, colour);
        draw->AddCircle(at, 5.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);

        if (label && *label) {
            draw->AddText(ImVec2(at.x + 8.0f, at.y - 7.0f), IM_COL32(0, 0, 0, 220), label);
            draw->AddText(ImVec2(at.x + 7.0f, at.y - 8.0f), colour, label);
        }
    }

    // A circle on the ground around the character, drawn segment by segment where both ends show.
    void Ring(ImDrawList* draw, const float centre[3], float radius) {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        constexpr int kSegments = 96;
        ImVec2 last;
        bool lastShown = false;

        for (int i = 0; i <= kSegments; i++) {
            const float a = static_cast<float>(i) / kSegments * 6.2831853f;
            const float px = centre[0] + std::cos(a) * radius, py = centre[1] + std::sin(a) * radius;
            float pz = centre[2];
            SXC::WorldApi::GroundHeight(px, py, pz);
            float x, y;
            const bool shown = SXC::WorldApi::ToWindow(px, py, pz + 0.2f, x, y);
            const ImVec2 at(x * size.x, y * size.y);

            if (shown && lastShown) {
                draw->AddLine(last, at, IM_COL32(255, 200, 60, 220), 2.0f);
            }

            last = at;
            lastShown = shown;
        }
    }
}

namespace SXC::RayTool {
    void DrawPanel(void* user) {
        ImGuiIO& io = ImGui::GetIO();
        float player[3] = {};
        const bool hasPlayer = Player(player);
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemActive();
        const ImVec2 mouse = io.MousePos;
        const bool mouseInWindow = mouse.x >= 0.0f && mouse.y >= 0.0f && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f;

        ImGui::Checkbox("Live under the mouse", &g_live);
        ImGui::SetItemTooltip("Read what lies under the mouse every frame (one ray, %.0f yd).", kReach);
        ImGui::SameLine();

        if (ImGui::Button(g_armed ? "Click a spot... (Esc)" : "Pin a spot")) {
            g_armed = !g_armed;
        }

        ImGui::SetItemTooltip("The next left click in the world pins the spot; its distances follow you as you move.");
        ImGui::SameLine();
        ImGui::Checkbox("Keep pinning", &g_keepArmed);

        if (g_armed && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_armed = false;
        }

        const bool overWorld = !overUi && mouseInWindow;
        g_hover = Pick();

        if (overWorld && (g_live || g_armed)) {
            g_hover = Cast(mouse.x / io.DisplaySize.x, mouse.y / io.DisplaySize.y);
        }

        // While armed, the world's left click is ours: the game doesn't target or turn.
        if (g_armed && overWorld) {
            ImGui::SetNextFrameWantCaptureMouse(true);

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && g_hover.valid) {
                if (g_pins.size() >= kMaxPins) {
                    g_pins.erase(g_pins.begin());
                }

                g_pins.push_back(Pin{ g_hover, g_nextNumber++ });
                g_armed = g_keepArmed;
            }
        }

        ImGui::SeparatorText("Under the mouse");

        if (g_hover.valid) {
            Readout(g_hover, player, hasPlayer);
        } else {
            ImGui::TextDisabled(overWorld ? "nothing within %.0f yd" : "(the mouse is over the overlay)", kReach);
        }

        ImGui::SeparatorText("Ring around you");
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("radius (yd)", &g_ring, 0.0f, 500.0f, g_ring > 0.0f ? "%.0f" : "off");
        ImGui::SetItemTooltip("A circle on the ground around the character, to see where a reach ends. Ctrl+click to type a value.");

        ImGui::SeparatorText("Pinned");

        if (g_pins.empty()) {
            ImGui::TextDisabled("none");
        } else if (ImGui::SmallButton("Clear")) {
            g_pins.clear();
        }

        int remove = -1;

        for (size_t i = 0; i < g_pins.size(); i++) {
            const Pin& pin = g_pins[i];
            ImGui::PushID(pin.number);
            const float dx = pin.pick.pos[0] - player[0], dy = pin.pick.pos[1] - player[1], dz = pin.pick.pos[2] - player[2];
            const bool open = ImGui::TreeNode("pin", "#%d  %.1f yd from you (%.1f flat)", pin.number, Length(dx, dy, dz), std::sqrt(dx * dx + dy * dy));
            ImGui::SameLine();

            if (ImGui::SmallButton("x")) {
                remove = static_cast<int>(i);
            }

            if (open) {
                Readout(pin.pick, player, hasPlayer);
                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        if (remove >= 0) {
            g_pins.erase(g_pins.begin() + remove);
        }

        // Over the world.
        ImDrawList* draw = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());

        if (hasPlayer && g_ring > 0.0f) {
            Ring(draw, player, g_ring);
        }

        for (const Pin& pin : g_pins) {
            char label[64];
            const float dx = pin.pick.pos[0] - player[0], dy = pin.pick.pos[1] - player[1], dz = pin.pick.pos[2] - player[2];
            std::snprintf(label, sizeof(label), "#%d %.0f yd", pin.number, hasPlayer ? Length(dx, dy, dz) : pin.pick.fromCamera);
            Mark(draw, pin.pick.pos, IM_COL32(120, 220, 255, 255), label);
        }

        if (g_hover.valid) {
            char label[64];
            std::snprintf(label, sizeof(label), "%.1f yd", g_hover.fromCamera);
            Mark(draw, g_hover.pos, g_armed ? IM_COL32(255, 120, 120, 255) : IM_COL32(255, 255, 255, 230), label);
        }
    }
}
