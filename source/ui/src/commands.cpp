#include "commands.h"

#include "editor_state.h"      // EditorState / openWorkflowDialog / saveWorkflowDialog
#include "composite_editor.h"  // 上级节点：会话 / 保存 / 删除选中 / 新建 / 打包对话框
#include "settings_panel.h"    // openSettingsPanel（设置面板）
#include "text_view.h"         // copyText（帮助 → 复制诊断信息）
#include "diagnostics_report.h" // utils::diagnosticsReportText（帮助 → 复制诊断信息）
#include "file_dialog.h"       // utils::openFolder（帮助 → 打开日志/数据目录）
#include "paths.h"
#include "log.h"

#include "interactor.h"       // ARCH-E5：interactorClearSelection（删除后清编辑器内部选中）

#include <GLFW/glfw3.h>        // glfwSetWindowShouldClose（退出命令）

#include <string>
#include <vector>

namespace aiwrite::ui {

// ============================================================================
//  dispatchCommand：三入口**单一实现**
//
//  * 菜单（app.cpp）/ 工具栏（toolbar.cpp）/ 快捷键（handleShortcut → 此处）
//    全部收敛到本函数 —— 同一动作行为完全一致（ARCHITECTURE §2.1 三入口收敛）
//  * 无可用性保护的命令（DeleteSelection 等）在**菜单侧**用 enabled 灰显，
//    在**快捷键侧**由 dispatch 内部前置条件兜底（无选中时静默，与 Delete 键
//    原行为一致：不弹提示、不误删）
// ============================================================================
void dispatchCommand(AppCommand cmd, const CommandContext& ctx)
{
    EditorState& state = editor();

    switch (cmd) {
    // ---- 文件 ----
    case AppCommand::NewWorkflow:
        state.clearWorkflow();
        break;
    case AppCommand::SampleWorkflow:
        state.createSampleWorkflow();
        break;
    case AppCommand::OpenWorkflow:
        openWorkflowDialog();
        break;
    case AppCommand::SaveWorkflow:
        saveWorkflowDialog();
        break;
    case AppCommand::Settings:
        openSettingsPanel();
        break;
    case AppCommand::Exit:
        if (ctx.window != nullptr) {
            glfwSetWindowShouldClose(ctx.window, GLFW_TRUE);
        }
        break;

    // ---- 编辑 ----
    case AppCommand::Undo:
        state.undoOnce();
        break;
    case AppCommand::Redo:
        state.redoOnce();
        break;
    case AppCommand::Copy:
        state.copySelection();
        break;
    case AppCommand::Paste:
        state.pasteClipboard();
        break;
    case AppCommand::DeleteSelection:
        // 快捷键兜底：无选中时静默（菜单/工具栏侧已用 enabled 灰显；与主画布
        // Delete 键原行为一致 —— 无选中按 Delete 不做任何事）
        if (state.has_selection()) {
            state.deleteSelected();
            interactorClearSelection(); // 删除后清空编辑器内部选中，避免残留选中框/状态
        }
        break;

    // ---- 运行 ----
    case AppCommand::Run:
        state.startRunAsync();
        break;
    case AppCommand::Stop:
        state.cancelRun();
        break;

    // ---- 上级节点 ----
    case AppCommand::CompositeNew:
        compositeNewDialog();
        break;
    case AppCommand::CompositePack:
        compositePackDialog();
        break;
    case AppCommand::CompositeOpenEditor:
        // 一律走文件路径：弹新建对话框 → 落盘空白规格 JSON → 打开编辑器读取
        // （不直接创建无文件支撑的内存会话，避免依赖 imgui-node-editor 上下文时序）
        if (!compositeEditorState().active) {
            compositeNewDialog();
        }
        break;
    case AppCommand::CompositeSave: {
        // 与 ce 顶栏「保存」按钮完全同一路径（DEBT-B1：Ctrl+S 与保存按钮一致）
        std::string error;
        if (compositeEditorSave(&error)) {
            log::info("[上级节点] 已保存 " + compositeEditorState().working.id + "（" +
                      compositeEditorState().working.name + "）");
        }
        else {
            log::warn("[上级节点] 保存失败: " + error);
        }
        break;
    }
    case AppCommand::CompositeDeleteSelection:
        // ce 内部图删除（一次批次一次撤销快照）；无选中时内部静默返回
        compositeEditorDeleteSelection();
        break;

    // ---- 帮助 ----
    case AppCommand::CopyDiagnostics:
        copyText(utils::diagnosticsReportText(), "[帮助] 诊断");
        state.setStatus("诊断信息已复制到剪贴板（可直接粘贴给支持人员）");
        break;
    case AppCommand::OpenLogDir:
        if (utils::openFolder(paths::logsDir().string())) {
            state.setStatus("已打开日志目录：" + paths::logsDir().string());
        }
        else {
            state.setStatus("打开日志目录失败（可能尚未创建）");
        }
        break;
    case AppCommand::OpenDataDir:
        if (utils::openFolder(paths::dataRoot().string())) {
            state.setStatus("已打开数据目录：" + paths::dataRoot().string());
        }
        else {
            state.setStatus("打开数据目录失败（可能尚未创建）");
        }
        break;
    case AppCommand::FirstRunGuide:
        if (ctx.request_first_run_guide != nullptr) {
            *ctx.request_first_run_guide = true; // 重置「已看过」的会话内标记
        }
        break;

    case AppCommand::Count:
        break; // 哨兵（不可分派）
    }
}


// ============================================================================
//  快捷键表（表驱动 · ARCH-R4）
//
//  * **作用域隔离**：Delete / Ctrl+S 在 Main 与 CompositeEditor 各有绑定 ——
//    由调用方在**对应窗口上下文**内调用 handleShortcut（窗口聚焦/悬停判定天然隔离）
//  * **修 Ctrl+S**：主窗口 Ctrl+S → SaveWorkflow（此前主窗口无保存快捷键）；
//    ce Ctrl+S → CompositeSave（DEBT-B1 已修，本批收敛进表，与按钮同一路径）
// ============================================================================
const ShortcutBinding kShortcutTable[] = {
    // 主窗口：Ctrl+S = 保存工作流 · Delete = 删主画布选中
    {AppCommand::SaveWorkflow,             ImGuiMod_Ctrl | ImGuiKey_S, CommandScope::Main},
    {AppCommand::DeleteSelection,          ImGuiKey_Delete,            CommandScope::Main},
    // 上级节点编辑器：Ctrl+S = 保存上级节点 · Delete = 删 ce 选中
    {AppCommand::CompositeSave,            ImGuiMod_Ctrl | ImGuiKey_S, CommandScope::CompositeEditor},
    {AppCommand::CompositeDeleteSelection, ImGuiKey_Delete,            CommandScope::CompositeEditor},
};

const ShortcutBinding* shortcutTable() { return kShortcutTable; }

std::size_t shortcutTableCount()
{
    return sizeof(kShortcutTable) / sizeof(kShortcutTable[0]);
}

// ============================================================================
//  handleShortcut：表驱动快捷键分派
//
//  * 通用隔离（与主画布 Delete 键 / ce 快捷键原条件一致）：
//      - 未在输入文本（WantTextInput）
//      - 无 ImGui 控件激活（参数面板改名、节点库搜索、对话框输入不误触）
//      - 无弹出层（右键菜单不误触）
//      - 左键未按住（拖节点 / 框选 / 拉连线过程中不触发）
//  * window_active 由调用方在窗口上下文内判定传入（聚焦/悬停）；false = 跳过
//  * 命中 → dispatchCommand（与菜单/工具栏同一实现）
// ============================================================================
void handleShortcut(CommandScope scope, bool window_active, const CommandContext& ctx,
                    AppCommand only)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        return;
    }
    if (!window_active) {
        return;
    }

    for (std::size_t i = 0; i < shortcutTableCount(); ++i) {
        const ShortcutBinding& binding = kShortcutTable[i];
        if (binding.scope != scope) {
            continue;
        }
        if (only != AppCommand::Count && binding.command != only) {
            continue;
        }
        if (ImGui::IsKeyChordPressed(binding.chord)) {
            dispatchCommand(binding.command, ctx);
            return; // 一次按键只分派一个命令（表内命令互斥）
        }
    }
}

} // namespace aiwrite::ui
