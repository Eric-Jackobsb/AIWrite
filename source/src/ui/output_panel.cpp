#include "ui/output_panel.h"

#include "engine/executor.h"
#include "engine/graph.h"
#include "ui/editor_state.h"
#include "ui/text_view.h"
#include "ui/texture_cache.h"
#include "utils/file_dialog.h"
#include "utils/text_export.h"
#include "utils/log.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace aiwrite::ui {
namespace {

// 面板内显示上限（只截断**显示**；复制与导出始终为完整内容）
constexpr std::size_t kDisplayLimit = 40000;

const ImVec4 kColorDone(0.31f, 0.75f, 0.42f, 1.0f);
const ImVec4 kColorError(1.00f, 0.35f, 0.35f, 1.0f);
const ImVec4 kColorSkip(0.85f, 0.70f, 0.30f, 1.0f);
const ImVec4 kColorFinal(0.55f, 0.85f, 1.00f, 1.0f); // M_textio：最终输出高亮

ImVec4 state_color(engine::NodeState state)
{
    switch (state) {
    case engine::NodeState::Done:    return kColorDone;
    case engine::NodeState::Error:   return kColorError;
    case engine::NodeState::Skipped: return kColorSkip;
    default:                         return ImGui::GetStyle().Colors[ImGuiCol_Text];
    }
}

std::string node_header(const engine::NodeRunInfo& info)
{
    return "[" + info.node_id + "] " + info.type + " · " + engine::nodeStateName(info.state) +
           " · " + std::to_string(static_cast<long long>(info.duration_ms + 0.5)) + " ms";
}

// PB-01：读模型条目使用同一标题格式（面板改读 RunSnapshot 后仍与自检一致）
std::string node_header(const engine::RunNodeView& info)
{
    std::string header = "[" + info.node_id + "] " + info.type + " · " +
                         engine::nodeStateName(info.state) + " · " +
                         std::to_string(static_cast<long long>(info.duration_ms + 0.5)) + " ms";
    // PB-08：运行中显示流式进度（正文会随增量逐段增长）
    if (info.state == engine::NodeState::Running) {
        header += " · 生成中（" + std::to_string(info.delta_bytes) + " 字）";
    }
    return header;
}

// M5-03：渲染节点结果里的图片（缩略图 + 打开所在文件夹）
//  * 纹理按「路径 + 修改时间 + 大小」缓存（`ui/texture_cache`），重复运行不重复解码
//  * image_height > 0 时限制最大显示高度（保持宽高比）
//  * 图片不可用时给出原因（一行红字，不占版面）
void draw_result_images(const engine::RunNodeView& info, float image_height)
{
    for (std::size_t index = 0; index < info.images.size(); ++index) {
        const std::string& path = info.images[index];
        ImGui::PushID(static_cast<int>(index));
        const TextureInfo texture = texture_for(path);
        if (texture.texture != 0) {
            const float available = ImGui::GetContentRegionAvail().x;
            float       width     = static_cast<float>(texture.width);
            float       height    = static_cast<float>(texture.height);
            if (available > 32.0f && width > available) {
                const float shrink = available / width;
                width *= shrink;
                height *= shrink;
            }
            if (image_height > 0.0f && height > image_height) {
                const float shrink = image_height / height;
                width *= shrink;
                height *= shrink;
            }
            ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture.texture)),
                         ImVec2(width, height));
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, kColorError);
            ImGui::TextWrapped("图片不可用：%s", texture.error.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::SmallButton("打开所在文件夹")) {
            utils::open_in_explorer(path);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s（%d×%d）", path.c_str(), texture.width, texture.height);
        ImGui::PopID();
    }
}

} // namespace

std::string node_output_text(const engine::Graph& graph, const engine::Executor& executor,
                             const std::string& node_id)
{
    // PB-01：委托到引擎侧唯一实现（多线程下只有一份规则）
    return engine::nodeOutputText(graph, executor.outputs(), node_id);
}

std::string run_output_text(const engine::Graph& graph, const engine::Executor& executor)
{
    // PB-01：单一实现 —— 走快照重载（快照文本由 makeSnapshot 用同一规则落定）
    return run_output_text(graph, engine::makeSnapshot(graph, executor));
}

