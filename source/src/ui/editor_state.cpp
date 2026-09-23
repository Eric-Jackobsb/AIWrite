#include "ui/editor_state.h"

#include "engine/node_registry.h"
#include "engine/workflow_io.h"
#include "utils/log.h"
#include "utils/paths.h"

#include <algorithm>
#include <filesystem>

namespace aiwrite::ui {

EditorState& editor()
{
    static EditorState state;
    return state;
}

void EditorState::set_status(const std::string& text)
{
    status = text;
}

void EditorState::log_summary(const std::string& prefix) const
{
    log::info(prefix + "（节点 " + std::to_string(graph.nodes.size()) + "，连线 " +
              std::to_string(graph.edges.size()) + "）");
}

// ------------------------------------------------------------------ 撤销 -----
void EditorState::snapshot(const std::string& label)
{
    undo.push(graph, label);
}

bool EditorState::undo_once()
{
    engine::Graph restored;
    std::string   label;
    if (!undo.undo(graph, restored, &label)) {
        set_status("没有可撤销的操作");
        return false;
    }
    graph = std::move(restored);
    selected_nodes.clear();
    selected_node.clear();
    log_summary("[撤销] " + label);
    set_status("已撤销：" + label);
    return true;
}

bool EditorState::redo_once()
{
    engine::Graph restored;
    std::string   label;
    if (!undo.redo(graph, restored, &label)) {
        set_status("没有可重做的操作");
        return false;
    }
    graph = std::move(restored);
    selected_nodes.clear();
    selected_node.clear();
    log_summary("[重做] " + label);
    set_status("已重做：" + label);
    return true;
}

// ----------------------------------------------------------------- 工作流 ----
void EditorState::clear_workflow()
{
    graph.clear();
    undo.clear();
    selected_nodes.clear();
    selected_node.clear();
    clipboard_nodes_.clear();
    clipboard_edges_.clear();
    log_summary("[新建] 已清空画布");
    set_status("画布已清空（撤销栈也已清空）");
}

void EditorState::create_sample_workflow()
{
    graph.clear();
    undo.clear();

    graph.name        = "默认工作流";
    graph.description = "文本输入 → 提示词模板 → 文本生成 → 文本输出（提供商配置接入文本生成的 provider 输入）";

    create_node("TextInput", -420.0f, -40.0f);
    create_node("PromptTemplate", -120.0f, -40.0f);
    create_node("LLMGenerate", 220.0f, -40.0f);
    create_node("TextOutput", 560.0f, -40.0f);
    // 提供商配置放在文本生成下方，其 provider 输出接到文本生成的 provider 输入
    create_node("ProviderConfig", 220.0f, 260.0f);

    if (graph.nodes.size() == 5) {
        const std::string input_id    = graph.nodes[0].id;
        const std::string prompt_id   = graph.nodes[1].id;
        const std::string llm_id      = graph.nodes[2].id;
        const std::string output_id   = graph.nodes[3].id;
        const std::string provider_id = graph.nodes[4].id;

        std::string error;
        graph.addEdge(input_id, "text", prompt_id, "vars", &error);
        graph.addEdge(prompt_id, "text", llm_id, "prompt", &error);
        graph.addEdge(llm_id, "text", output_id, "text", &error);
        // 设计 §9.2：ProviderConfig 输出 provider 句柄 → 推理节点的 provider 输入
        graph.addEdge(provider_id, "provider", llm_id, "provider", &error);

        if (engine::Node* input = graph.findNode(input_id)) {
            if (engine::Param* param = input->findParam("text")) {
                param->value = std::string("从前有座山，山里有座庙。");
            }
        }
    }

    // 默认启动工作流落盘：~/.brain-ai/workflows/default.json（设计 §11.3 Workflow 存储）
    // 注意：密钥类参数（is_secret，如 API Key）不会写入文件
    const std::filesystem::path default_file = paths::workflows_dir() / "default.json";
    std::string                 save_error;
    if (engine::save_workflow(graph, default_file, &save_error)) {
        log::info("[默认工作流] 已保存到 " + default_file.string());
    }
    else {
        log::warn("[默认工作流] 保存失败: " + save_error);
    }

    undo.clear(); // 默认工作流作为初始状态，不进入撤销历史
    selected_nodes.clear();
    selected_node.clear();
    request_navigate_to_content = true;
    log_summary("[默认工作流] 已创建");
    set_status("已创建默认工作流（含提供商配置 → 文本生成），并保存到 default.json");
}

// ------------------------------------------------------------------ 节点 -----
std::string EditorState::create_node(const std::string& type, float x, float y)
{
    std::string error;
    const std::string id = graph.addNode(type, x, y, &error);
    if (id.empty()) {
        log::error("创建节点失败: " + error);
        set_status("创建节点失败：" + error);
        return {};
    }

    const engine::Definition* definition = engine::NodeRegistry::instance().find(type);
    const std::string name = (definition != nullptr) ? definition->display_name : type;
    log::info("创建节点 " + id + "（" + name + "）");
    set_status("已创建节点：" + name + "（" + id + "）");
    return id;
}

bool EditorState::delete_node(const std::string& node_id)
{
    const engine::Node* node = graph.findNode(node_id);
    if (node == nullptr) {
        return false;
    }
    const std::string title = node->title;

    if (!graph.removeNode(node_id)) {
        return false;
    }

    selected_nodes.erase(std::remove(selected_nodes.begin(), selected_nodes.end(), node_id),
                         selected_nodes.end());
    if (selected_node == node_id) {
        selected_node.clear();
    }
    log::info("删除节点 " + node_id + "（" + title + "）");
    return true;
}

bool EditorState::delete_selected()
{
    if (selected_nodes.empty() && selected_links.empty()) {
        set_status("没有选中的节点或连线");
        return false;
    }

    const std::size_t node_total = selected_nodes.size();
    const std::size_t link_total = selected_links.size();

    // ---- 节点：直接删除（连带其连线，内部已按需压快照）----
    if (!selected_nodes.empty()) {
        snapshot("删除选中节点");
        const std::vector<std::string> targets = selected_nodes;
        for (const std::string& id : targets) {
            delete_node(id);
        }
        selected_nodes.clear();
        selected_node.clear();
    }

    // ---- 连线：同样直接删模型 ----
    // 不能用 ed::DeleteLink()：DeleteItemsAction::Add() 在 GetCurrentAction() != nullptr 时
    // **静默返回 false**（右键菜单打开期间当前动作就是上下文菜单动作），因此删除不会入队。
    // 本工程以 Graph 为唯一真相：删掉边后画布不再提交该连线，编辑器会在本帧 End() 时
    // 因"未被提交（!m_IsLive）"自动回收内部连线对象（见 imgui_node_editor.cpp L1307）。
    if (!selected_links.empty()) {
        snapshot("删除选中连线");
        for (const std::string& edge_id : selected_links) {
            if (graph.removeEdge(edge_id)) {
                log::info("删除连线 " + edge_id);
            }
        }
        selected_links.clear();
    }

    log_summary("[删除] 已删除 节点 " + std::to_string(node_total) + " / 连线 " +
                std::to_string(link_total));
    set_status("已删除 节点 " + std::to_string(node_total) + " 个、连线 " +
               std::to_string(link_total) + " 条");
    return true;
}

// -------------------------------------------------------------- 复制/粘贴 ----
// 【暂停接线】UI 入口（右键菜单 / 工具栏 / 编辑菜单）已下线，函数保留待 M2/M3 重做时接回。
// 暂停原因：粘贴会新建节点并请求视图跟随，而 ed::GetNodePosition() 对"编辑器还不认识"的手柄
// 返回 FLT_MAX（imgui_node_editor.cpp:1676），旧版 sync_positions() 会把该值当位置回写模型，
// 叠加 NavigateToContent 后视图落入 ~1e9 量级坐标 → 界面无响应（详见 CHANGELOG「复制/粘贴暂停」）。
// 现已加位置守卫（node_canvas.cpp：clamp_position / 未知手柄时反向推送 / 视图跟随前校验）。
void EditorState::copy_selection()
{
    clipboard_nodes_.clear();
    clipboard_edges_.clear();

    if (selected_nodes.empty()) {
        set_status("没有选中的节点");
        return;
    }

    for (const std::string& id : selected_nodes) {
        if (const engine::Node* node = graph.findNode(id)) {
            clipboard_nodes_.push_back(*node);
        }
    }
    // 只复制选区内部的连线
    for (const engine::Edge& edge : graph.edges) {
        const bool from_selected = std::find(selected_nodes.begin(), selected_nodes.end(),
                                             edge.from_node) != selected_nodes.end();
        const bool to_selected   = std::find(selected_nodes.begin(), selected_nodes.end(),
                                             edge.to_node) != selected_nodes.end();
        if (from_selected && to_selected) {
            clipboard_edges_.push_back(edge);
        }
    }

    log::info("复制 " + std::to_string(clipboard_nodes_.size()) + " 个节点到剪贴板");
    set_status("已复制 " + std::to_string(clipboard_nodes_.size()) + " 个节点");
}

bool EditorState::paste_clipboard()
{
    if (clipboard_nodes_.empty()) {
        set_status("剪贴板为空");
        return false;
    }

    snapshot("粘贴节点");

    constexpr float kOffset = 40.0f;
    std::vector<std::pair<std::string, std::string>> id_map; // 原 id → 新 id
    std::vector<std::string> created;

    for (const engine::Node& source : clipboard_nodes_) {
        std::string error;
        const std::string new_id = graph.addNode(source.type, source.x + kOffset,
                                                 source.y + kOffset, &error);
        if (new_id.empty()) {
            log::error("粘贴节点失败: " + error);
            continue;
        }
        if (engine::Node* node = graph.findNode(new_id)) {
            node->title  = source.title;
            node->params = source.params; // 一并复制参数值
        }
        id_map.emplace_back(source.id, new_id);
        created.push_back(new_id);
    }

    for (const engine::Edge& edge : clipboard_edges_) {
        std::string from_id;
        std::string to_id;
        for (const auto& entry : id_map) {
            if (entry.first == edge.from_node) {
                from_id = entry.second;
            }
            if (entry.first == edge.to_node) {
                to_id = entry.second;
            }
        }
        if (from_id.empty() || to_id.empty()) {
            continue;
        }
        std::string error;
        graph.addEdge(from_id, edge.from_port, to_id, edge.to_port, &error);
    }

    selected_nodes = created;
    selected_node  = created.empty() ? std::string() : created.front();
    request_navigate_to_content = true;

    log_summary("[粘贴] 已粘贴 " + std::to_string(created.size()) + " 个节点");
    set_status("已粘贴 " + std::to_string(created.size()) + " 个节点");
    return !created.empty();
}

} // namespace aiwrite::ui
