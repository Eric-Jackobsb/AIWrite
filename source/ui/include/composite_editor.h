#pragma once

// ============================================================================
//  上级节点（Composition_node）UI 模块
//
//  * 编辑会话：打开 = 载入规格工作副本；保存 = 校验 + 写注册表 + 落盘
//  * 打包：把主画布选中节点打包为上级节点（自动识别边界输入/输出/暴露参数）
//  * 新建：落盘空白规格 JSON → 打开编辑器读取（绕过 imgui 编辑器状态）
//  * 编辑器窗口：内部图画布 + 规格标记面板（对外输入/输出/暴露参数）
// ============================================================================

#include <array>
#include <string>

#include "composite.h"
#include "undo_stack.h"

namespace aiwrite::ui {

// 编辑会话状态（UI 层，跨帧存活；画布上下文在 .cpp 内）
struct CompositeEditorState {
    bool                 active       = false; // 编辑窗口是否打开
    bool                 dirty        = false; // 有未保存更改
    std::string          error;                 // 上次操作错误（展示用）
    bool                 ask_close    = false;  // 关闭确认弹窗
    std::array<char, 256> name_buf{};           // 名称输入缓冲
    std::array<char, 256> desc_buf{};           // 描述输入缓冲
    engine::CompositeSpec working;              // 工作副本（保存时才写注册表）
    engine::UndoStack    undo;                  // 编辑会话内撤销
};

CompositeEditorState& composite_editor_state();

// 打开编辑会话：读取已落盘的 composite_id 规格
//（新建/打包均先落盘 JSON 再走此标准打开路径，不存在「内存未保存会话」）
void composite_editor_open(const std::string& composite_id);
// 请求关闭（有未保存更改时置 ask_close 让用户确认）
void composite_editor_request_close();
void composite_editor_force_close();
// 保存：校验 + 写注册表（同 id 覆盖或新建 id）+ 落盘；成功返回 true
bool composite_editor_save(std::string* error);
// 会话内快照（修改前调用）
void composite_editor_snapshot(const std::string& label);
// 编辑器内撤销/重做
bool composite_editor_can_undo();
bool composite_editor_can_redo();
void composite_editor_undo_once();
void composite_editor_redo_once();

// 编辑会话内创建/删除内部节点、连线
std::string composite_editor_add_node(const std::string& type, float x, float y);
bool        composite_editor_delete_node(const std::string& node_id);
// 会话内连线（校验失败写 error 并返回空）
std::string composite_editor_add_edge(const std::string& from_node, const std::string& from_port,
                                      const std::string& to_node, const std::string& to_port,
                                      std::string* error);

// 打包：把主画布选中节点打包为上级节点
//  * internal_graph = 选中节点 + 选区内部连线
//  * 输入 = 选区边界输入端口（有外部边进入选区）；输出 = 边界输出（唯一，自动挑选）
//  * 暴露参数 = 选中节点全部参数
//  * 成功返回新规格 id 并打开编辑器；失败返回空并写 error
std::string composite_pack_selection(const std::string& name, std::string* error);

// 画布窗口（dock 进主布局；未打开时为空操作）
void draw_composite_editor(const char* title, bool* open);

// 新建/打包名称对话框（主循环每帧调用；内部用 PopupModal）
void draw_composite_dialogs();

// 主画布右键菜单入口：新建上级节点（弹名称框 → 落盘空白规格 JSON → 打开编辑器）
void composite_new_dialog();
// 主画布右键菜单入口：打包选中节点（弹名称框）
void composite_pack_dialog();

// [调试] --auto-open-composite：程序化驱动「新建上级节点」对话框并自动点「创建并打开编辑器」
void composite_editor_test_drive_new_dialog();

} // namespace aiwrite::ui
