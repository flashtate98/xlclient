// SXC's Geosets panel (SXC/Models/Geosets.h).

#include "SXC/Models/Geosets.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
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

    void DrawBatchesPanel(void* user) {
        ImGui::RadioButton("Player", &s_fromTarget, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Target", &s_fromTarget, 1);

        CM2Model* model = Selected();

        if (!Ready(model) || !model->m_shared->m_data) {
            ImGui::TextDisabled(s_fromTarget ? "No target, or its model isn't loaded." : "No player model loaded.");
            return;
        }

        const M2Data* data = model->m_shared->m_data;
        const M2SkinProfile* skin = model->m_shared->m_skinData;
        ImGui::TextWrapped("%s: %u batches", model->m_shared->m_fileName, skin->m_batches.m_count);
        // The table as text, for pasting (filled while the rows are drawn, copied at the end).
        const bool copy = ImGui::SmallButton("Copy table");
        ImGui::SetItemTooltip("Copy the table as text (tab separated) to the clipboard.");
        std::string text = std::string(model->m_shared->m_fileName) + "\n#\tgeoset\tshown\tshader\tblend\teffect\tstages\n";

        if (!ImGui::BeginTable("##batches", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            return;
        }

        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("geoset");
        ImGui::TableSetupColumn("shown");
        ImGui::TableSetupColumn("shader");
        ImGui::TableSetupColumn("blend");
        ImGui::TableSetupColumn("effect");
        ImGui::TableSetupColumn("stages: type texture (transform)");
        ImGui::TableHeadersRow();

        static const char* kBlends[] = { "opaque", "alpha key", "alpha", "no alpha add", "add", "mod", "mod2x", "blend add" };

        for (uint32_t i = 0; i < skin->m_batches.m_count; i++) {
            const M2Batch& batch = skin->m_batches[i];
            const M2SkinSection& section = skin->m_skinSections[batch.m_skinSectionIndex];
            const M2Material* material = batch.m_materialIndex < data->m_materials.m_count ? &data->m_materials[batch.m_materialIndex] : nullptr;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", i);
            ImGui::TableNextColumn();
            const uint32_t geoset = M2SkinSectionGeosetId(*skin, section);
            ImGui::Text("%u", geoset);
            ImGui::TableNextColumn();
            // The batch's skin section shown or hidden directly (for telling batches of one geoset apart;
            // the section may carry other batches too, and the client's next geoset update resets it).
            bool shown = model->m_skinSections[batch.m_skinSectionIndex] != 0;

            if (ImGui::Checkbox("##shown", &shown)) {
                model->m_skinSections[batch.m_skinSectionIndex] = shown ? 1 : 0;
                model->UnoptimizeVisibleGeometry();
                model->OptimizeVisibleGeometry();
            }

            ImGui::SetItemTooltip("Show or hide skin section %u (this batch, and any other batch drawing the same section).", batch.m_skinSectionIndex);
            ImGui::TableNextColumn();
            ImGui::Text("0x%04x", batch.m_shader);
            ImGui::TableNextColumn();
            char blend[48] = "";

            if (material) {
                std::snprintf(blend, sizeof(blend), "%s%s", material->m_blendMode < 8 ? kBlends[material->m_blendMode] : "?", (material->m_flags & 1) ? ", unlit" : "");
            }

            ImGui::TextUnformatted(blend);
            ImGui::TableNextColumn();
            const CShaderEffect* effect = model->m_shared->m_effects ? model->m_shared->m_effects[i] : nullptr;
            const char* effectName = effect && effect->m_key.m_str ? effect->m_key.m_str : "(none)";
            ImGui::TextUnformatted(effectName);
            ImGui::TableNextColumn();
            char row[160];
            std::snprintf(row, sizeof(row), "%u\t%u\t%s\t0x%04x\t%s\t%s\t", i, geoset, shown ? "yes" : "-", batch.m_shader, blend, effectName);
            text += row;

            for (uint32_t k = 0; k < batch.m_textureCount; k++) {
                const uint32_t at = batch.m_textureComboIndex + k;
                const uint32_t combo = at < data->m_textureCombos.m_count ? data->m_textureCombos[at] : 0xFFFF;
                const uint32_t tat = batch.m_textureTransformComboIndex + k;
                const uint32_t transform = tat < data->m_textureTransformCombos.m_count ? data->m_textureTransformCombos[tat] : 0xFFFF;
                const CTexture* texture = combo < data->m_textures.m_count ? model->m_textures[combo] : nullptr;
                const char* name = texture ? texture->m_filename : "(no texture)";
                const char* slash = strrchr(name, '\\');
                // Whether it reached the GPU: without a GxTex the device binds its blank white fallback.
                char gpu[64] = "";

                if (texture) {
                    if (texture->m_gxTex) {
                        std::snprintf(gpu, sizeof(gpu), " [gpu %ux%u fmt %d]", texture->m_gxWidth, texture->m_gxHeight, static_cast<int>(texture->m_gxTexFormat));
                    } else {
                        std::snprintf(gpu, sizeof(gpu), " [NOT ON GPU, flags 0x%x]", texture->m_flags);
                    }
                }

                char stage[400];
                std::snprintf(stage, sizeof(stage), "%u: %u %s (%s)%s", k, combo < data->m_textures.m_count ? data->m_textures[combo].m_type : 0xFFFF,
                              slash ? slash + 1 : name, transform == 0xFFFF ? "-" : std::to_string(transform).c_str(), gpu);
                ImGui::TextUnformatted(stage);
                text += (k ? " | " : "") + std::string(stage);
            }

            text += "\n";
            ImGui::PopID();
        }

        ImGui::EndTable();

        if (copy) {
            ImGui::SetClipboardText(text.c_str());
        }
    }
}
