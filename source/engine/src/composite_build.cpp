#include "composite.h"

#include "graph.h"

#include <algorithm>
#include <string>
#include <vector>

namespace aiwrite::engine {

// ---------------------------------------------------- 编辑器试运行沙箱 ----
// 上级节点编辑器「执行连线」：内部图脱离父工作流也能直接跑
//  * 内部图本身没有数据源（暴露输入是“外部喂进来”的引用）⇒ 为每个暴露输入补一个
//    「测试源节点」并接向它指向的内部输入端口，让运行前校验通过、数据从源端起产出
//  * 源节点类型按端口类型映射：Text/Any→TextInput、Image→ImageInput、Provider→ProviderConfig
//  * 源节点 id 用 "__ce_src_<k>"（不进入画布；内部 n1.. 不与它冲突）
//  * 幂等无关：每次「执行连线」都新建沙箱，不改动规格内部图
const char* test_source_type_for(engine::PortType type)
{
    switch (type) {
    case engine::PortType::Image:    return "ImageInput";
    case engine::PortType::Provider: return "ProviderConfig";
    case engine::PortType::Text:     return "TextInput";
    case engine::PortType::Any:      return "TextInput";
    case engine::PortType::Number:   return "TextInput"; // 兜底（当前节点集没有 Number 输入端口）
    }
    return "TextInput";
}

bool buildCompositeTestGraph(const CompositeSpec& spec, Graph* out, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    if (out == nullptr) {
        return fail("输出图指针为空");
    }
    if (spec.id.empty()) {
        return fail("上级节点规格 id 为空：无法构建试运行图");
    }

    *out = spec.internal_graph; // 深拷贝内部图（节点/边/参数原样保留）
    out->refreshSequences();    // 防御：确保后续 addNode/addEdge 的 id 不与内部 n1.. 冲突

    for (std::size_t k = 0; k < spec.inputs.size(); ++k) {
        const CompositePortRef& ref = spec.inputs[k];
        const Node*             target = out->findNode(ref.internal_node);
        if (target == nullptr) {
            continue; // 规格校验已保证存在；防御性跳过
        }
        const Port* port = target->findPort(ref.internal_port, PortDirection::Input);
        if (port == nullptr) {
            continue;
        }
        // 目标端口已有内部入边 ⇒ 不是真正的外部输入（规格校验已禁止；防御性跳过）
        if (out->findEdgeIntoInput(ref.internal_node, ref.internal_port) != nullptr) {
            continue;
        }

        const char* src_type = test_source_type_for(port->type);

        std::string       add_error;
        const std::string raw_id =
            out->addNode(src_type, target->x - 240.0f, target->y, &add_error);
        if (raw_id.empty()) {
            return fail("添加测试源节点失败: " + add_error);
        }
        Node* src = out->findNode(raw_id);
        if (src == nullptr) {
            return fail("测试源节点查找失败: " + raw_id);
        }
        src->id    = "__ce_src_" + std::to_string(k + 1);
        src->title = "测试输入 " + std::to_string(k + 1) + "（暴露输入 " +
                     (ref.display_name.empty() ? ref.internal_port : ref.display_name) + "）";
        // Number 端口兜底：把源输出端口类型改为目标类型，保证 canConnect 通过
        if (!src->outputs.empty() && src->outputs[0].type != port->type &&
            port->type != PortType::Any) {
            src->outputs[0].type = port->type;
        }
        const std::string out_port = src->outputs.empty() ? std::string() : src->outputs[0].id;
        if (out_port.empty()) {
            return fail("测试源节点没有输出端口: " + std::string(src_type));
        }
        const std::string edge_id =
            out->addEdge(src->id, out_port, ref.internal_node, ref.internal_port, &add_error);
        if (edge_id.empty()) {
            return fail("连接测试源失败: " + add_error);
        }
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

// ------------------------------------------------------------------ 打包 ----
// ARCH-E6：打包下沉 —— 主画布「打包为上级节点」的规格构建从 UI 层（ce_pack.cpp）迁入
// engine 单一函数；与 validate_composite_spec 同源（§4 验收判据 4）。
//  * 自动修正：暴露输入端口有内部入边（variadic 端口内外并存）时，该内部边不纳入
//    内部图 —— 外部输入在展开时接管该端口 ⇒ 生成产物必然通过严格校验（问题 2）。
bool buildCompositeSpecFromSelection(const Graph& source, const std::vector<std::string>& selected_nodes,
                                     const std::string& name, CompositeSpec* out, std::string* error)
{
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };
    if (out == nullptr) {
        return fail("输出规格指针为空");
    }
    if (selected_nodes.size() < 2) {
        return fail("打包需要至少选中 2 个节点");
    }
    const auto is_selected = [&selected_nodes](const std::string& node_id) {
        return std::find(selected_nodes.begin(), selected_nodes.end(), node_id) != selected_nodes.end();
    };

    CompositeSpec spec;
    spec.name = name.empty() ? "组合节点" : name;

    // ---- 1) internal_graph：选中节点（深拷贝，保持 id 与坐标）+ 选区内部连线 ----
    for (const std::string& nid : selected_nodes) {
        const Node* n = source.findNode(nid);
        if (n == nullptr) {
            continue;
        }
        if (!n->composite_id.empty()) {
            return fail("选区内不能包含上级节点（不支持嵌套）");
        }
        Node copy = *n;
        copy.state         = NodeState::Idle;
        copy.error_message.clear();
        spec.internal_graph.nodes.push_back(std::move(copy));
    }

    // ---- 2) 暴露输入：选区节点的输入端口，有来自选区外的入边 ----
    //  （variadic 端口可与内部入边并存 ⇒ 生成阶段落实「暴露输入不得有内部入边」：
    //    指向暴露输入端口的内边不纳入内部图 = 打包-校验同源）
    std::vector<CompositePortRef> inputs;
    std::vector<std::pair<std::string, std::string>> exposed_in_ports; // (internal_node, internal_port)
    for (const Node& n : spec.internal_graph.nodes) {
        for (const Port& p : n.inputs) {
            bool exposed = false;
            for (const Edge& e : source.edges) {
                if (e.to_node == n.id && e.to_port == p.id && !is_selected(e.from_node)) {
                    exposed = true;
                    break;
                }
            }
            if (!exposed) {
                continue;
            }
            CompositePortRef ref;
            ref.internal_node = n.id;
            ref.internal_port = p.id;
            ref.display_name  = p.display_name;
            ref.type          = p.type;
            inputs.push_back(std::move(ref));
            exposed_in_ports.emplace_back(n.id, p.id);
        }
    }

    for (const Edge& edge : source.edges) {
        if (!is_selected(edge.from_node) || !is_selected(edge.to_node)) {
            continue; // 只有两端都在选区内的边才进内部图
        }
        // 暴露输入端口的内部入边不纳入（外部输入在展开时接管该端口；问题 2 自动修正）
        const bool targets_exposed_input = std::any_of(
            exposed_in_ports.begin(), exposed_in_ports.end(),
            [&edge](const std::pair<std::string, std::string>& kv) {
                return kv.first == edge.to_node && kv.second == edge.to_port;
            });
        if (targets_exposed_input) {
            continue;
        }
        spec.internal_graph.edges.push_back(edge);
    }
    spec.internal_graph.refreshSequences();
    spec.inputs = std::move(inputs);

    // ---- 3) 暴露输出：边界输出端口（唯一；编辑器可再调整）----
    {
        std::vector<CompositePortRef> boundary;
        for (const Node& n : spec.internal_graph.nodes) {
            for (const Port& p : n.outputs) {
                for (const Edge& e : source.edges) {
                    if (e.from_node == n.id && e.from_port == p.id && !is_selected(e.to_node)) {
                        CompositePortRef ref;
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
            for (const Node& n : spec.internal_graph.nodes) {
                if (n.outputs.empty()) {
                    continue;
                }
                const Port& p = n.outputs.front();
                CompositePortRef ref;
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
    for (const Node& n : spec.internal_graph.nodes) {
        for (const Param& p : n.params) {
            if (p.is_secret) {
                continue; // 密钥（如 API Key）不暴露
            }
            const bool dup = std::any_of(spec.exposed_params.begin(), spec.exposed_params.end(),
                                         [&p](const CompositeParamRef& r) {
                                             return r.param_id == p.id;
                                         });
            if (dup) {
                continue; // 外部参数 id 唯一：重复的只暴露首个
            }
            CompositeParamRef ref;
            ref.internal_node = n.id;
            ref.param_id      = p.id;
            ref.display_name  = p.display_name;
            spec.exposed_params.push_back(std::move(ref));
        }
    }

    *out = std::move(spec);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}



// ------------------------------------------------------------ 展开执行 ----
// CN-实施决策：在执行器工作副本上**就地展开**（见设计文档 §7.2「展开执行」）
//  * 展开发生在 start/startFrom 之内 ⇒ 同步/异步两条路径天然共用
//  * 展开后图中不再有 Composite 节点 ⇒ 无需注册 Composite 执行器
//  * 运行前校验在展开后的图上执行 ⇒ 内部必填端口未接会被正确拦截
bool expand_composites(Graph& graph,
                       std::unordered_map<std::string, std::vector<std::string>>* replaced_out,
                       std::string* error)
{
    CompositeRegistry& registry = CompositeRegistry::instance();
    // 节点快照副本：展开过程会增删 graph.nodes，不能边遍历边改
    const std::vector<Node> original_nodes = graph.nodes;

    for (const Node& composite : original_nodes) {
        if (composite.composite_id.empty()) {
            continue;
        }
        const CompositeSpec* spec = registry.find(composite.composite_id);
        if (spec == nullptr) {
            if (error != nullptr) {
                *error = "[上级节点] 规格不存在: " + composite.composite_id + "（文件 " +
                         registry.root().string() + "\\" + composite.composite_id +
                         ".json 缺失或被移除）";
            }
            return false;
        }

        // ---- 内部节点平移：深拷贝 + 前缀 id（<composite节点id>__<内部节点id>）----
        std::vector<std::string> internal_ids;
        internal_ids.reserve(spec->internal_graph.nodes.size());
        for (const Node& internal : spec->internal_graph.nodes) {
            Node copy = internal;
            copy.id  = composite.id + "__" + internal.id;
            copy.x += 16.0f; // 轻微偏移：仅调试视觉，不影响执行
            copy.y += 16.0f;
            copy.state = NodeState::Idle;
            copy.error_message.clear();
            graph.nodes.push_back(std::move(copy));
            internal_ids.push_back(composite.id + "__" + internal.id);
        }

        // ---- 暴露参数覆盖内部参数值（外部实例值优先）----
        for (const Param& external_param : composite.params) {
            for (const CompositeParamRef& ref : spec->exposed_params) {
                if (ref.param_id != external_param.id) {
                    continue;
                }
                Node*  target = graph.findNode(composite.id + "__" + ref.internal_node);
                Param* target_param = (target != nullptr) ? target->findParam(external_param.id)
                                                          : nullptr;
                if (target_param != nullptr) {
                    target_param->value = external_param.value;
                }
                break;
            }
        }

        // ---- 内部边平移 ----
        for (const Edge& edge : spec->internal_graph.edges) {
            Edge copy       = edge;
            copy.id         = composite.id + "__" + edge.id;
            copy.from_node  = composite.id + "__" + edge.from_node;
            copy.to_node    = composite.id + "__" + edge.to_node;
            copy.is_valid   = true;
            graph.edges.push_back(std::move(copy));
        }

        // ---- 外部输入边 → 内部标记输入端口 ----
        // 外部输入端口 id = in1…inN，按 spec.inputs 顺序一一对应
        for (std::size_t k = 0; k < spec->inputs.size(); ++k) {
            const std::string       ext_port = "in" + std::to_string(k + 1);
            const CompositePortRef& ref      = spec->inputs[k];
            const std::string       int_node = composite.id + "__" + ref.internal_node;
            for (Edge& edge : graph.edges) {
                if (edge.to_node == composite.id && edge.to_port == ext_port) {
                    edge.to_node = int_node;
                    edge.to_port = ref.internal_port;
                }
            }
        }

        // ---- 外部输出边 ← 内部标记输出端口 ----
        for (std::size_t k = 0; k < spec->outputs.size(); ++k) {
            const std::string       ext_port = "out" + std::to_string(k + 1);
            const CompositePortRef& ref      = spec->outputs[k];
            const std::string       int_node = composite.id + "__" + ref.internal_node;
            for (Edge& edge : graph.edges) {
                if (edge.from_node == composite.id && edge.from_port == ext_port) {
                    edge.from_node = int_node;
                    edge.from_port = ref.internal_port;
                }
            }
        }

        // ---- 移除 Composite 占位节点（残余连线已全部重接/内部化）----
        graph.removeNode(composite.id);

        if (replaced_out != nullptr) {
            (*replaced_out)[composite.id] = std::move(internal_ids);
        }
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}


} // namespace aiwrite::engine
