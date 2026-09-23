#include "ui/node_library.h"

#include "engine/node_registry.h"
#include "ui/node_canvas.h"
#include "ui/theme.h"

#include <imgui.h>

namespace aiwrite::ui {

void draw_node_library(const char* title, bool* open)
{
    if (open != nullptr && !*open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(220.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }

    engine::registerAllNodes();
    engine::NodeRegistry& registry = engine::NodeRegistry::instance();

    ImGui::TextDisabled("点击节点即在画布中心添加");
    ImGui::Separator();

    const engine::NodeCategory categories[] = {
        engine::NodeCategory::Input,  engine::NodeCategory::Process, engine::NodeCategory::Config,
        engine::NodeCategory::Inference, engine::NodeCategory::Output};

    for (engine::NodeCategory category : categories) {
        const std::vector<const engine::Definition*> items = registry.listByCategory(category);
        if (items.empty()) {
            continue;
        }
        if (!ImGui::CollapsingHeader(engine::categoryName(category),
                                     ImGuiTreeNodeFlags_DefaultOpen)) {
            continue;
        }

        for (const engine::Definition* definition : items) {
            ImGui::PushID(definition->type.c_str());

            // 分类色点 + 节点名（点击添加）
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(cursor.x + 4.0f, cursor.y + ImGui::GetTextLineHeight() * 0.5f), 4.0f,
                category_color(category), 12);
            ImGui::Dummy(ImVec2(12.0f, ImGui::GetTextLineHeight()));
            ImGui::SameLine(0.0f, 2.0f);

            if (ImGui::Selectable(definition->display_name.c_str())) {
                canvas_add_node_at_center(definition->type);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(definition->description.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("类型 %s", definition->type.c_str());
                for (const engine::Port& port : definition->inputs) {
                    ImGui::BulletText("输入 %s（%s）", port.display_name.c_str(),
                                      engine::portTypeName(port.type));
                }
                for (const engine::Port& port : definition->outputs) {
                    ImGui::BulletText("输出 %s（%s）", port.display_name.c_str(),
                                      engine::portTypeName(port.type));
                }
                for (const engine::Param& param : definition->params) {
                    ImGui::BulletText("参数 %s（%s）", param.display_name.c_str(),
                                      engine::paramTypeName(param.type));
                }
                ImGui::EndTooltip();
            }

            ImGui::PopID();
        }
        ImGui::Spacing();
    }

    ImGui::End();
}

} // namespace aiwrite::ui
