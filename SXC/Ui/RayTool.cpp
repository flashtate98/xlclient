#include "SXC/Ui/RayTool.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "SXC/Api/sxc_world.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "SXC/World/World.h"
#include "SXC/Wmo/Wmo.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/Types.h"
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
        std::vector<std::string> surface;   // a WMO's surface: triangle, material, shader, textures, colours
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

    // Retail's WMO shaders by id (wowdev's WMO page, MOMT), for the surface readout.
    const char* WmoShaderName(uint32_t shader) {
        static const char* const kNames[] = {
            "Diffuse", "Specular", "Metal", "Env", "Opaque", "EnvMetal", "TwoLayerDiffuse", "TwoLayerEnvMetal",
            "TwoLayerTerrain", "DiffuseEmissive", "waterWindow", "MaskedEnvMetal", "EnvMetalEmissive",
            "TwoLayerDiffuseOpaque", "submarineWindow", "TwoLayerDiffuseEmissive", "DiffuseTerrain",
            "AdditiveMaskedEnvMetal", "TwoLayerDiffuseMod2x", "TwoLayerDiffuseMod2xNA", "TwoLayerDiffuseAlpha", "Lod",
            "Parallax", "UnkDFShader",
        };
        return shader < sizeof(kNames) / sizeof(kNames[0]) ? kNames[shader] : "?";
    }

    void Line(std::vector<std::string>& out, const char* format, ...) {
        char text[512];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        out.emplace_back(text);
    }

    // Whether the segment p + (q - p) t, t in [0, 1], passes through the box (slabs).
    bool SegmentInBox(const float p[3], const float q[3], const CAaBox& box) {
        const float lo[3] = { box.b.x, box.b.y, box.b.z }, hi[3] = { box.t.x, box.t.y, box.t.z };
        float t0 = 0.0f, t1 = 1.0f;

        for (int a = 0; a < 3; a++) {
            const float d = q[a] - p[a];

            if (std::fabs(d) < 1e-9f) {
                if (p[a] < lo[a] || p[a] > hi[a]) return false;
                continue;
            }

            float ta = (lo[a] - p[a]) / d, tb = (hi[a] - p[a]) / d;
            if (ta > tb) std::swap(ta, tb);
            t0 = ta > t0 ? ta : t0;
            t1 = tb < t1 ? tb : t1;
            if (t0 > t1) return false;
        }

        return true;
    }

    // Where the segment meets the triangle (both sides), as t along it; Moeller-Trumbore.
    bool SegmentTriangle(const float p[3], const float d[3], const C3Vector& a, const C3Vector& b, const C3Vector& c, float& t) {
        const float e1[3] = { b.x - a.x, b.y - a.y, b.z - a.z }, e2[3] = { c.x - a.x, c.y - a.y, c.z - a.z };
        const float h[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
        const float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];

        if (std::fabs(det) < 1e-12f) return false;

        const float inv = 1.0f / det;
        const float s[3] = { p[0] - a.x, p[1] - a.y, p[2] - a.z };
        const float u = inv * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]);
        if (u < 0.0f || u > 1.0f) return false;

        const float qv[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
        const float v = inv * (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]);
        if (v < 0.0f || u + v > 1.0f) return false;

        t = inv * (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]);
        return t >= 0.0f && t <= 1.0f;
    }

    bool g_drawnFaces = true;   // read the WMO's drawn triangles, not the collision ones (material 255: collision only)

    // The WMO triangle the ray meets, and what draws it. The camera's mask meets collidable faces only, often a
    // collision-only hull (material 255) in front of what's drawn: with g_drawnFaces the WMO it found is then
    // searched along the same ray for the nearest drawn triangle (loaded groups, in the WMO's own space).
    // `drawnAt`: where along the ray that triangle lies (0..1), when one was found.
    std::vector<std::string> DescribeSurface(const C3Vector& start, const C3Vector& end, float& drawnAt) {
        std::vector<std::string> out;
        C3Vector s = start, e = end;
        float t = 1.0f;
        uint16_t hitFace = 0xFFFF;
        CMapObj* mapObj = nullptr;
        CMapObjDef* def = nullptr;
        CMapObjDefGroup* defGroup = nullptr;
        drawnAt = -1.0f;

        if (!CMap::VectorIntersect(&s, &e, kSolid, 0x100, &t, &hitFace, &mapObj, &def, &defGroup) || !mapObj || !defGroup || hitFace == 0xFFFF) {
            return out;
        }

        uint32_t groupNum = defGroup->m_groupNum;
        uint32_t face = hitFace;
        bool drawn = false;

        if (g_drawnFaces && def && mapObj->m_header) {
            const C3Vector ls = start * def->m_invMat, le = end * def->m_invMat;
            const float p[3] = { ls.x, ls.y, ls.z }, q[3] = { le.x, le.y, le.z };
            const float d[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
            float best = 2.0f;
            const uint32_t groups = mapObj->m_header->m_nGroups < 512 ? mapObj->m_header->m_nGroups : 512;

            for (uint32_t g = 0; g < groups; g++) {
                const CMapObjGroup* group = mapObj->m_mapObjGroupArray[g];

                if (!group || !group->m_polyList || !group->m_indices || !group->m_vertexList || !SegmentInBox(p, q, group->m_bbox)) {
                    continue;
                }

                const uint32_t faces = static_cast<uint32_t>(group->m_polyListSize);

                for (uint32_t f = 0; f < faces && 3 * f + 2 < static_cast<uint32_t>(group->m_indicesCount); f++) {
                    if (group->m_polyList[f].m_materialId == 0xFF) continue;

                    const uint16_t* i = &group->m_indices[3 * f];
                    const uint32_t n = static_cast<uint32_t>(group->m_vertexListCount);
                    if (i[0] >= n || i[1] >= n || i[2] >= n) continue;

                    float ft;
                    if (SegmentTriangle(p, d, group->m_vertexList[i[0]], group->m_vertexList[i[1]], group->m_vertexList[i[2]], ft) && ft < best) {
                        best = ft, groupNum = g, face = f, drawn = true;
                    }
                }
            }

            if (drawn) drawnAt = best;
        }

        CMapObjGroup* group = groupNum < 512 ? mapObj->m_mapObjGroupArray[groupNum] : nullptr;

        if (!group || !group->m_polyList || face >= static_cast<uint32_t>(group->m_polyListSize)) {
            return out;
        }

        const uint32_t headerFlags = mapObj->m_header ? mapObj->m_header->m_flags : 0;
        const bool unified = (headerFlags & 0x2) != 0;
        Line(out, "%s", mapObj->m_wmoName);
        Line(out, "root flags 0x%02x (%s)", headerFlags, unified ? "unified" : "classic");
        Line(out, "group %u %s, flags 0x%08x (%s)", groupNum, group->m_groupName ? group->m_groupName : "",
            static_cast<uint32_t>(group->m_flags), (group->m_flags & 0x2000) ? "interior" : (group->m_flags & 0x8) ? "exterior" : "neither");

        const SMOPoly& poly = group->m_polyList[face];
        Line(out, "triangle %u (%s), MOPY flags 0x%02x, material %u", face,
            drawn ? "the nearest drawn one" : g_drawnFaces ? "collision; no drawn one on the ray" : "collision", poly.m_flags, poly.m_materialId);

        const uint32_t index = static_cast<uint32_t>(face) * 3;

        for (int32_t i = 0; i < group->m_batchListCount; i++) {
            const SMOBatch& batch = group->m_batchList[i];

            if (index >= batch.m_indexStart && index < batch.m_indexStart + batch.m_indexCount) {
                const char* kind = i < group->m_transparencyBatchesCount ? "transition"
                    : i < group->m_transparencyBatchesCount + group->m_intBatchCount ? "interior" : "exterior";
                Line(out, "batch %d (%s), vertices %u-%u", i, kind, batch.m_vertexStart, batch.m_vertexEnd);
                break;
            }
        }

        if (poly.m_materialId < mapObj->m_materialsCount) {
            SMOMaterial& material = mapObj->m_materialList[poly.m_materialId];
            const MapObjCata::ShaderNote note = MapObjCata::Note(material);
            const uint32_t fileShader = note.known ? note.retail : material.m_shader;
            Line(out, "shader in the file: %u %s", fileShader, WmoShaderName(fileShader));

            const uint32_t ours = MapObjCata::OurShaderId(material, material.m_shader);

            if (unified && SXC::Wmo::OurShader() && SXC::Wmo::Covers(ours)) {
                Line(out, "drawn with: our WMO shader, id %u %s", ours, WmoShaderName(ours));
            } else if (unified && MapObjCata::Drawn(material.m_shader)) {
                Line(out, "drawn with: Cataclysm's effect %u (MapObjU%s)", material.m_shader, WmoShaderName(material.m_shader));
            } else {
                Line(out, "drawn with: 3.3.5's %s effect %u %s", unified ? "unified" : "classic", material.m_shader, WmoShaderName(material.m_shader));
            }

            if (note.reason && ours == material.m_shader) {
                Line(out, "  why: %s", note.reason);
            }

            Line(out, "blend mode %u, flags 0x%x, flags2 0x%x", material.m_blendMode, material.m_flags, material.m_flags2);
            const uint32_t offsets[3] = { material.m_texture1, material.m_texture2, material.m_texture3 };

            for (uint32_t k = 0; k < 3; k++) {
                if (mapObj->m_textureNameList && offsets[k] < static_cast<uint32_t>(mapObj->m_texturesSize) && mapObj->m_textureNameList[offsets[k]]) {
                    Line(out, "texture %u: %s", k + 1, &mapObj->m_textureNameList[offsets[k]]);
                }
            }
        }

        // The corners: vertex colour (MOCV, as the client fixed it), the blend set's alpha, the UV sets.
        for (uint32_t k = 0; k < 3; k++) {
            const uint16_t v = group->m_indices[index + k];
            char colour[48] = "-", blend[16] = "-", uv2[32] = "";

            if (group->m_colorVertexList && v < group->m_colorVertexListSize) {
                const CImVector& c = group->m_colorVertexList[v];
                std::snprintf(colour, sizeof(colour), "%u,%u,%u a%u", c.r, c.g, c.b, c.a);
            }

            if (group->m_colorVertexListExtra && v < group->m_colorVertexListExtraSize) {
                std::snprintf(blend, sizeof(blend), "%u", group->m_colorVertexListExtra[v].a);
            }

            if (group->m_textureVertexList2 && v < group->m_textureVertexList2Count) {
                std::snprintf(uv2, sizeof(uv2), " / %.2f,%.2f", group->m_textureVertexList2[v].x, group->m_textureVertexList2[v].y);
            }

            const C2Vector uv = v < group->m_textureVertexListCount ? group->m_textureVertexList[v] : C2Vector(0.0f, 0.0f);
            Line(out, "corner %u (vertex %u): MOCV %s, blend alpha %s, uv %.2f,%.2f%s", k, v, colour, blend, uv.x, uv.y, uv2);
        }

        return out;
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

        if (!pick.water && debug.m_type == 4) {
            float drawnAt;
            pick.surface = DescribeSurface(start, end, drawnAt);

            // The marker and the distances on the drawn surface the readout describes.
            if (drawnAt >= 0.0f) {
                pick.pos[0] = start.x + (end.x - start.x) * drawnAt;
                pick.pos[1] = start.y + (end.y - start.y) * drawnAt;
                pick.pos[2] = start.z + (end.z - start.z) * drawnAt;
                pick.fromCamera = drawnAt * kReach;
            }
        }

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

        if (!pick.surface.empty()) {
            ImGui::PushID(&pick);
            ImGui::SeparatorText("WMO surface");
            ImGui::SameLine();

            if (ImGui::SmallButton("Copy")) {
                std::string text;
                for (const std::string& line : pick.surface) text += line + "\n";
                ImGui::SetClipboardText(text.c_str());
            }

            ImGui::SetItemTooltip("The surface readout as text, to paste into a bug report or a chat.");

            for (const std::string& line : pick.surface) {
                ImGui::TextUnformatted(line.c_str());
            }

            ImGui::PopID();
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
        ImGui::Checkbox("WMOs: drawn triangles", &g_drawnFaces);
        ImGui::SetItemTooltip("On a WMO, read the nearest drawn triangle along the ray instead of the collision face the ray "
            "stops at (often an invisible collision hull, material 255). Off: the collision face, as before.");

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
