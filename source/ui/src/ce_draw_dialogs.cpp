#include "composite_editor.h"

#include "ce_internal.h"   // CN：上级节点会话内部契约（CompositeDialogsState / ceSession）
#include "commands.h"      // ARCH-R4：命令路由层（对话框按钮与菜单同路）
#include "editor_controller.h"
#include "log.h"
#include "node_registry.h"
#include "nodes.h"         // 新建对话框的节点清单
#include "string_util.h"

#include <imgui.h>

#include <cctype>
#include <string>
#include <vector>

namespace aiwrite::ui {

namespace ed = ax::NodeEditor;  // 原文件级别名（对话框段不使用 ed::，保留以维持原文一致性）

// ==================================================== 新建 / 打包对话框 ====
namespace {

// 对话框状态（开关 / 名称输入缓冲 / 展示用错误文案 / 调试自动确认）见
// `CompositeDialogsState`（composite_editor.h · ARCH-A6）：原先是 7 条文件级 static，
// 现在由 `EditorController::composite_dialogs()` 持有并逐帧传入。

// 点「创建并打开编辑器」的动作：组装空白规格 → 落盘 JSON → 走标准「打开」路径读取。
// （不创建无文件支撑的内存会话，避免依赖 imgui-node-editor 上下文的时序。）
// 返回 true = 已创建并打开（调用方应关闭对话框）；false = 失败（dlg.new_error 已填）。
bool create_and_open_new_composite(CompositeDialogsState& dlg)
{
    dlg.new_error.clear();
    std::string name = dlg.new_name;
    if (name.empty()) {
        name = "未命名上级节点";
    }
    engine::CompositeSpec spec;
    spec.id   = engine::CompositeRegistry::instance().makeId();
    spec.name = name;
    std::string save_error;
    // 新建 = 落盘「空白草稿」：跳过「内部图为空 / 未标记输出」校验（用户随后在编辑器里
    // 添加内部节点、标记输出；点「保存」时再走严格校验）。此前严格校验导致空白规格
    // 永远无法落盘 → 「创建并打开编辑器」只报错、编辑器永不打开（见 CN 校验与 2026-10 修复）。
    if (!engine::CompositeRegistry::instance().save(spec, &save_error,
                                                    /*allow_incomplete_draft=*/true)) {
        dlg.new_error = "创建失败：" + save_error;
        return false; // 保留弹窗展示错误，不关闭
    }
    compositeEditorOpen(spec.id);
    return true;
}

void run_new_dialog(CompositeDialogsState& dlg)
{
    if (!dlg.new_open) {
        return;
    }
    ImGui::OpenPopup("新建上级节点##ce_new");
    if (!ImGui::BeginPopupModal("新建上级节点##ce_new", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }
    if (dlg.new_auto_accept) {
        dlg.new_auto_accept = false;
        if (create_and_open_new_composite(dlg)) {
            ImGui::CloseCurrentPopup();
            dlg.new_open = false;
        }
    }
    ImGui::TextWrapped("新建一个空的上级节点：先在本地落盘一份空白规格 JSON，随后打开编辑器"
                       "添加内部节点、标记端口与参数。");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("名称", dlg.new_name, sizeof(dlg.new_name));
    bool closed = false;
    if (ImGui::Button("创建并打开编辑器", ImVec2(180.0f, 0.0f))) {
        closed = create_and_open_new_composite(dlg);
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(80.0f, 0.0f))) {
        closed = true;
    }
    if (!dlg.new_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", dlg.new_error.c_str());
    }
    if (closed) {
        ImGui::CloseCurrentPopup();
        dlg.new_open = false;
    }
    ImGui::EndPopup();
}

void run_pack_dialog(EditorController& editor, CompositeDialogsState& dlg)
{
    if (!dlg.pack_open) {
        return;
    }
    ImGui::OpenPopup("打包为上级节点##ce_pack");
    if (!ImGui::BeginPopupModal("打包为上级节点##ce_pack", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }
    const int count = static_cast<int>(editor.selection().nodes().size()); // ARCH-E4
    ImGui::TextWrapped("把选中的 %d 个节点打包为上级节点。\n边界输入/输出、暴露参数将自动识别（可在编辑器调整）。",
                       count);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("名称", dlg.pack_name, sizeof(dlg.pack_name));

    if (ImGui::Button("打包", ImVec2(100.0f, 0.0f))) {
        dlg.pack_error.clear();
        const std::string id =
            compositePackSelection(std::string(dlg.pack_name), &dlg.pack_error, editor);
        if (!id.empty()) {
            ImGui::CloseCurrentPopup();
            dlg.pack_open = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(80.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
        dlg.pack_open = false;
    }
    if (!dlg.pack_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", dlg.pack_error.c_str());
    }
    ImGui::EndPopup();
}

} // namespace

void compositeNewDialog(EditorController& editor)
{
    CompositeDialogsState& dlg = editor.composite_dialogs(); // ARCH-A6：对话框状态
    if (!dlg.new_open) {
        dlg.new_open = true;
        dlg.new_name[0]     = '\0';
        dlg.new_error.clear(); // 重新打开时清空上次失败原因
    }
}

// [调试] --auto-open-composite：打开「新建上级节点」对话框，并在其首次可见帧
// 自动执行「创建并打开编辑器」点击（与手工操作同路径，便于复现窗口栈断言）。
void compositeEditorTestDriveNewDialog(EditorController& editor)
{
    CompositeDialogsState& dlg = editor.composite_dialogs(); // ARCH-A6：对话框状态
    if (!dlg.new_open) {
        dlg.new_open        = true;
        dlg.new_name[0]            = '\0';
        dlg.new_error.clear();
        dlg.new_auto_accept = true;
    }
}

void compositePackDialog(EditorController& editor)
{
    CompositeDialogsState& dlg = editor.composite_dialogs(); // ARCH-A6：对话框状态
    if (!dlg.pack_open) {
        dlg.pack_open = true;
        dlg.pack_name[0]     = '\0';
        dlg.pack_error.clear(); // 重新打开时清空上次失败原因
        if (editor.selection().nodes().size() >= 2) { // ARCH-E4：选中独立对象
            std::snprintf(dlg.pack_name, sizeof(dlg.pack_name), "组合节点");
        }
        else {
            ceSession().error = "请先在画布上选中至少 2 个节点再打包";
            dlg.pack_open             = false;
        }
    }
}

void drawCompositeDialogs(EditorController& editor)
{
    CompositeDialogsState& dlg = editor.composite_dialogs(); // ARCH-A6：对话框状态
    run_new_dialog(dlg);
    run_pack_dialog(editor, dlg);
}


} // namespace aiwrite::ui
