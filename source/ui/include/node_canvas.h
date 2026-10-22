#pragma once

#include <string>

// ============================================================================
//  节点画布（设计文档 §6.1 / §6.2 / §14）
//
//  * 由 engine::Graph 渲染节点与连线（不再有写死的演示数据）
//  * 鼠标交互：右键画布分类新建、右键节点复制/删除、右键连线删除、
//    单击/Ctrl+单击/框选、拖拽移动、拖出端口连线（类型校验）、双击标题重命名
//  * 所有修改都会通过 EditorState 先压撤销快照
// ============================================================================

namespace aiwrite::ui {

struct CanvasOptions {
    bool show_grid  = true;
    bool running_animation = true; // PA-08：ui.running_animation —— 运行中节点的脉冲动效
    int  node_count = 0;                // 输出
    int  link_count = 0;                // 输出
};

void drawNodeCanvas(const char* title, CanvasOptions& options);

// 在画布可视中心创建节点（节点库点击 / 粘贴 使用）；返回新节点 id
std::string canvasAddNodeAtCenter(const std::string& type);
// CN（Composition_node）：在画布可视中心创建上级节点（依据规格）；返回新节点 id
std::string canvasAddCompositeAtCenter(const std::string& composite_id);

} // namespace aiwrite::ui
