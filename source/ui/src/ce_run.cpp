#include "composite_editor.h"
#include "ce_internal.h"
#include "composite.h"
#include "editor_controller.h"
#include "log.h"
#include "nodes.h"
#include "validate.h"

#include <string>
#include <vector>

namespace aiwrite::ui {

// ==================================================== 试运行「执行连线」====
// 与主画布同机制：独立执行器在**沙箱副本**（内部图 + 暴露输入测试源）上推进；
// 主线程每帧 composite_editor_tick_run() 泵事件 → 回写内部图节点状态 / 结果读模型。
//  * 运行态值不进入规格 / 撤销快照 / JSON（同主画布「运行态不进 Graph」原则）
//  * 测试源节点（__ce_src_k）只存在于沙箱，不回写画布
namespace {

bool ce_is_test_source(const std::string& node_id)
{
    return node_id.rfind("__ce_src_", 0) == 0;
}

// 取/建 run_snapshot 中某内部节点的条目（只记录内部节点；测试源不进读模型）
engine::RunNodeView* ce_snapshot_entry(CeSession& session, const std::string& node_id)
{
    for (engine::RunNodeView& view : session.run_snapshot.nodes) {
        if (view.node_id == node_id) {
            return &view;
        }
    }
    engine::RunNodeView view;
    view.node_id = node_id;
    if (const engine::Node* node = session.model().graph().findNode(node_id)) {
        view.type = node->type;
    }
    session.run_snapshot.nodes.push_back(std::move(view));
    return &session.run_snapshot.nodes.back();
}

} // namespace

bool compositeEditorStartRun(std::string* error, EditorController& editor)
{
    CeSession& session = ceSession();
    const auto fail    = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };
    if (!session.active) {
        return fail("上级节点编辑器未打开");
    }
    if (session.run_active) {
        return true; // 已在运行（幂等）
    }
    // 与主画布运行互斥：避免两个执行器并发打 HTTP / 抢画布状态色
    if (editor.session_active()) {
        return fail("主画布正在运行：请先停止主画布的运行，再执行本规格");
    }

    aiwrite::nodes::registerAllExecutors(); // 幂等

    engine::Graph sandbox;
    std::string   build_error;
    if (!engine::buildCompositeTestGraph(session.model().spec(), &sandbox, &build_error)) {
        return fail("构建试运行图失败: " + build_error);
    }

    engine::ValidationMessages errors;
    engine::ValidationMessages warnings;
    if (!engine::validateBeforeRun(sandbox, &errors, &warnings)) {
        for (const std::string& text : errors) {
            log::error("[上级节点试运行校验] " + text);
        }
        return fail("运行被拒绝：" + (errors.empty() ? std::string("未知原因") : errors.front()));
    }
    for (const std::string& text : warnings) {
        log::warn("[上级节点试运行校验] " + text);
    }

    std::string start_error;
    // 丢弃上一会话残留事件（stop 后未泵的事件不清零，重跑前必须排空）
    {
        std::vector<engine::RunEvent> stale;
        session.run_executor.pumpEvents(&stale);
    }
    if (!session.run_executor.startAsync(&sandbox, &start_error)) {
        return fail("启动执行器失败: " + start_error);
    }

    session.run_snapshot         = engine::RunSnapshot{}; // 新会话：清空读模型
    session.run_snapshot.running = true;
    session.run_active           = true;
    session.run_status           = "试运行中…";
    // 画布即时反馈：全图节点回到「等待」（执行器对工作副本做了同样重置，事件随后逐节点接管）
    for (engine::Node& node : session.model().graph().nodes) {
        node.state = engine::NodeState::Waiting;
        node.error_message.clear();
    }
    log::info("[上级节点] 试运行开始（规格 " + session.model().spec().id + " · " +
              session.model().spec().name + "），计划 " +
              std::to_string(session.run_executor.totalCount()) + " 个节点");
    return true;
}

void compositeEditorStopRun()
{
    CeSession& session = ceSession();
    if (!session.run_active) {
        return;
    }
    session.run_executor.stopAsync(); // join（幂等；退出 / 切会话前必调）
    session.run_active           = false;
    session.run_snapshot.running = false;
    // 尚未收到 Finished 事件的运行态节点回落 Idle（避免「停止后仍显示运行中」）
    for (engine::Node& node : session.model().graph().nodes) {
        if (node.state == engine::NodeState::Running ||
            node.state == engine::NodeState::Waiting) {
            node.state = engine::NodeState::Idle;
        }
    }
}

void compositeEditorClearRun()
{
    CeSession& session = ceSession();
    if (session.run_active) {
        compositeEditorStopRun();
    }
    session.run_snapshot = engine::RunSnapshot{};
    session.run_status.clear();
    for (engine::Node& node : session.model().graph().nodes) {
        node.state = engine::NodeState::Idle;
        node.error_message.clear();
    }
}

bool compositeEditorRunActive()
{
    return ceSession().run_active;
}

const engine::RunSnapshot& compositeEditorRunSnapshot()
{
    return ceSession().run_snapshot;
}

// 每帧泵事件：沙箱节点状态/输出 → 内部图节点状态 + 结果读模型（测试源不回写）
void compositeEditorTickRun()
{
    CeSession& session = ceSession();
    if (!session.run_active) {
        return;
    }
    std::vector<engine::RunEvent> events;
    session.run_executor.pumpEvents(&events);

    engine::Graph& ig = session.model().graph();
    for (const engine::RunEvent& event : events) {
        switch (event.kind) {
        case engine::RunEvent::Kind::NodeState: {
            if (ce_is_test_source(event.node_id)) {
                break; // 测试源节点不显示在画布
            }
            if (engine::Node* node = ig.findNode(event.node_id)) {
                node->state = event.state; // 主线程写 Graph（唯一写者）
                if (!event.error.empty()) {
                    node->error_message = event.error;
                }
            }
            engine::RunNodeView* view = ce_snapshot_entry(session, event.node_id);
            view->state               = event.state;
            if (!event.error.empty()) {
                view->error = event.error;
            }
            if (event.state == engine::NodeState::Skipped) {
                view->text.clear(); // 跳过 ⇒ 旧结果失效
            }
            break;
        }
        case engine::RunEvent::Kind::NodeOutput: {
            if (ce_is_test_source(event.node_id)) {
                break;
            }
            engine::RunNodeView* view = ce_snapshot_entry(session, event.node_id);
            view->text                = event.text;
            view->state               = event.state;
            view->delta_bytes         = event.text.size();
            break;
        }
        case engine::RunEvent::Kind::Delta: {
            if (ce_is_test_source(event.node_id)) {
                break;
            }
            engine::RunNodeView* view = ce_snapshot_entry(session, event.node_id);
            view->text += event.text;
            view->state       = engine::NodeState::Running;
            view->delta_bytes = view->text.size();
            break;
        }
        case engine::RunEvent::Kind::Console: {
            log::info("[上级节点执行] " + event.text);
            break;
        }
        case engine::RunEvent::Kind::Finished: {
            session.run_snapshot.summary     = event.summary;
            session.run_snapshot.final_state = event.final_state;
            session.run_snapshot.running     = false;
            session.run_executor.stopAsync(); // join 后再读权威数据（本次只用 summary）
            session.run_active = false;
            session.run_status = "运行结束：" + event.summary;
            log::info("[上级节点执行器] " + event.summary);
            break;
        }
        }
    }
}

} // namespace aiwrite::ui
