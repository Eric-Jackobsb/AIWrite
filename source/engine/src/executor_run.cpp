#include "executor.h"

#include "composite.h"
#include "validate.h"

#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace aiwrite::engine {

namespace {

// PA-01：节点耗时（毫秒）
double ms_since(const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

// PA-04：耗时文本（<100ms 保留两位小数，便于看清本地节点的瞬时耗时）
std::string format_ms(double ms)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(ms < 100.0 ? 2 : 0) << ms << " ms";
    return stream.str();
}

} // namespace
// 节点输出文本（引擎侧唯一实现；ui::node_output_text 委托到它）
std::string nodeOutputText(const Graph& graph, const NodeOutputs& outputs, const std::string& node_id)
{
    const Node* node = graph.findNode(node_id);
    if (node == nullptr) {
        return {};
    }

    std::string text;
    for (const Port& port : node->outputs) {
        const nlohmann::json* value = outputs.find(node_id, port.id);
        if (value == nullptr) {
            continue;
        }
        std::string item;
        if (value->is_string()) {
            item = value->get<std::string>();
        }
        else if (!value->is_null()) {
            item = value->dump();
        }
        if (item.empty()) {
            continue;
        }
        if (!text.empty()) {
            text += "\n";
        }
        if (node->outputs.size() > 1) {
            text += port.display_name + "：";
        }
        text += item;
    }
    if (text.empty()) {
        // M_textio P5：汇点节点（无输出端口）的正文存放在虚拟端口 __result
        if (const nlohmann::json* value = outputs.find(node_id, "__result")) {
            if (value->is_string()) {
                text = value->get<std::string>();
            }
            else if (!value->is_null()) {
                text = value->dump();
            }
        }
    }
    return text;
}

const RunNodeView* RunSnapshot::find(const std::string& node_id) const
{
    for (const RunNodeView& view : nodes) {
        if (view.node_id == node_id) {
            return &view;
        }
    }
    return nullptr;
}

namespace {

// M5-03：收集节点 Image 端口的运行结果（本地图片路径，按端口顺序）
//  * 普通节点：Image 类型的输出端口（如 ImageInput.image）
//  * 汇点节点（TextOutput）：结果存在虚拟端口 __result（见 executeNode）
std::vector<std::string> node_image_paths(const Graph& graph, const NodeOutputs& outputs,
                                          const std::string& node_id)
{
    std::vector<std::string> images;
    const Node*              node = graph.findNode(node_id);
    if (node == nullptr) {
        return images;
    }
    const auto collect = [&images, &outputs, &node_id](const std::string& port_id) {
        const nlohmann::json* value = outputs.find(node_id, port_id);
        if (value == nullptr) {
            return;
        }
        if (value->is_string()) {
            images.push_back(value->get<std::string>());
            return;
        }
        if (value->is_array()) {
            for (const nlohmann::json& item : *value) {
                if (item.is_string()) {
                    images.push_back(item.get<std::string>());
                }
            }
        }
    };
    bool has_image_input = false;
    for (const Port& port : node->inputs) {
        if (port.type == PortType::Image) {
            has_image_input = true;
            break;
        }
    }
    for (const Port& port : node->outputs) {
        if (port.type == PortType::Image) {
            collect(port.id);
        }
    }
    if (has_image_input && node->outputs.empty()) {
        collect("__result"); // TextOutput 等汇点节点
    }
    return images;
}

} // namespace

RunSnapshot makeSnapshot(const Graph& graph, const Executor& executor)
{
    RunSnapshot snapshot;
    snapshot.summary     = executor.summary();
    snapshot.running     = executor.running();
    snapshot.final_state = executor.state();
    for (const NodeRunInfo& info : executor.runInfos()) {
        RunNodeView view;
        view.node_id     = info.node_id;
        view.type        = info.type;
        view.state       = info.state;
        view.duration_ms = info.duration_ms;
        view.error       = info.error;
        view.text        = nodeOutputText(graph, executor.outputs(), info.node_id);
        view.delta_bytes = view.text.size();
        view.images      = node_image_paths(graph, executor.outputs(), info.node_id); // M5-03
        snapshot.nodes.push_back(std::move(view));
    }
    return snapshot;
}

