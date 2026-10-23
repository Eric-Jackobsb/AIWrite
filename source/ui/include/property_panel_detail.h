#pragma once

// ============================================================================
//  参数面板 —— **模块内契约**(ARCH-Z1 第 3 组)
//
//  * 只允许 `ui/src/property_panel*.cpp` 自己 include；**不是**公开 API（公开入口 = `property_panel.h`）
//  * 逐字搬迁（零语义变化）：原 855 行 = 单个 693 行巨型面板函数；
//    把尾部「运行状态 + 重跑 + 底部批量操作」整段（原文 639–853）搬成独立入口，
//    调用点、调用时机、同帧内顺序全部不变（仍在同一个 Begin/End 之间、同一个 ImGui 窗口上下文）
//  * 拆分版图：
//      `property_panel.cpp`        输入 / 参数 / 网页版 / 面板诊断段（原 1–638）
//      `property_panel_runtime.cpp` 运行状态 + 重跑 + 批量应用/重置（原 639–853）
// ============================================================================

#include "property_panel.h"     // PropertyEditResult
#include "editor_controller.h"  // EditorController
#include "graph.h"              // engine::Node
#include "readonly_view.h"      // ReadOnlyView

namespace aiwrite::ui {

void draw_property_panel_runtime(engine::Node* node, const ReadOnlyView& view,
                                 PropertyEditResult& result, EditorController& editor);

}  // namespace aiwrite::ui
