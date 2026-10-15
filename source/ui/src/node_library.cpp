#include "node_library.h"

#include "node_registry.h"
#include "node_canvas.h"
#include "theme.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace aiwrite::ui {
namespace {

// M_node_expansion L3-01：大小写不敏感子串匹配（ASCII 折叠；中文原样字节比较）
std::size_t find_ci(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) {
        return 0;
    }
    if (needle.size() > haystack.size()) {
        return std::string::npos;
    }
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lower(haystack[i + j]) != lower(needle[j])) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return i;
        }
    }
    return std::string::npos;
}

// 渲染单个节点项：分类色点 + 名称（搜索时在匹配段上画高亮底色）
bool draw_library_item(const engine::Definition* definition, engine::NodeCategory category,
                       const std::string& query)
{
    ImGui::PushID(definition->type.c_str());

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(cursor.x + 4.0f, cursor.y + ImGui::GetTextLineHeight() * 0.5f), 4.0f,
        category_color(category), 12);
    ImGui::Dummy(ImVec2(12.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, 2.0f);

    const bool clicked = ImGui::Selectable(definition->display_name.c_str());

    // L3-01 高亮匹配：在 Selectable 文本上叠一层半透明底色（覆盖匹配段）
    if (!query.empty()) {
        const std::size_t pos = find_ci(definition->display_name, query);
        if (pos != std::string::npos) {
            ImDrawList* dl       = ImGui::GetWindowDrawList();
            const ImVec2 item_min = ImGui::GetItemRectMin();
            const ImVec2 item_max = ImGui::GetItemRectMax();
            const std::string& name = definition->display_name;
            const float x0 = item_min.x + ImGui::CalcTextSize(name.substr(0, pos).c_str()).x;
            const float w  = ImGui::CalcTextSize(name.substr(pos, query.size()).c_str()).x;
            dl->AddRectFilled(ImVec2(x0, item_min.y), ImVec2(x0 + w, item_max.y),
                              IM_COL32(235, 180, 60, 80));
        }
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
    return clicked;
}

} // namespace

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

    // ---- M_node_expansion L3-01：节点搜索框（顶部；跨分类；一键清空）----
    static char search_buf[64] = "";
    ImGui::SetNextItemWidth(-36.0f);
    ImGui::InputText("##node_search", search_buf, sizeof(search_buf));
    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::Button("✕", ImVec2(24.0f, 0.0f))) {
        search_buf[0] = '\0';
    }
    if (ImGui::IsItemHovered() && search_buf[0] != '\0') {
        ImGui::SetTooltip("清空搜索，恢复全部分类列表");
    }
    const std::string query = search_buf;
    const bool        searching = find_ci(search_buf, "") != 0 ? false : (search_buf[0] != '\0');
    ImGui::TextDisabled("点击节点即在画布中心添加");
    ImGui::Separator();

    const engine::NodeCategory categories[] = {
        engine::NodeCategory::Input,  engine::NodeCategory::Process, engine::NodeCategory::Config,
        engine::NodeCategory::Inference, engine::NodeCategory::Output, engine::NodeCategory::Auxiliary};

    // M_node_expansion L2（§4.3.2 伪两层）：顶层「辅助功能」下的子分类视觉分组标签
    struct SubgroupLabel {
        engine::NodeCategory category;
        const char*         label; // 空 = 不显示分组标签
    };
    const SubgroupLabel subgroup_labels[] = {
        {engine::NodeCategory::Auxiliary, "文本处理"}, // TextProcess（子分类 · 仅 UI）
    };

    // ---- L3-01 搜索模式：跨分类平铺全部命中（显示名或 type 含查询）----
    if (searching) {
        bool any = false;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        for (const engine::Definition* definition : registry.listTypes()) {
            if (find_ci(definition->display_name, query) == std::string::npos &&
                find_ci(definition->type, query) == std::string::npos) {
                continue;
            }
            any = true;
            if (draw_library_item(definition, definition->category, query)) {
                canvas_add_node_at_center(definition->type);
            }
        }
        ImGui::PopStyleColor();
        if (!any) {
            ImGui::TextDisabled("（无匹配节点）");
        }
    }
    else {
        for (engine::NodeCategory category : categories) {
            const std::vector<const engine::Definition*> items = registry.listByCategory(category);
            if (items.empty()) {
                continue;
            }
            if (!ImGui::CollapsingHeader(engine::categoryName(category),
                                         ImGuiTreeNodeFlags_DefaultOpen)) {
                continue;
            }

            const char* subgroup = nullptr;
            for (const SubgroupLabel& item : subgroup_labels) {
                if (item.category == category) {
                    subgroup = item.label;
                    break;
                }
            }
            if (subgroup != nullptr) {
                ImGui::Indent(4.0f);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", subgroup);
                ImGui::SameLine();
                ImGui::TextDisabled("(子分类)");
                ImGui::Unindent(4.0f);
            }

            for (const engine::Definition* definition : items) {
                if (draw_library_item(definition, category, query)) {
                    canvas_add_node_at_center(definition->type);
                }
            }
            ImGui::Spacing();
        }
    }

    ImGui::End();
}

} // namespace aiwrite::ui