// SXC's Geosets panel (SXC/Models/Geosets.h).

#include "SXC/Models/Geosets.h"

#include <cstdio>
#include <map>
#include <string>
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "SXC/ThirdParty/imgui/imgui.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"

namespace {
    // Forced states by geoset id, per model file (lower case, as the client names it).
    std::map<std::string, std::map<uint32_t, bool>> s_overrides;
    int s_fromTarget;
    bool s_onlyVariants;

    // The groups the client's character code drives (CCharacterComponent::GeosRenderPrep), and the later
    // ones by wowdev's character geoset list [believed, not checked against retail's client].
    const char* GroupName(uint32_t group) {
        switch (group) {
            case 0: return "skin, hair";
            case 1: return "facial 1";
            case 2: return "facial 2";
            case 3: return "facial 3";
            case 4: return "gloves";
            case 5: return "boots";
            case 7: return "ears";
            case 8: return "sleeves";
            case 9: return "kneepads";
            case 10: return "shirt, chest";
            case 11: return "pants";
            case 12: return "tabard";
            case 13: return "robe, trousers";
            case 15: return "cape";
            case 17: return "eyes";
            case 18: return "belt";
            case 19: return "tail";
            case 20: return "feet";
            case 22: return "torso";
            case 23: return "hand attachment";
            case 24: return "head attachment";
            case 25: return "blindfold";
            default: return "";
        }
    }

    bool Ready(const CM2Model* model) {
        return model && (model->m_fFlags & 1) && model->m_shared && model->m_shared->m_skinData && model->m_skinSections;
    }

    // Sets the sections of `id` to `visible` where they differ; whether any changed.
    bool Force(CM2Model* model, uint32_t id, bool visible) {
        const M2SkinProfile* skin = model->m_shared->m_skinData;

        for (uint32_t i = 0; i < skin->m_skinSections.m_count; i++) {
            if (M2SkinSectionGeosetId(*skin, skin->m_skinSections[i]) == id && (model->m_skinSections[i] != 0) != visible) {
                model->SetGeometryVisible(id, id, visible ? 1 : 0);
                return true;
            }
        }

        return false;
    }

    bool Apply(CM2Model* model) {
        const auto found = s_overrides.find(model->m_shared->m_fileName);

        if (found == s_overrides.end()) {
            return false;
        }

        bool changed = false;

        for (const auto& [id, visible] : found->second) {
            changed |= Force(model, id, visible);
        }

        return changed;
    }

    CM2Model* Selected() {
        CGObject_C* object = nullptr;

        if (s_fromTarget) {
            if (CGGameUI::m_lockedTarget) {
                object = ClntObjMgrObjectPtr(CGGameUI::m_lockedTarget, TYPE_UNIT, __FILE__, __LINE__);
            }
        } else {
            object = ClntObjMgrGetActivePlayerObj();
        }

        return object ? object->GetObjectModel() : nullptr;
    }

    struct Geoset {
        uint32_t sections = 0, vertices = 0, triangles = 0;
        bool shown = false;
    };
}

namespace SXC::Geosets {
    void ApplyOverrides(CM2Model* model) {
        if (Ready(model)) {
            Apply(model);
        }
    }

    void DrawPanel(void* user) {
        ImGui::RadioButton("Player", &s_fromTarget, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Target", &s_fromTarget, 1);
        ImGui::SameLine();
        ImGui::Checkbox("Only groups with variants", &s_onlyVariants);
        ImGui::SetItemTooltip("Leave out groups with a single geoset.");

        CM2Model* model = Selected();

        if (!Ready(model)) {
            ImGui::TextDisabled(s_fromTarget ? "No target, or its model isn't loaded." : "No player model loaded.");
            return;
        }

        // Kept applied while the panel is open (a creature's geosets are set once, not on every change).
        if (Apply(model)) {
            model->OptimizeVisibleGeometry();
        }

        const std::string file = model->m_shared->m_fileName;
        auto& overrides = s_overrides[file];
        ImGui::TextWrapped("%s", file.c_str());

        const M2SkinProfile* skin = model->m_shared->m_skinData;
        std::map<uint32_t, std::map<uint32_t, Geoset>> groups;

        for (uint32_t i = 0; i < skin->m_skinSections.m_count; i++) {
            const M2SkinSection& section = skin->m_skinSections[i];
            const uint32_t id = M2SkinSectionGeosetId(*skin, section);
            Geoset& geoset = groups[id / 100][id];
            geoset.sections++;
            geoset.vertices += section.m_vertexCount;
            geoset.triangles += section.m_indexCount / 3;
            geoset.shown |= model->m_skinSections[i] != 0;
        }

        ImGui::Text("%u sections, %zu groups, %zu overridden", skin->m_skinSections.m_count, groups.size(), overrides.size());
        ImGui::SameLine();

        if (ImGui::SmallButton("Clear overrides")) {
            overrides.clear();
            // The client's own choice back: a character re-sets its geosets on the next change; until then
            // (and for creatures) what was forced stays.
        }

        ImGui::SetItemTooltip("Forget this model's overrides. A character takes the client's choice back at its next geoset update (change a piece of gear); a creature keeps the forced state until it reloads.");

        bool changed = false;

        for (const auto& [group, geosets] : groups) {
            if (s_onlyVariants && geosets.size() < 2) {
                continue;
            }

            uint32_t shown = 0;

            for (const auto& [id, geoset] : geosets) {
                shown += geoset.shown ? 1 : 0;
            }

            char label[96];
            std::snprintf(label, sizeof(label), "%u  %s  (%u of %zu shown)###g%u", group, GroupName(group), shown, geosets.size(), group);

            if (!ImGui::TreeNodeEx(label, group >= 20 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                continue;
            }

            if (ImGui::BeginTable("##geosets", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                for (const auto& [id, geoset] : geosets) {
                    ImGui::PushID(static_cast<int>(id));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    bool visible = geoset.shown;
                    char name[16];
                    std::snprintf(name, sizeof(name), "%u", id);

                    if (ImGui::Checkbox(name, &visible)) {
                        overrides[id] = visible;
                        changed |= Force(model, id, visible);
                    }

                    ImGui::TableNextColumn();
                    const auto forced = overrides.find(id);

                    if (forced != overrides.end()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), forced->second ? "forced on" : "forced off");
                        ImGui::SameLine();

                        if (ImGui::SmallButton("x")) {
                            overrides.erase(forced);
                        }

                        ImGui::SetItemTooltip("Drop this override (the client's choice returns at the next geoset update).");
                    } else {
                        ImGui::TextDisabled("client");
                    }

                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%u tris, %u verts", geoset.triangles, geoset.vertices);
                    ImGui::TableNextColumn();

                    if (geoset.sections > 1) {
                        ImGui::TextDisabled("%u sections", geoset.sections);
                    }

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            ImGui::TreePop();
        }

        if (changed) {
            model->OptimizeVisibleGeometry();
        }

        if (overrides.empty()) {
            s_overrides.erase(file);
        }
    }
}
