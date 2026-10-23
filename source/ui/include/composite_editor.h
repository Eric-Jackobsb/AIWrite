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

#include "ce_session.h" // ARCH-Z2：CeSession / CeModel / CeFile / CeSelection / CeSpec / CeReadOnlyView
#include "composite.h"
#include "executor.h"

namespace aiwrite::ui {

class EditorController; // ARCH-A3：主画布编排层（启动注入；本模块不拥有）

// ---------------------------------------------------------------------------
//  ARCH-Z2：原 `CompositeEditorState`（会话单例结构）已对称拆分为 5 个对象 ——
//  `CeModel`（规格工作副本 + 撤销栈）· `CeFile`（打开/保存）· `CeSelection`（内部图选中）·
//  `CeSpec`（规格标记）· `CeSession`（会话门面，`ceSession()` 取唯一实例）。
//  字段归属与主编辑器一致：WorkflowModel / WorkflowFile / SelectionState ↔ 前四个。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  ARCH-A6：新建 / 打包对话框的跨帧状态（原 `ce_draw.cpp` 7 条文件级 static）
//  * 归属 = `EditorController::composite_dialogs()`（与窗口同寿；多窗口并按不互踩）
//  * 只放「界面临时状态」：对话框开关 / 名称输入缓冲 / 展示用错误文案
// ---------------------------------------------------------------------------
struct CompositeDialogsState {
    bool        new_open        = false; // 「新建上级节点」对话框是否打开
    char        new_name[128]   = {};    // 名称输入缓冲
    std::string new_error;               // 创建失败原因（打开时清空）
    // [调试] --auto-open-composite：对话框可见后自动执行一次「创建并打开编辑器」
    bool new_auto_accept = false;

    bool        pack_open     = false; // 「打包为上级节点」对话框是否打开
    char        pack_name[128] = {};   // 名称输入缓冲
    std::string pack_error;            // 打包失败原因（打开时清空）
};

// 打开编辑会话：读取已落盘的 composite_id 规格
//（新建/打包均先落盘 JSON 再走此标准打开路径，不存在「内存未保存会话」）
void compositeEditorOpen(const std::string& composite_id);
// 请求关闭（有未保存更改时置 ask_close 让用户确认）
void compositeEditorRequestClose();
void compositeEditorForceClose();
// 保存：校验 + 写注册表（同 id 覆盖或新建 id）+ 落盘；成功返回 true
// ARCH-A3：热重载同步主画布实例需要主编辑器（启动注入）
bool compositeEditorSave(std::string* error, EditorController& main_editor);
// 会话内快照（修改前调用）
void compositeEditorSnapshot(const std::string& label);
// 编辑器内撤销/重做
bool compositeEditorCanUndo();
bool compositeEditorCanRedo();
void compositeEditorUndoOnce();
void compositeEditorRedoOnce();

// 编辑会话内创建/删除内部节点、连线
std::string compositeEditorAddNode(const std::string& type, float x, float y);
bool        compositeEditorDeleteNode(const std::string& node_id);
// 会话内连线（校验失败写 error 并返回空）
std::string composite_editor_add_edge(const std::string& from_node, const std::string& from_port,
                                      const std::string& to_node, const std::string& to_port,
                                      std::string* error);

// 试运行「执行连线」：构建沙箱图（内部图 + 暴露输入测试源）→ 运行前校验 → 异步执行
//  * 与主画布运行互斥（主画布会话活动时拒绝启动）；失败写 error 并返回 false
//  * ARCH-A3：主画布会话状态来自启动注入的 EditorController（不再有全局编辑器单例）
bool compositeEditorStartRun(std::string* error, EditorController& editor);
// 停止试运行（join，幂等；关闭编辑器 / 切会话前必调）
void compositeEditorStopRun();
// 清空运行结果（停止 + 重置内部节点状态 + 清空读模型；不压撤销快照）
void compositeEditorClearRun();
// 试运行是否进行中
bool compositeEditorRunActive();
// 每帧泵事件：把沙箱节点状态/结果映射回内部图（draw_composite_editor 内调用）
void compositeEditorTickRun();
// 试运行结果只读快照（画布结果摘要读它）
const engine::RunSnapshot& compositeEditorRunSnapshot();

// 打包：把主画布选中节点打包为上级节点
//  * internal_graph = 选中节点 + 选区内部连线
//  * 输入 = 选区边界输入端口（有外部边进入选区）；输出 = 边界输出（唯一，自动挑选）
//  * 暴露参数 = 选中节点全部参数
//  * 成功返回新规格 id 并打开编辑器；失败返回空并写 error
//  * ARCH-A3：主画布选中/图来自启动注入的 EditorController（不再有全局编辑器单例）
std::string compositePackSelection(const std::string& name, std::string* error,
                                   EditorController& editor);

// 画布窗口（dock 进主布局；未打开时为空操作）
// ARCH-A3：试运行互斥判定与快捷键命令需要主编辑器（启动注入）
// ARCH-Z2：渲染只读 `CeReadOnlyView`（§4 判据 7 · 对称 ReadOnlyView）；
// 视图由 app.cpp 主循环每帧构建一次（只存引用、零拷贝），写路径仍经 ceSession()
void drawCompositeEditor(const char* title, bool* open, const CeReadOnlyView& view,
                          EditorController& editor);
// ce 窗口当前是否聚焦/悬停（ARCH-R4：主循环据此判断 Ctrl+S 作用域归属；
// 由 ce_draw 每帧在窗口上下文内更新）
bool compositeEditorWindowFocused();
// 删除选中的内部节点/连线（ARCH-R4：ce Delete 快捷键 → 命令路由层单一实现；
// 一次批次只压一次撤销快照；无选中时静默返回）
void compositeEditorDeleteSelection();

// 新建/打包名称对话框（主循环每帧调用；内部用 PopupModal）
// ARCH-A3：打包需要主画布选中（启动注入）
void drawCompositeDialogs(EditorController& editor);

// 主画布右键菜单入口：新建上级节点（弹名称框 → 落盘空白规格 JSON → 打开编辑器）
void compositeNewDialog(EditorController& editor);
// 主画布右键菜单入口：打包选中节点（弹名称框）
// ARCH-A3：需要主画布选中（启动注入）
void compositePackDialog(EditorController& editor);

// [调试] --auto-open-composite：程序化驱动「新建上级节点」对话框并自动点「创建并打开编辑器」
void compositeEditorTestDriveNewDialog(EditorController& editor);

} // namespace aiwrite::ui
