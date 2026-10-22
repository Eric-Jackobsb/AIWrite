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
#include "executor.h"
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

    // ---- 试运行「执行连线」（与主画布同机制）----
    //  * 独立执行器在**沙箱副本**（内部图 + 暴露输入测试源）上推进，事件回写内部图节点状态/结果
    //  * 运行态值只存执行器与 run_snapshot，不进规格/撤销快照/JSON
    engine::Executor    run_executor;           // 试运行执行器（工作线程跑沙箱副本）
    engine::RunSnapshot run_snapshot;           // 内部节点运行结果（读模型；UI 只读它）
    bool                run_active   = false;   // 试运行进行中
    std::string         run_status;             // 试运行摘要 / 上次结果（顶栏展示）
};

CompositeEditorState& compositeEditorState();

// 打开编辑会话：读取已落盘的 composite_id 规格
//（新建/打包均先落盘 JSON 再走此标准打开路径，不存在「内存未保存会话」）
void compositeEditorOpen(const std::string& composite_id);
// 请求关闭（有未保存更改时置 ask_close 让用户确认）
void compositeEditorRequestClose();
void compositeEditorForceClose();
// 保存：校验 + 写注册表（同 id 覆盖或新建 id）+ 落盘；成功返回 true
bool compositeEditorSave(std::string* error);
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
bool compositeEditorStartRun(std::string* error);
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
std::string compositePackSelection(const std::string& name, std::string* error);

// 画布窗口（dock 进主布局；未打开时为空操作）
void drawCompositeEditor(const char* title, bool* open);

// 新建/打包名称对话框（主循环每帧调用；内部用 PopupModal）
void drawCompositeDialogs();

// 主画布右键菜单入口：新建上级节点（弹名称框 → 落盘空白规格 JSON → 打开编辑器）
void compositeNewDialog();
// 主画布右键菜单入口：打包选中节点（弹名称框）
void compositePackDialog();

// [调试] --auto-open-composite：程序化驱动「新建上级节点」对话框并自动点「创建并打开编辑器」
void compositeEditorTestDriveNewDialog();

} // namespace aiwrite::ui