std::string node_output_text(const engine::Graph& graph, const engine::RunSnapshot& snapshot,
                             const std::string& node_id)
{
    (void)graph;
    const engine::RunNodeView* view = snapshot.find(node_id);
    return view != nullptr ? view->text : std::string();
}

std::string run_output_text(const engine::Graph& graph, const engine::RunSnapshot& snapshot)
{
    (void)graph;
    std::string text;
    for (const engine::RunNodeView& info : snapshot.nodes) {
        if (info.text.empty() && info.error.empty()) {
            continue; // 无输出也无错误（如被跳过的中间节点）→ 不占版面
        }
        if (!text.empty()) {
            text += "\n\n";
        }
        text += node_header(info);
        if (!info.error.empty()) {
            text += "\n错误：" + info.error;
        }
        if (!info.text.empty()) {
            text += "\n" + info.text;
        }
    }
    return text;
}

bool export_node_document(EditorState& state, const std::string& node_id)
{
    const engine::Node* node = state.graph.findNode(node_id);
    if (node == nullptr) {
        return false;
    }
    const engine::RunSnapshot& snapshot = state.run_snapshot_view();
    const engine::RunNodeView* view     = snapshot.find(node_id);
    const std::string body = node_output_text(state.graph, snapshot, node_id);
    if (body.empty()) {
        state.set_status("该节点暂无可导出的运行结果（请先运行）");
        return false;
    }
    std::string label = "输出";
    if (const engine::Param* label_param = node->findParam("label")) {
        const std::string text = label_param->text();
        if (!text.empty()) {
            label = text;
        }
    }
    const std::string stamp = utils::document_stamp_now();
    std::string stem = state.workflow_display_name(); // M_textio P5：文件名主干可由参数覆盖
    if (const engine::Param* name_param = node->findParam("file_name")) {
        const std::string text = name_param->text();
        if (!text.empty()) {
            stem = text;
        }
    }
    std::string default_dir; // M_textio P5：导出目录参数作为对话框默认目录
    if (const engine::Param* dir_param = node->findParam("export_dir")) {
        default_dir = dir_param->text();
    }
    const std::string default_name = utils::render_document_name(stem, stamp, "md");
    const std::string path =
        utils::save_file({{"Markdown", "md"}, {"文本", "txt"}}, default_dir, default_name);
    if (path.empty()) {
        return false; // 用户取消
    }
    utils::ExportRequest request;
    request.text          = body;
    request.label         = label;
    request.include_meta  = true;
    request.overwrite     = true; // 已在保存对话框确认
    request.path          = path;
    request.meta.workflow = state.workflow_display_name();
    request.meta.stamp    = utils::document_time_text();
    if (view != nullptr) {
        request.meta.node_id     = view->node_id;
        request.meta.state       = engine::nodeStateName(view->state);
        request.meta.duration_ms = view->duration_ms;
    }
    const utils::ExportResult result = utils::export_text_document(request);
    if (!result.ok) {
        state.set_status("导出文档失败：" + result.error);
        log::error("[输出面板] 导出文档失败：" + result.error);
        return false;
    }
    state.set_status("已导出文档：" + result.path);
    log::info("[输出面板] 已导出文档：" + result.path);
    return true;
}

