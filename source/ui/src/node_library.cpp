#include "node_library.h"

#include "composite.h"          // CN：上级节点注册表
#include "composite_editor.h"   // CN：新建/打包对话框 + 编辑器会话状态（删除前 force_close）
#include "commands.h"           // ARCH-R4：命令路由层（新建/打包上级节点 → dispatchCommand）
#include "editor_controller.h"       // PS-04：删除结果回写状态栏
#include "log.h"
#include "node_registry.h"
#include "node_canvas.h"
#include "string_util.h"        // CLN-05：findCi 收敛
#include "theme.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace aiwrite::ui {
namespace {

// ---- PS-04 / §7.1：上级节点列表项操作状态（右键删除 / 复制到我的节点库）----
std::string g_composite_context_id;             // 右键命中的上级节点 id
bool        g_composite_context_open = false;   // 右键命中后待打开菜单（须在 PushID 作用域外 OpenPopup）
std::string g_composite_delete_pending;         // 待删除确认的 id
bool        g_composite_delete_confirm_open = false;
std::string g_composite_copy_pending;           // 待复制确认的 id
bool        g_composite_copy_confirm_open = false;

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
        const std::size_t pos = utils::findCi(definition->display_name, query);
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

void drawNodeLibrary(const char* title, bool* open, EditorController& editor)
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
    const bool        searching = utils::findCi(search_buf, "") != 0 ? false : (search_buf[0] != '\0');
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
            if (utils::findCi(definition->display_name, query) == std::string::npos &&
                utils::findCi(definition->type, query) == std::string::npos) {
                continue;
            }
            any = true;
            if (draw_library_item(definition, definition->category, query)) {
                canvasAddNodeAtCenter(definition->type, editor);
            }
        }
        ImGui::PopStyleColor();
        if (!any) {
            ImGui::TextDisabled("（无匹配节点）");
        }
    }
    else {
        // ---- CN：上级节点（组合节点）独立分类 ----
        if (ImGui::CollapsingHeader("上级节点", ImGuiTreeNodeFlags_DefaultOpen)) {
            const std::vector<const engine::CompositeSpec*> composites =
                engine::CompositeRegistry::instance().list();
            if (composites.empty()) {
                ImGui::TextDisabled("（暂无上级节点）");
            }
            for (const engine::CompositeSpec* spec : composites) {
                const bool builtin = engine::CompositeRegistry::instance().is_builtin(spec->id);
                ImGui::PushID(spec->id.c_str());

                // PS-D9 §7.1：官方徽标 —— 内置源项名称前画金色小圆点
                if (builtin) {
                    const ImVec2 cursor = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        ImVec2(cursor.x + 4.0f, cursor.y + ImGui::GetTextLineHeight() * 0.5f),
                        4.0f, IM_COL32(217, 158, 46, 255), 12);
                    ImGui::Dummy(ImVec2(10.0f, ImGui::GetTextLineHeight()));
                    ImGui::SameLine(0.0f, 2.0f);
                }

                if (ImGui::Selectable(spec->name.c_str())) {
                    canvasAddCompositeAtCenter(spec->id, editor);
                }
                const bool row_hovered = ImGui::IsItemHovered();

                // 右键列表项 → 删除（用户项）/ 只读提示（内置项）
                if (row_hovered && ImGui::IsMouseClicked(1)) {
                    g_composite_context_id  = spec->id;
                    g_composite_context_open = true; // 在 PushID 作用域外统一 OpenPopup（见下）
                }

                // 复制按钮（仅内置源项；PS-D9 §7.1）
                if (builtin) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("⧉ 复制")) {
                        g_composite_copy_pending        = spec->id;
                        g_composite_copy_confirm_open   = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("复制一份到我的节点库，之后可编辑（官方预设为只读）");
                    }
                }

                if (row_hovered) {
                    ImGui::BeginTooltip();
                    ImGui::TextWrapped("%s", spec->description.c_str());
                    if (builtin) {
                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(0.85f, 0.62f, 0.18f, 1.0f),
                                           "内置官方预设（只读）· 复制到我的节点库后可编辑");
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("输入 %d / 输出 %d / 参数 %d",
                                        static_cast<int>(spec->inputs.size()),
                                        static_cast<int>(spec->outputs.size()),
                                        static_cast<int>(spec->exposed_params.size()));
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
            // 列表项右键菜单（循环外统一渲染；context id 由循环内右键设置）
            //  ⚠️ OpenPopup 必须与 BeginPopup 处于**同一 ImGui ID 作用域**：若在列表项的
            //     PushID 内 OpenPopup，Popup id 会带上「<列表项id>##composite_item_menu」前缀，
            //     与本处的「##composite_item_menu」不匹配 ⇒ 菜单永不弹出（2026-10-18 修复）。
            if (g_composite_context_open) {
                ImGui::OpenPopup("##composite_item_menu");
                g_composite_context_open = false;
            }
            if (ImGui::BeginPopup("##composite_item_menu")) {
                const engine::CompositeSpec* ctx =
                    engine::CompositeRegistry::instance().find(g_composite_context_id);
                const bool ctx_builtin =
                    ctx != nullptr && engine::CompositeRegistry::instance().is_builtin(ctx->id);
                if (ctx_builtin) {
                    ImGui::TextDisabled("内置官方预设（只读）");
                    ImGui::Separator();
                    if (ImGui::MenuItem("复制到我的节点库…")) {
                        g_composite_copy_pending      = ctx->id;
                        g_composite_copy_confirm_open = true;
                    }
                }
                else if (ctx != nullptr) {
                    if (ImGui::MenuItem("删除上级节点…")) {
                        g_composite_delete_pending      = ctx->id;
                        g_composite_delete_confirm_open = true;
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::Separator();
            if (ImGui::Button("新建上级节点…", ImVec2(-1.0f, 0.0f))) {
                dispatchCommand(AppCommand::CompositeNew, CommandContext{nullptr, nullptr, &editor}); // ARCH-R4：与菜单同一实现
            }
            if (ImGui::Button("打包选区为上级节点…", ImVec2(-1.0f, 0.0f))) {
                dispatchCommand(AppCommand::CompositePack, CommandContext{nullptr, nullptr, &editor}); // ARCH-R4：与菜单同一实现
            }
            ImGui::Spacing();
            ImGui::Separator();
        }

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
                    canvasAddNodeAtCenter(definition->type, editor);
                }
            }
            ImGui::Spacing();
        }
    }

    // ---- PS-04 / §7.1：删除 / 复制确认弹窗 ----
    if (g_composite_delete_confirm_open) {
        ImGui::OpenPopup("删除上级节点##composite_delete");
        g_composite_delete_confirm_open = false;
    }
    if (ImGui::BeginPopupModal("删除上级节点##composite_delete", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        const engine::CompositeSpec* target =
            engine::CompositeRegistry::instance().find(g_composite_delete_pending);
        ImGui::TextWrapped("确定删除上级节点「%s」？",
                           target != nullptr ? target->name.c_str()
                                             : g_composite_delete_pending.c_str());
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f),
                           "此操作不可恢复；画布上正在使用的实例将变成孤儿节点（无规格可执行）。");
        ImGui::Spacing();
        if (ImGui::Button("删除", ImVec2(90.0f, 0.0f))) {
            // 目标节点正被编辑器打开 → 先强制关闭编辑器（§9.1②）
            if (compositeEditorState().active &&
                compositeEditorState().working.id == g_composite_delete_pending) {
                compositeEditorForceClose();
            }
            std::string error;
            if (!engine::CompositeRegistry::instance().remove(g_composite_delete_pending,
                                                              &error)) {
                editor.setStatus("删除上级节点失败：" + error);
                log::warn("[上级节点] 删除失败: " + error);
            }
            else {
                editor.setStatus("已删除上级节点：" + g_composite_delete_pending);
                log::info("[上级节点] 已删除 " + g_composite_delete_pending);
            }
            g_composite_delete_pending.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(90.0f, 0.0f))) {
            g_composite_delete_pending.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_composite_copy_confirm_open) {
        ImGui::OpenPopup("复制到我的节点库##composite_copy");
        g_composite_copy_confirm_open = false;
    }
    if (ImGui::BeginPopupModal("复制到我的节点库##composite_copy", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        const engine::CompositeSpec* target =
            engine::CompositeRegistry::instance().find(g_composite_copy_pending);
        ImGui::TextWrapped("将复制「%s」到我的节点库。复制后可编辑；删除用户副本将恢复官方预设。",
                           target != nullptr ? target->name.c_str()
                                             : g_composite_copy_pending.c_str());
        ImGui::Spacing();
        if (ImGui::Button("复制", ImVec2(90.0f, 0.0f))) {
            std::string error;
            if (!engine::CompositeRegistry::instance().copyToUser(g_composite_copy_pending,
                                                                    &error)) {
                editor.setStatus("复制到我的节点库失败：" + error);
                log::warn("[上级节点] 复制失败: " + error);
            }
            else {
                editor.setStatus("已复制到我的节点库（可编辑）：" + g_composite_copy_pending);
                log::info("[上级节点] 已复制到用户源 " + g_composite_copy_pending);
            }
            g_composite_copy_pending.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(90.0f, 0.0f))) {
            g_composite_copy_pending.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace aiwrite::ui