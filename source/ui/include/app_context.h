#pragma once

// ============================================================================
//  应用级服务统一容器（ARCH-A1）
//
//  架构落点：ARCHITECTURE.md §1.8 `AppContext` / §1.8.11 依赖图 / 决策 ARCH-A1
//   * 收口 Config / Clipboard / Diagnostics / RecentFiles / AutosaveService /
//     WorkflowFile 六个应用级服务，供 EditorState 编排、app.cpp 主循环读取
//   * **构造顺序**（成员声明顺序 = 构造顺序）：
//       config / clipboard / diagnostics（无依赖）→
//       autosave / workflow_file（依赖 WorkflowModel&）
//   * **为什么由 EditorState 持有而不是外部组装注入**（决策 ARCH-A1）：
//     AutosaveService 与 WorkflowFile 依赖文档级 `WorkflowModel`（§1.8.11 依赖图），
//     而 WorkflowModel 在 A1 阶段仍由 EditorState 持有（`model_`，§1.8.3 过渡登记）——
//     若由 app.cpp 先构造 AppContext 再传入 EditorState，则 AppContext 无法在
//     `model_` 之后拿到其引用（构造顺序鸡生蛋）。故本批 AppContext 作为 EditorState
//     成员（`app_ctx_`，声明在 `model_` 之后）；随 ARCH-A3 `editor()` 退场后迁到
//     app/main 层外部注入（届时 UI 模块直接持有 WorkflowModel）。
// ============================================================================

#include "autosave_service.h"
#include "clipboard.h"
#include "config.h"
#include "diagnostics_service.h"
#include "recent_files_service.h"
#include "workflow_file.h"

namespace aiwrite::ui {

struct AppContext {
    // model = 文档级工作流模型（EditorState::model_；A3 后由外部注入）
    explicit AppContext(WorkflowModel& model);

    Config          config;        // 配置（app.cpp 启动 loadConfig 灌入；UI 逻辑读它）
    Clipboard       clipboard;     // 复制/粘贴剪贴板（纯数据 + 注入）
    Diagnostics     diagnostics;   // 诊断（异常计数 / 内存采样 / 工作线程结论通道）
    AutosaveService autosave;      // 自动保存（订阅编辑事件 → 节流写快照）
    WorkflowFile    workflow_file; // 工作流文件（保存/打开/最近/归档；含 RecentFiles）
};

} // namespace aiwrite::ui
