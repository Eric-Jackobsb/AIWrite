#include "property_panel.h"

#include "editor_controller.h"
#include "output_panel.h"   // node_output_text（运行结果全文，与输出面板同源）
#include "text_view.h"      // draw_readonly_text / copyText
#include "texture_cache.h"  // M5-03：图片结果预览（与输出面板共用纹理缓存）
#include "log.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <string>

#include "property_panel_detail.h"  // ARCH-Z1：本 TU 提供 draw_property_panel_runtime

namespace aiwrite::ui {



using engine::Node;       // 原 property_panel.cpp 匿名命名空间内的 using（本 TU 逐字沿用）
using engine::Param;
using engine::ParamType;


void draw_property_panel_runtime(engine::Node* node, const ReadOnlyView& view,
                                 PropertyEditResult& result, EditorController& editor)
{
    // ---- 运行状态（PA-03）：只读展示 + 复制（与输出面板 / 画布节点摘要同源）----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("运行状态", ImGuiTreeNodeFlags_DefaultOpen)) {
        const engine::RunSnapshot& snapshot = view.snapshot;   // ARCH-R3：渲染读取走 view
        const engine::RunNodeView* run      = snapshot.find(node->id);

        if (run == nullptr) {
            ImGui::TextDisabled("尚未运行（点工具栏「▶ 运行」；首次网页版会话约数秒）");
        }
        else {
            ImGui::TextDisabled("状态: %s    耗时: %.2f ms", engine::nodeStateName(run->state),
                                run->duration_ms);
            if (!run->error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("错误：%s", run->error.c_str());
                ImGui::PopStyleColor();
            }

            // M5-03：图片结果预览（与输出面板共用纹理缓存）
            if (!run->images.empty()) {
                for (std::size_t index = 0; index < run->images.size(); ++index) {
                    ImGui::PushID(static_cast<int>(3000 + index));
                    const TextureInfo texture = editor.textures().textureFor(run->images[index]);
                    if (texture.texture != 0) {
                        const float limit  = std::min(ImGui::GetContentRegionAvail().x, 320.0f);
                        float       width  = static_cast<float>(texture.width);
                        float       height = static_cast<float>(texture.height);
                        if (width > limit && width > 0.0f) {
                            const float shrink = limit / width;
                            width *= shrink;
                            height *= shrink;
                        }
                        ImGui::Image(
                            reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture.texture)),
                            ImVec2(width, height));
                        ImGui::TextDisabled("预览 %d×%d", texture.width, texture.height);
                    }
                    else {
                        ImGui::TextDisabled("图片不可用：%s", texture.error.c_str());
                    }
                    ImGui::PopID();
                }
            }

