#include "ce_session.h"

#include <algorithm>

namespace aiwrite::ui {

// =============================================================== CeSpec ====
// 规格标记域（ce 独有 · ARCH-Z2）：对外输入 / 对外输出（唯一）/ 暴露参数（param_id 全局唯一）
// 的查询与增删，以及外部显示名编辑缓冲。
//  * 原实现散在 ce_draw.cpp 的规格面板里（直接改 working.inputs / outputs / exposed_params），
//    现收敛为本对象的单一实现：渲染只调用 has_* 查询，修改只经 toggle_* / set_output。
//  * 显示名缓冲原为 `ce_name_buffers()` 函数内 static → 现为本对象成员（规则 6）。
namespace {

bool port_ref_matches(const engine::CompositePortRef& ref, const std::string& node_id,
                      const std::string& port_id)
{
    return ref.internal_node == node_id && ref.internal_port == port_id;
}

bool param_ref_matches(const engine::CompositeParamRef& ref, const std::string& node_id,
                       const std::string& param_id)
{
    return ref.internal_node == node_id && ref.param_id == param_id;
}

} // namespace

bool CeSpec::has_input(const std::string& node_id, const std::string& port_id) const
{
    const std::vector<engine::CompositePortRef>& refs = model_.spec().inputs;
    return std::any_of(refs.begin(), refs.end(), [&](const engine::CompositePortRef& ref) {
        return port_ref_matches(ref, node_id, port_id);
    });
}

bool CeSpec::has_output(const std::string& node_id, const std::string& port_id) const
{
    const std::vector<engine::CompositePortRef>& refs = model_.spec().outputs;
    return std::any_of(refs.begin(), refs.end(), [&](const engine::CompositePortRef& ref) {
        return port_ref_matches(ref, node_id, port_id);
    });
}

bool CeSpec::has_exposed(const std::string& node_id, const std::string& param_id) const
{
    const std::vector<engine::CompositeParamRef>& refs = model_.spec().exposed_params;
    return std::any_of(refs.begin(), refs.end(), [&](const engine::CompositeParamRef& ref) {
        return param_ref_matches(ref, node_id, param_id);
    });
}

bool CeSpec::param_id_conflict(const std::string& node_id, const std::string& param_id) const
{
    const std::vector<engine::CompositeParamRef>& refs = model_.spec().exposed_params;
    return std::any_of(refs.begin(), refs.end(), [&](const engine::CompositeParamRef& ref) {
        return ref.param_id == param_id && ref.internal_node != node_id;
    });
}

void CeSpec::toggle_input(const std::string& node_id, const std::string& port_id, bool expose)
{
    engine::CompositeSpec& spec = model_.spec();
    spec.inputs.erase(std::remove_if(spec.inputs.begin(), spec.inputs.end(),
                                     [&](const engine::CompositePortRef& ref) {
                                         return port_ref_matches(ref, node_id, port_id);
                                     }),
                      spec.inputs.end());
    if (!expose) {
        return;
    }
    // 端口显示名 / 类型取自内部图（勾选瞬间的快照值；之后可改外部显示名）
    const engine::Node* node = model_.graph().findNode(node_id);
    const engine::Port* port =
        (node != nullptr) ? node->findPort(port_id, engine::PortDirection::Input) : nullptr;
    engine::CompositePortRef ref;
    ref.internal_node = node_id;
    ref.internal_port = port_id;
    if (port != nullptr) {
        ref.display_name = port->display_name;
        ref.type         = port->type;
    }
    spec.inputs.push_back(std::move(ref));
}

void CeSpec::set_output(const std::string& node_id, const std::string& port_id)
{
    engine::CompositeSpec& spec = model_.spec();
    spec.outputs.clear(); // 对外输出唯一（单选语义）
    const engine::Node* node = model_.graph().findNode(node_id);
    const engine::Port* port =
        (node != nullptr) ? node->findPort(port_id, engine::PortDirection::Output) : nullptr;
    engine::CompositePortRef ref;
    ref.internal_node = node_id;
    ref.internal_port = port_id;
    if (port != nullptr) {
        ref.display_name = port->display_name;
        ref.type         = port->type;
    }
    spec.outputs.push_back(std::move(ref));
}

void CeSpec::toggle_exposed(const std::string& node_id, const std::string& param_id, bool expose)
{
    engine::CompositeSpec& spec = model_.spec();
    spec.exposed_params.erase(std::remove_if(spec.exposed_params.begin(),
                                             spec.exposed_params.end(),
                                             [&](const engine::CompositeParamRef& ref) {
                                                 return param_ref_matches(ref, node_id, param_id);
                                             }),
                              spec.exposed_params.end());
    if (!expose) {
        return;
    }
    const engine::Node*  node  = model_.graph().findNode(node_id);
    const engine::Param* param = (node != nullptr) ? node->findParam(param_id) : nullptr;
    engine::CompositeParamRef ref;
    ref.internal_node = node_id;
    ref.param_id      = param_id;
    if (param != nullptr) {
        ref.display_name = param->display_name;
    }
    spec.exposed_params.push_back(std::move(ref));
}

void CeSpec::drop_node_refs(const std::string& node_id)
{
    engine::CompositeSpec& spec = model_.spec();
    const auto drop_port = [&node_id](std::vector<engine::CompositePortRef>& refs) {
        refs.erase(std::remove_if(refs.begin(), refs.end(),
                                  [&node_id](const engine::CompositePortRef& ref) {
                                      return ref.internal_node == node_id;
                                  }),
                   refs.end());
    };
    drop_port(spec.inputs);
    drop_port(spec.outputs);
    spec.exposed_params.erase(
        std::remove_if(spec.exposed_params.begin(), spec.exposed_params.end(),
                       [&node_id](const engine::CompositeParamRef& ref) {
                           return ref.internal_node == node_id;
                       }),
        spec.exposed_params.end());
}

void CeSpec::clear()
{
    engine::CompositeSpec& spec = model_.spec();
    spec.inputs.clear();
    spec.outputs.clear();
    spec.exposed_params.clear();
}

std::array<char, 128>& CeSpec::name_buffer(const std::string& key, const std::string& initial)
{
    auto it = name_bufs_.find(key);
    if (it == name_bufs_.end()) {
        std::array<char, 128> buf{};
        const std::size_t     n = std::min<std::size_t>(initial.size(), buf.size() - 1);
        initial.copy(buf.data(), n);
        buf[n] = '\0';
        it     = name_bufs_.emplace(key, std::move(buf)).first;
    }
    return it->second;
}

bool CeSpec::set_input_display_name(const std::string& node_id, const std::string& port_id,
                                    const std::string& name)
{
    for (engine::CompositePortRef& ref : model_.spec().inputs) {
        if (port_ref_matches(ref, node_id, port_id)) {
            ref.display_name = name;
            return true;
        }
    }
    return false;
}

bool CeSpec::set_output_display_name(const std::string& node_id, const std::string& port_id,
                                     const std::string& name)
{
    for (engine::CompositePortRef& ref : model_.spec().outputs) {
        if (port_ref_matches(ref, node_id, port_id)) {
            ref.display_name = name;
            return true;
        }
    }
    return false;
}

} // namespace aiwrite::ui
