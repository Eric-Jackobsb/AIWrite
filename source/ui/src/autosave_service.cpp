#include "autosave_service.h"

#include <algorithm>

#include "autosave.h" // engine::autosave_*（原子写盘 / 探测 / 清理）
#include "config.h"   // appConfig()：进程级配置缓存
#include "log.h"

namespace aiwrite::ui {

namespace {

// ARCH-E2：autosave 订阅的内容类编辑事件（SelectionChanged 非内容变化，不触发写盘）
constexpr EditEventKind kAutosaveEditKinds[] = {
    EditEventKind::NodeAdded,   EditEventKind::NodeRemoved, EditEventKind::NodeMoved,
    EditEventKind::EdgeAdded,   EditEventKind::EdgeRemoved, EditEventKind::ParamChanged,
    EditEventKind::TitleChanged,
};

} // namespace

AutosaveService::AutosaveService(WorkflowModel& model) : model_(model)
{
    // ARCH-E2：订阅内容类编辑事件（替代每帧 1 Hz content signature 轮询；
    // SelectionChanged 非内容变化不订阅；收到事件 → 标记「有未保存变更」）。
    // 挂在文档级总线（model().event_bus()，ARCH-E3 随迁）
    for (const EditEventKind kind : kAutosaveEditKinds) {
        sub_handles_.push_back(
            model_.event_bus().subscribe(kind, [this](const EditEvent&) { on_edit_content(); }));
    }
}

AutosaveService::~AutosaveService()
{
    for (const std::size_t handle : sub_handles_) {
        model_.event_bus().unsubscribe(handle);
    }
}

// 内容类编辑事件回调 —— 任何内容类编辑事件 → 标记「有未保存变更」。
//  * 不检查 paused_：暂停期间产生的变更解闸后仍会补写（与 1 Hz 轮询语义等价）
//  * last_change_ 每次收到事件都刷新 ⇒ 持续编辑（如拖拽 NodeMoved）不断后移「静默」时刻
void AutosaveService::on_edit_content()
{
    change_seen_ = true;
    last_change_ = std::chrono::steady_clock::now();
}

void AutosaveService::pause(bool paused)
{
    paused_ = paused;
    if (!paused) {
        // ARCH-E2：解闸后重新计时「静默」（替代 1 Hz 探测重置）—— 解闸前已标记的变更
        // 也要再静默 idle_sec 才写，避免解闸瞬间立刻写盘
        last_change_ = std::chrono::steady_clock::now();
    }
}

void AutosaveService::mark_baseline()
{
    change_seen_ = false;
    last_change_ = std::chrono::steady_clock::now();
    retry_after_ = {};
}

bool AutosaveService::tick(const std::string& workflow_path)
{
    if (paused_) {
        return false;
    }
    const Config& config = appConfig();
    if (!config.ui.autosave_enabled) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < retry_after_) {
        return false; // 写失败退避中
    }
    if (!change_seen_) {
        return false; // ARCH-E2：事件驱动 —— 没有编辑事件 ⇒ 无变更 ⇒ 不写盘
    }

    const int idle_seconds = std::max(1, config.ui.autosave_idle_sec);
    if (now - last_change_ < std::chrono::seconds(idle_seconds)) {
        return false; // 静默判据（持续编辑 ⇒ last_change_ 不断后移 = 「静默」语义）
    }

    engine::AutosaveInfo info;
    std::string          error;
    if (engine::autosave_write(model_.graph, workflow_path, config.ui.autosave_keep, &info,
                               &error)) {
        change_seen_ = false;
        log::info("[自动保存] 已写入快照（节点 " + std::to_string(model_.graph.nodes.size()) +
                  " / 连线 " + std::to_string(model_.graph.edges.size()) +
                  (workflow_path.empty() ? "，未命名工作流" : "，关联 " + workflow_path) + "）");
        return true;
    }

    log::warn("[自动保存] 写入失败：" + error + "（10 秒后重试）");
    retry_after_ = now + std::chrono::seconds(10);
    return false;
}

void AutosaveService::flush(const std::string& workflow_path)
{
    if (paused_) {
        return;
    }
    const Config& config = appConfig();
    if (!config.ui.autosave_enabled) {
        return;
    }
    if (!change_seen_) {
        return; // ARCH-E2：没有未保存的更改（无编辑事件）⇒ 不写盘
    }

    engine::AutosaveInfo info;
    std::string          error;
    if (engine::autosave_write(model_.graph, workflow_path, config.ui.autosave_keep, &info,
                               &error)) {
        log::info("[自动保存] 退出前已补写快照（下次启动会询问是否恢复未保存的更改）");
    }
    else {
        log::warn("[自动保存] 退出前补写失败：" + error);
    }
}

void AutosaveService::clear_snapshot()
{
    std::string error;
    engine::autosaveClear(&error);
    if (!error.empty()) {
        log::warn("[自动保存] 清空旧快照失败：" + error);
    }
}

} // namespace aiwrite::ui
