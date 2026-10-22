#include "composite_editor.h"
#include "ce_internal.h"
#include "composite.h"
#include "editor_state.h"

#include <algorithm>
#include <string>
#include <vector>

namespace aiwrite::ui {

// ==================================================== 打包选区为上级节点 ====
//  * internal_graph = 选中节点（深拷贝）+ 选区内部连线
//  * 输入 = 选区边界输入端口（有来自选区外的入边）
//  * 输出 = 边界输出端口（有到选区外的出边）；无边界输出时取选区第一个输出端口
//  * 暴露参数 = 选中节点全部非密钥参数（外部参数 id = 内部参数 id，重复取首）
std::string compositePackSelection(const std::string& name, std::string* error)
{
    EditorState& state = editor();

    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return std::string();
    };

    if (state.selected_nodes.size() < 2) {
        return fail("打包需要至少选中 2 个节点");
    }
    const auto is_selected = [&state](const std::string& node_id) {
        return std::find(state.selected_nodes.begin(), state.selected_nodes.end(), node_id) !=
               state.selected_nodes.end();
    };

    engine::CompositeSpec spec;
    spec.name = name.empty() ? "组合节点" : name;

    // ---- 1) internal_graph：选中节点（深拷贝，保持 id 与坐标）+ 选区内部连线 ----
    for (const std::string& nid : state.selected_nodes) {
        const engine::Node* n = state.model().graph.findNode(nid);
        if (n == nullptr) {
            continue;
        }
        if (!n->composite_id.empty()) {
            return fail("选区内不能包含上级节点（不支持嵌套）");
        }
        engine::Node copy = *n;
        copy.state         = engine::NodeState::Idle;
        copy.error_message.clear();
        spec.internal_graph.nodes.push_back(std::move(copy));
    }
    for (const engine::Edge& edge : state.model().graph.edges) {
        if (is_selected(edge.from_node) && is_selected(edge.to_node)) {
            spec.internal_graph.edges.push_back(edge);
        }
    }
    spec.internal_graph.refreshSequences();

    // ---- 2) 暴露输入：选区节点的输入端口，有来自选区外的入边 ----
    for (const engine::Node& n : spec.internal_graph.nodes) {
        for (const engine::Port& p : n.inputs) {
            bool exposed = false;
            for (const engine::Edge& e : state.model().graph.edges) {
                if (e.to_node == n.id && e.to_port == p.id && !is_selected(e.from_node)) {
                    exposed = true;
                    break;
                }
            }
            if (!exposed) {
                continue;
            }
            engine::CompositePortRef ref;
            ref.internal_node = n.id;
            ref.internal_port = p.id;
            ref.display_name  = p.display_name;
            ref.type          = p.type;
            spec.inputs.push_back(std::move(ref));
        }
    }

    // ---- 3) 暴露输出：边界输出端口（唯一；编辑器可再调整）----
    {
        std::vector<engine::CompositePortRef> boundary;
        for (const engine::Node& n : spec.internal_graph.nodes) {
            for (const engine::Port& p : n.outputs) {
                for (const engine::Edge& e : state.model().graph.edges) {
                    if (e.from_node == n.id && e.from_port == p.id && !is_selected(e.to_node)) {
                        engine::CompositePortRef ref;
                        ref.internal_node = n.id;
                        ref.internal_port = p.id;
                        ref.display_name  = p.display_name;
                        ref.type          = p.type;
                        boundary.push_back(std::move(ref));
                        break;
                    }
                }
            }
        }
        if (boundary.empty()) {
            // 无边界输出：退而取选区第一个输出端口（若选区整体是端到端内部流程）
            for (const engine::Node& n : spec.internal_graph.nodes) {
                if (n.outputs.empty()) {
                    continue;
                }
                const engine::Port& p = n.outputs.front();
                engine::CompositePortRef ref;
                ref.internal_node = n.id;
                ref.internal_port = p.id;
                ref.display_name  = p.display_name;
                ref.type          = p.type;
                boundary.push_back(std::move(ref));
                break;
            }
        }
        if (boundary.empty()) {
            return fail("选区内没有任何可对外输出的端口（至少需要一个输出端口）");
        }
        spec.outputs.push_back(std::move(boundary.front()));
    }

    // ---- 4) 暴露参数：选中节点全部非密钥参数（外部参数 id = 内部参数 id）----
    for (const engine::Node& n : spec.internal_graph.nodes) {
        for (const engine::Param& p : n.params) {
            if (p.is_secret) {
                continue; // 密钥（如 API Key）不暴露
            }
            const bool dup = std::any_of(spec.exposed_params.begin(), spec.exposed_params.end(),
                                         [&p](const engine::CompositeParamRef& r) {
                                             return r.param_id == p.id;
                                         });
            if (dup) {
                continue; // 外部参数 id 唯一：重复的只暴露首个
            }
            engine::CompositeParamRef ref;
            ref.internal_node = n.id;
            ref.param_id      = p.id;
            ref.display_name  = p.display_name;
            spec.exposed_params.push_back(std::move(ref));
        }
    }

    // ---- 5) 校验 + 落盘 + 打开编辑器 ----
    spec.id = engine::CompositeRegistry::instance().makeId();
    std::string save_error;
    if (!engine::CompositeRegistry::instance().save(spec, &save_error)) {
        return fail("打包失败：" + save_error);
    }
    compositeEditorOpen(spec.id);
    if (error != nullptr) {
        error->clear();
    }
    return spec.id;
}

} // namespace aiwrite::ui