            const std::string body = node_output_text(view.graph(), snapshot, node->id);
            if (!body.empty()) {
                draw_readonly_text("##prop_run_result", body, 8.0f);
                if (ImGui::Button("复制运行结果")) {
                    copyText(body, "[参数面板] " + node->id);
                }
            }
            if (node->type == "TextOutput") { // M_textio P3：最终输出
                std::string label = "输出";
                if (const engine::Param* label_param = node->findParam("label")) {
                    const std::string text = label_param->text();
                    if (!text.empty()) {
                        label = text;
                    }
                }
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "【最终输出】%s", label.c_str());
                ImGui::BeginDisabled(body.empty());
                if (ImGui::Button("导出为文档…")) {
                    exportNodeDocument(editor, node->id);
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("导出为 .md/.txt（与预览同源，含元信息头）");
                }
            }
            else if (run->error.empty() && run->images.empty()) {
                ImGui::TextDisabled("（该节点无输出）");
            }

            if (!editor.workflow_file().last_archive_dir().empty()) {
                ImGui::TextDisabled("已归档：%s", editor.workflow_file().last_archive_dir().c_str());
                if (ImGui::SmallButton("复制归档路径")) {
                    copyText(editor.workflow_file().last_archive_dir(), "[参数面板] 归档路径");
                }
            }
        }
    }

    // ---- M_rerun：「重新生成（新 seed）」—— 等效“再次运行”，但使用新的随机种子 ----
    if (node->type == "LLMGenerate") {
        ImGui::Separator();
        if (const engine::Param* seed_param = node->findParam("seed")) {
            ImGui::TextDisabled("随机种子：%d（0 = 不指定；「重新生成」会写入新值）",
                                static_cast<int>(seed_param->number(0.0)));
        }
        const bool rerunning = editor.executor().running() || editor.session_active();
        ImGui::BeginDisabled(rerunning);
        if (ImGui::Button("重新生成（新 seed）", ImVec2(-FLT_MIN, 0.0f))) {
            if (engine::Param* seed_param = node->findParam("seed")) {
                editor.snapshot("重新生成（新 seed）"); // 先压快照：可撤销
                const long long stamp = static_cast<long long>(std::time(nullptr));
                const int       value = static_cast<int>(stamp % 1000000000LL); // ∈ 参数范围 [0, 1e9]
                seed_param->value     = (value > 0) ? value : 1;
                const int applied     = static_cast<int>(seed_param->number(0.0));
                log::info("[重跑] " + node->id + " 新 seed=" + std::to_string(applied));
                editor.setStatus("已为 " + node->id + " 设置新 seed=" + std::to_string(applied) +
                                    "，开始重新生成…");
                editor.startRunAsync();
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("等效于「再次运行」，但使用新的随机种子：\n"
                              "· 官方 API：请求体带 seed\n"
                              "· 网页版：协议无 seed 槽位（忽略，但重跑结果也会不同）\n"
                              "· 会先记录一步撤销，可回退到旧 seed");
        }
    }

    // ---- PD-06①（FEA-M4-11 · 通用重跑）：重跑该节点 / 重跑该节点及下游（复用上游结果）----
    if (node->state != engine::NodeState::Waiting) {
        ImGui::Separator();
        const bool rerunning = editor.executor().running() || editor.session_active();
        ImGui::BeginDisabled(rerunning);
        if (ImGui::Button("重跑该节点", ImVec2(-FLT_MIN, 0.0f))) {
            editor.startRerunAsync(node->id, engine::RerunScope::NodeOnly);
        }
        if (ImGui::Button("重跑该节点及下游", ImVec2(-FLT_MIN, 0.0f))) {
            editor.startRerunAsync(node->id, engine::RerunScope::Downstream);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("重跑**复用上游结果**（上游不重算）；受影响节点的结果与状态会先清除。\n"
                              "· 重跑该节点：只重跑当前节点\n"
                              "· 重跑该节点及下游：重跑当前节点及其全部下游\n"
                              "（上游值取自上一次运行；若上游尚未成功运行过，本节点会以清晰报错失败）");
        }
    }

    // ---- 底部操作（F2：批量应用 / 重置 均需二次确认）----
    static std::string confirm_apply_node; // 待确认"应用到同类节点"的节点 id
    static std::string confirm_reset_node; // 待确认"重置为默认值"的节点 id

    ImGui::Separator();
    int same_type_others = 0;
    for (const engine::Node& other : view.nodes_for_render()) {
        if (other.id != node->id && other.type == node->type) {
            ++same_type_others;
        }
    }
    ImGui::TextDisabled("同类型（%s）其他节点：%d 个", node->type.c_str(), same_type_others);

    if (confirm_apply_node == node->id) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                           "确认把 %s 的参数应用到 %d 个同类节点？（密钥类参数不复制）",
                           node->id.c_str(), same_type_others);
        if (ImGui::Button("确认应用", ImVec2(120.0f, 0.0f))) {
            editor.snapshot("批量应用参数到同类节点"); // 先压快照：整批应用可一次撤销
            std::string error;
            const int   affected = editor.model().graph.copyParamsToSameType(node->id, &error);
            if (affected > 0) {
                result.changed = true;
                editor.setStatus("已把 " + node->id + " 的参数应用到 " +
                                    std::to_string(affected) + " 个同类节点（可撤销）");
                log::info("[参数面板] 批量应用参数: " + node->id + " → " +
                          std::to_string(affected) + " 个同类节点");
            }
            else {
                editor.setStatus("批量应用未生效：" +
                                    (error.empty() ? std::string("没有同类节点") : error));
            }
            confirm_apply_node.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##apply", ImVec2(80.0f, 0.0f))) {
            confirm_apply_node.clear();
        }
    }
    else {
        ImGui::BeginDisabled(same_type_others == 0);
        if (ImGui::Button("把当前参数应用到全部同类节点", ImVec2(-FLT_MIN, 0.0f))) {
            confirm_apply_node = node->id; // 二次确认（批量改动多节点）
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("按参数 id 复制到同类型节点（不改动标题 / 位置 / 连线）；\n"
                              "密钥类参数（API Key）不会被复制");
        }
    }

    ImGui::Separator();
    if (confirm_reset_node == node->id) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "确认把 %s 的全部参数重置为默认值？",
                           node->id.c_str());
        if (ImGui::Button("确认重置", ImVec2(120.0f, 0.0f))) {
            editor.snapshot("重置参数为默认值"); // 先压快照：可撤销（D-M3-5）
            for (Param& param : node->params) {
                param.resetToDefault();
            }
            result.changed = true;
            log::info("参数重置为默认值: " + node->id);
            editor.setStatus("已把 " + node->id + " 的参数重置为默认值（可撤销）");
            confirm_reset_node.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消##reset", ImVec2(80.0f, 0.0f))) {
            confirm_reset_node.clear();
        }
    }
    else {
        if (ImGui::Button("重置为默认值", ImVec2(-FLT_MIN, 0.0f))) {
            confirm_reset_node = node->id; // 二次确认（避免误触清空编辑）
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("把该节点全部参数恢复为定义中的默认值（需再确认一次；\n"
                              "执行前会自动记录一步撤销，可回退）");
        }
    }

    ImGui::End();
}
} // namespace aiwrite::ui