bool Executor::runToCompletion(Graph* graph, int max_steps)
{
    if (state_ != ExecState::Running) {
        return false;
    }
    int steps = 0;
    while (state_ == ExecState::Running && steps < max_steps) {
        if (!tick(graph)) {
            break;
        }
        ++steps;
    }
    if (state_ == ExecState::Running) {
        ctx_.console("[执行器] 超过最大步数 " + std::to_string(max_steps) + "，已中止");
        state_    = ExecState::Failed;
        end_time_ = std::chrono::steady_clock::now();
        return false;
    }
    return state_ == ExecState::Finished;
}

bool Executor::executeNode(Graph& graph, const std::string& node_id)
{
    Node* node = graph.findNode(node_id);
    if (node == nullptr) {
        return false;
    }

    node->state = NodeState::Running;
    node->error_message.clear();
    current_node_ = node_id;
    notifyState(node_id, NodeState::Running);
    ctx_.graph           = &graph;
    ctx_.current_node_id = node_id;
    ctx_.console("[" + node_id + "] 开始执行（" + node->type + " · " + node->title + "）");
    const auto started = std::chrono::steady_clock::now(); // PA-01：节点耗时起点

    try {
        const nlohmann::json inputs = collectInputs(graph, *node);
        const nlohmann::json params = paramObject(*node);
        const NodeExecutor*  executor = NodeExecutorRegistry::instance().find(node->type);
        if (executor == nullptr) {
            throw NodeError("节点类型缺少执行实现: " + node->type);
        }

        nlohmann::json output = (*executor)(inputs, params, ctx_);

        // 约定：单输出节点 → 返回值即该端口的值；多输出节点 → {"端口id": 值}
        //  * M_node_expansion L2（TextStats / TextExtract 为**双输出**）：修复分支顺序 ——
        //    原先「非 null ⇒ 存 __result」会截胡多输出节点（现有多输出节点首次出现才暴露）
        if (node->outputs.size() == 1) {
            outputs_.set(node_id, node->outputs[0].id, std::move(output));
        }
        else if (node->outputs.empty()) {
            // M_textio P5：汇点节点（TextOutput，无输出端口）也要把返回值
            // 存入运行态值（虚拟端口 __result）—— 否则面板 / 归档 / 预览都读不到正文
            outputs_.set(node_id, "__result", std::move(output));
        }
        else {
            if (!output.is_object()) {
                throw NodeError("多输出节点的执行结果必须是 {\"端口id\": 值} 对象");
            }
            for (const Port& port : node->outputs) {
                if (output.contains(port.id)) {
                    outputs_.set(node_id, port.id, output[port.id]);
                }
            }
        }

        node->state = NodeState::Done;
        notifyState(node_id, NodeState::Done);
        ++finished_;
        const double done_ms = ms_since(started);
        recordRun(*node, NodeState::Done, done_ms, {});
        ctx_.console("[" + node_id + "] 完成（" + format_ms(done_ms) + "）");
        current_node_.clear();
        return true;
    }
    catch (const std::exception& ex) {
        // ---- M_patchAB §2.1（PB-02 真取消）：取消**不算失败** ----
        //  * 取消导致的异常 ⇒ 节点记 `Skipped` + 取消记录（含耗时），会话由下一次 tick 翻 Cancelled
        //  * 只改归因与统计口径，不改任何既有失败路径
        if (ctx_.is_cancelled()) {
            const double cancelled_ms = ms_since(started);
            node->state               = NodeState::Skipped;
            node->error_message       = "已取消（用户点了「停止」）：" + std::string(ex.what());
            notifyState(node_id, NodeState::Skipped);
            ++skipped_;
            recordRun(*node, NodeState::Skipped, cancelled_ms, node->error_message);
            ctx_.console("[取消] " + node_id + " 已中断（" + format_ms(cancelled_ms) + "）：" +
                         node->error_message);
            markDownstreamSkipped(graph, node_id);
            current_node_.clear();
            return true; // 让 tick 顶部把会话翻成 Cancelled（不再执行后续节点）
        }
        node->state         = NodeState::Error;
        node->error_message = ex.what();
        notifyState(node_id, NodeState::Error);
        ++failed_;
        const double failed_ms = ms_since(started);
        recordRun(*node, NodeState::Error, failed_ms, node->error_message);
        ctx_.console("[" + node_id + "] 失败（" + format_ms(failed_ms) + "）：" + node->error_message);
        markDownstreamSkipped(graph, node_id);
        current_node_.clear();
        return false;
    }
}

} // namespace aiwrite::engine
