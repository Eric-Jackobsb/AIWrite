#pragma once

// ============================================================================
//  命令路由层（ARCH-R4 · ARCHITECTURE.md §2.1 三入口行为漂移）
//
//  * **AppCommand**：菜单 / 工具栏 / 快捷键三入口的动作全集
//  * **dispatchCommand**：**单一实现** —— 三入口全部收敛到这一个函数
//    （同一动作无论从菜单、工具栏还是快捷键触发，行为完全一致）
//  * **快捷键表**（ShortcutBinding + shortcutTable）：表驱动快捷键分派；
//    主循环 / 画布 / 上级节点编辑器各自调用 handleShortcut 按作用域匹配
//  * **作用域**（CommandScope）：
//      - Main              → 主窗口（Ctrl+S = 保存工作流 · Delete = 删主画布选中）
//      - CompositeEditor   → 上级节点编辑器（Ctrl+S = 保存上级节点 · Delete = 删 ce 选中）
//    作用域隔离由**窗口条件**（调用方在各自窗口上下文内判定传入）保证
//
//  范围边界（本批不收敛，见 audit_arch_r4.md）：
//  * 「视图」菜单（面板可见性切换 + config 持久化）不入命令层 ——
//    仅菜单一个入口（无三入口漂移），且强耦合 app.cpp 局部 config 持久化；
//    由 ARCH-E 系列 / 后续批次承接。
//  * 对话框内部的按钮（打开文件 / 保存文件对话框等）不入命令层（非三入口）。
// ============================================================================

#include <cstddef>

#include <imgui.h> // ImGuiKeyChord / ImGuiKey

struct GLFWwindow; // 前置声明（dispatchCommand(Exit) 用；命令层不依赖 GLFW 头）

namespace aiwrite::ui {

class EditorController;

// ---- 应用命令全集（三入口动作收敛点）----
enum class AppCommand {
    // 文件
    NewWorkflow,      // 新建（清空画布）
    SampleWorkflow,   // 示例工作流
    OpenWorkflow,     // 打开工作流…
    SaveWorkflow,     // 保存工作流…
    Settings,         // 设置…
    Exit,             // 退出

    // 编辑
    Undo,             // 撤销
    Redo,             // 重做
    Copy,             // 复制
    Paste,            // 粘贴
    DeleteSelection,  // 删除选中

    // 运行
    Run,              // 运行
    Stop,             // 停止

    // 上级节点
    CompositeNew,             // 新建上级节点…
    CompositePack,            // 打包选区为上级节点…
    CompositeOpenEditor,      // 打开上级节点编辑器
    CompositeSave,            // 保存上级节点（ce 顶栏按钮 / ce Ctrl+S）
    CompositeDeleteSelection, // 删除上级节点编辑器选中（ce Delete）

    // 帮助
    CopyDiagnostics,  // 复制诊断信息
    OpenLogDir,       // 打开日志目录
    OpenDataDir,      // 打开数据目录
    FirstRunGuide,    // 首次使用引导

    Count,
};

// 分发上下文：承载 app.cpp 主循环的**局部**状态（命令层无法直接访问的变量）
//  * ARCH-A3：编辑器编排层经此**启动注入**（不再有全局编辑器单例）；
//    editor == nullptr ⇒ 依赖编辑器的命令不可用（调用方必须传）
//  * 对话框函数 / compositeEditorState() 等 ui 层接口命令层可直接调用，不进 ctx
struct CommandContext {
    GLFWwindow*       window                  = nullptr; // Exit 用（nullptr = 禁用退出命令）
    bool*             request_first_run_guide = nullptr; // FirstRunGuide 用（nullptr = 禁用该命令）
    EditorController* editor                  = nullptr; // ARCH-A3：编辑/文件命令用
};

// 单一实现：菜单 / 工具栏 / 快捷键统一入口（三入口收敛点）
void dispatchCommand(AppCommand cmd, const CommandContext& ctx);

// ---- 快捷键表（表驱动）----
enum class CommandScope { Main, CompositeEditor };

struct ShortcutBinding {
    AppCommand    command;
    ImGuiKeyChord chord;
    CommandScope  scope; // 生效作用域（窗口条件由调用方判定）
};

// 快捷键表 + 表长（cli_selftest 可静态断言）
const ShortcutBinding* shortcutTable();
std::size_t            shortcutTableCount();

// 表驱动快捷键分派：主循环 / 画布 / 上级节点编辑器每帧在**各自窗口上下文**内调用
//  * 通用隔离（文本输入 / 激活控件 / 弹出层 / 左键按住）内部统一判定
//  * window_active 由调用方在窗口内判定传入（聚焦/悬停）；false = 本帧跳过
//  * only != Count 时只匹配该命令（主循环 Ctrl+S 专用检测用）
//  * 命中 → dispatchCommand（与菜单/工具栏完全同一实现）
void handleShortcut(CommandScope scope, bool window_active, const CommandContext& ctx,
                    AppCommand only = AppCommand::Count);

} // namespace aiwrite::ui
