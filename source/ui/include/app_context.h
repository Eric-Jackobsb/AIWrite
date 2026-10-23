#pragma once

// ============================================================================
//  应用级聚合（ARCH-A1 收口 → **ARCH-A3 目标形态**）
//
//  架构落点：ARCHITECTURE.md §1.8.10 `AppContext` / §1.8.11 依赖图 / 决策 ARCH-A1 · D14
//   * **启动时**按依赖顺序构造全部长期对象，并**只在启动阶段**把引用交给界面模块；
//     之后界面模块直接使用自己拿到的引用，不再向 `AppContext` 取任何东西（判据 8）。
//   * **不执行逻辑、不渲染、不处理事件、不持有 UI 面板**（§1.8.10 限制）。
//   * 构造顺序 = 成员声明顺序；析构顺序天然为其逆序（§1.8.11 退出顺序：
//     WorkflowFile → AutosaveService（flush 由 app 退出路径显式调用）→ RunSession →
//     SelectionState → WorkflowModel → Diagnostics → Clipboard → Config）。
//   * ARCH-A3：全局 `editor` 单例已移除 —— 本容器的 `editor` 成员是**唯一**编排层
//     实例，由 app/main（启动代码）与 UI 模块之间传递引用。
// ============================================================================

#include "autosave_service.h"
#include "clipboard.h"
#include "config.h"
#include "diagnostics_service.h"
#include "editor_controller.h"
#include "run_session.h"
#include "selection_state.h"
#include "workflow_file.h"
#include "workflow_model.h"

namespace aiwrite::ui {

struct AppContext {
    AppContext();

    // ---- §1.8.11 初始化顺序（成员声明顺序 = 构造顺序）----
    Config          config;        // 2. 配置（无依赖）
    Clipboard       clipboard;     // 3. 剪贴板（无依赖）
    Diagnostics     diagnostics;   // 4. 诊断（无依赖）
    WorkflowModel   model;         // 5. 文档级模型（依赖 EventBus —— 总线为其成员）
    SelectionState  selection;     // 6. 选中状态（无依赖）
    RunSession      run;           // 7. 运行会话（依赖 EventBus，经方法参数取图）
    AutosaveService autosave;      // 8. 自动保存（依赖 EventBus + WorkflowModel）
    WorkflowFile    workflow_file; // 9. 工作流文件（依赖 EventBus + WorkflowModel，含 RecentFiles）

    // 编排层（ARCH-A3）：只持上列对象的引用，不拥有任何数据；启动后与 UI 模块共享
    EditorController editor;
};

} // namespace aiwrite::ui