void draw_output_panel(const char* title, bool* open, EditorState& state)
{
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }

    const engine::Executor& executor = state.executor;
    const engine::RunSnapshot& snapshot = state.run_snapshot_view();
    const auto&             infos    = snapshot.nodes;

    // M_textio P3：最终输出（TextOutput）置顶（其余保持原顺序）
    std::vector<engine::RunNodeView> ordered;
    ordered.reserve(infos.size());
    for (const engine::RunNodeView& info : infos) {
        const engine::Node* node = state.graph.findNode(info.node_id);
        if (node != nullptr && node->type == "TextOutput") {
            ordered.push_back(info);
        }
    }
    for (const engine::RunNodeView& info : infos) {
        const engine::Node* node = state.graph.findNode(info.node_id);
        if (node == nullptr || node->type != "TextOutput") {
            ordered.push_back(info);
        }
    }

    // ---- 顶部工具条 ----
    const std::string all_text = run_output_text(state.graph, snapshot);
    const bool        has_text = !all_text.empty();

    ImGui::BeginDisabled(!has_text);
    if (ImGui::Button("复制全文")) {
        copy_text(all_text, "[输出面板] 全文");
    }
    ImGui::SameLine();
    if (ImGui::Button("导出到文件…")) {
        const std::string path =
            utils::save_file({{"文本", "txt"}, {"全部文件", "*"}}, {}, "aiwrite-output.txt");
        if (!path.empty()) {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << all_text;
            if (stream.good()) {
                log::info("[输出面板] 已导出运行结果：" + path + "（" +
                          std::to_string(all_text.size()) + " 字符）");
            }
            else {
                log::error("[输出面板] 导出失败：" + path);
            }
        }
    }
    ImGui::EndDisabled();

    // M_textio P3：一键导出“最终输出”
    {
        std::string final_node;
        for (const engine::RunNodeView& info : ordered) {
            const engine::Node* node = state.graph.findNode(info.node_id);
            if (node != nullptr && node->type == "TextOutput" &&
                !node_output_text(state.graph, snapshot, info.node_id).empty()) {
                final_node = info.node_id;
                break;
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(final_node.empty());
        if (ImGui::Button("导出最终输出为文档…")) {
            export_node_document(state, final_node);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("把 TextOutput（最终输出）的结果导出为 .md/.txt");
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("｜ 节点 %zu ｜ 总耗时 %.2f s", infos.size(), executor.elapsedSeconds());

    // PC-05：最近一次归档位置（可复制路径，便于去目录里取生成文档）
    if (!state.last_archive_dir.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("复制归档路径")) {
            ImGui::SetClipboardText(state.last_archive_dir.c_str());
            log::info("[输出面板] 已复制归档路径：" + state.last_archive_dir);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("运行结果已自动归档：\n%s", state.last_archive_dir.c_str());
        }
        ImGui::TextDisabled("已归档：%s", state.last_archive_dir.c_str());
    }
    ImGui::Separator();

    if (infos.empty()) {
        ImGui::TextDisabled("尚无运行结果：点工具栏「▶ 运行」");
        ImGui::End();
        return;
    }

    // ---- 逐节点分段（M_textio P3：最终输出置顶 + 高亮 + 导出为文档）----
    for (const engine::RunNodeView& info : ordered) {
        const std::string   body     = node_output_text(state.graph, snapshot, info.node_id);
        const engine::Node* node     = state.graph.findNode(info.node_id);
        const bool          is_final = (node != nullptr && node->type == "TextOutput");
        std::string         header   = node_header(info);
        if (is_final) {
            std::string label = "输出";
            if (node->findParam("label") != nullptr) {
                const std::string text = node->findParam("label")->text();
                if (!text.empty()) {
                    label = text;
                }
            }
            header = "【最终输出】" + label + "    " + header;
        }
        const bool leaf = body.empty() && info.error.empty() && info.images.empty();

        ImGui::PushID(info.node_id.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, is_final ? kColorFinal : state_color(info.state));
        const bool open_section = ImGui::CollapsingHeader(
            header.c_str(), leaf ? ImGuiTreeNodeFlags_Leaf : ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor();

        if (open_section) {
            if (!info.error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kColorError);
                ImGui::TextWrapped("错误：%s", info.error.c_str());
                ImGui::PopStyleColor();
            }
            if (!body.empty()) {
                const std::string view_id = "##result_" + info.node_id;
                draw_readonly_text(view_id.c_str(), body, 10.0f, kDisplayLimit);
                if (ImGui::Button("复制该节点")) {
                    copy_text(body, "[输出面板] " + info.node_id);
                }
                if (is_final) {
                    ImGui::SameLine();
                    if (ImGui::Button("导出为文档…")) {
                        export_node_document(state, info.node_id);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("把该节点的最终输出导出为 .md/.txt（与预览同源，含元信息头）");
                    }
                }
            }
            // M5-03：图片结果（ImageInput 透传 / ImagePreview 预览）
            if (!info.images.empty()) {
                draw_result_images(info, 480.0f);
            }
            if (body.empty() && info.images.empty() && info.error.empty()) {
                ImGui::TextDisabled("（该节点无输出）");
            }
        }
        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace aiwrite::ui
