#pragma once

// ============================================================================
//  主画布 —— **模块内契约**(ARCH-Z1 第 3 组)
//
//  * 只允许 `ui/src/node_canvas*.cpp` 自己 include；**不是**公开 API（公开入口 = `node_canvas.h`）
//  * 逐字搬迁（零语义变化）：原 1126 行中的 19 个匿名命名空间函数整段搬出；
//    跨 TU 只需 12 个「渲染路径要调用」的入口（其余 7 个 = 内部实现细节，留在 TU1）
//  * 拆分版图：
//      `node_canvas_impl.cpp`  设置净化 + 编辑器上下文 + 交互回调 + 连线/删除/同步 +
//                              新建菜单 + 宽度自检 + Alt 平移 + 逐帧诊断（原 37–732）
//      `node_canvas.cpp`       上级节点辅助（吸附/小地图开关）+ `drawNodeCanvas` 渲染主循环 +
//                              画布中心建节点（原 734–1126）
// ============================================================================

#include "node_canvas.h"        // NodeCanvasState / CanvasContext
#include "editor_controller.h"  // EditorController
#include "graph.h"              // engine::Graph

#include <chrono>

#include <imgui.h>

namespace aiwrite::ui {
namespace canvas_detail {

void ensure_context(NodeCanvasState& nc);
void handle_create_link(EditorController& state);
void handle_delete_items(EditorController& state);
void sync_selection(EditorController& state);
void handle_delete_key(EditorController& editor);
void sync_positions(EditorController& state, NodeCanvasState& nc);
void show_add_node_menu(EditorController& state, const ImVec2& spawn);
void check_uniform_node_width(const engine::Graph& graph, const CanvasContext& canvas);
void log_wrap_selftest(const CanvasContext& canvas);
bool update_alt_left_pan(NodeCanvasState& nc);
double ms_between(const std::chrono::steady_clock::time_point& from,
                  const std::chrono::steady_clock::time_point& to);
void log_canvas_diagnostic(const EditorController& state, float zoom, double width_ms,
                           double nodes_ms, double links_ms);

}  // namespace canvas_detail
}  // namespace aiwrite::ui
