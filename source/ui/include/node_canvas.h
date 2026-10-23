#pragma once

#include <string>
#include <unordered_map>

#include "canvas_view.h" // ARCH-A6：CanvasContext / MinimapState / ed::EditorContext
#include "readonly_view.h"

// ============================================================================
//  节点画布（设计文档 §6.1 / §6.2 / §14）
//
//  * 由 engine::Graph 渲染节点与连线（不再有写死的演示数据）
//  * 鼠标交互：右键画布分类新建、右键节点复制/删除、右键连线删除、
//    单击/Ctrl+单击/框选、拖拽移动、拖出端口连线（类型校验）、双击标题重命名
//  * 所有修改都会通过 EditorController 先压撤销快照
//  * ARCH-R3：渲染读取统一走 `const ReadOnlyView&`（ARCH-D33）；
//    ARCH-A3：交互写走启动注入的 `EditorController&`（不再有全局编辑器单例）
//  * ARCH-A6：画布跨帧状态（编辑器上下文 / 小地图 / 吸附 / 拖拽 / 右键菜单落点）
//    从 11 条文件级 static 收进 `NodeCanvasState`，归属 `EditorController::node_canvas()`
// ============================================================================

namespace aiwrite::ui {

class EditorController;

// ---------------------------------------------------------------------------
//  主画布跨帧状态（ARCH-A6）
//  * 原 `node_canvas.cpp` 的 g_context / g_first_frame / g_drag_in_progress /
//    g_alt_pan_active / g_canvas / g_show_minimap / g_snap_enabled / g_snap_distance /
//    g_minimap / g_prev_positions + 3 条函数内 static（右键菜单落点 / 节点 id / 手柄）
//  * 归属 = `EditorController::node_canvas()`：与窗口同寿，多窗口并按也不互踩。
//  * `g_delete_in_progress`（原声明但**从未使用**）直接删除，不留死状态。
// ---------------------------------------------------------------------------
struct NodeCanvasState {
    ed::EditorContext* context = nullptr; // 主画布编辑器上下文（首帧创建；退出由 app 销毁）
    CanvasContext      canvas;            // 共享绘制层上下文（每帧重填：图 / 主题 / 纹理缓存）

    bool  first_frame      = true;  // 首帧 → NavigateToContent（只做一次）
    bool  drag_in_progress = false; // 一次拖拽只压一次撤销快照
    bool  alt_pan_active   = false; // Alt 平移模式（与吸附 / 拖节点互斥）

    // M_node_expansion L3：会话级辅助开关（不进 config，避免动 settings_validation 断言面）
    bool  show_minimap  = true;
    bool  snap_enabled  = true;
    float snap_distance = 8.0f; // 吸附距离（**屏幕像素**，换算画布坐标用当前缩放）

    MinimapState minimap; // CLN-09：小地图输入屏蔽快照（blocking/clicked/down/mouse）

    // 上一帧节点位置（对齐辅助判断「谁在拖动」：imgui-node-editor 无公开拖动查询 API）
    std::unordered_map<std::string, ImVec2> prev_positions;

    // 右键菜单：新建节点落点 / 当前作用节点 / 命中的节点手柄（跨帧保留）
    ImVec2      spawn_position = ImVec2(0.0f, 0.0f);
    std::string context_node;
    ed::NodeId  context_node_handle;
};

// 画布宿主选项（每帧由 app.cpp 按 config.toml + 本帧统计填/读）
struct CanvasOptions {
    bool show_grid         = true;
    bool running_animation = true; // PA-08：ui.running_animation —— 运行中节点的脉冲动效
    int  node_count        = 0;    // 输出
    int  link_count        = 0;    // 输出
};

void drawNodeCanvas(const char* title, CanvasOptions& options, const ReadOnlyView& view,
                    EditorController& editor);

// 在画布可视中心创建节点（节点库点击 / 粘贴 使用）；返回新节点 id
std::string canvasAddNodeAtCenter(const std::string& type, EditorController& editor);
// CN（Composition_node）：在画布可视中心创建上级节点（依据规格）；返回新节点 id
std::string canvasAddCompositeAtCenter(const std::string& composite_id, EditorController& editor);

} // namespace aiwrite::ui
