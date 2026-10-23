#include "app_detail.h"

#include "commands.h"          // handleShortcut（主窗口 Ctrl+S）
#include "composite_editor.h"  // 上级节点编辑器 + 新建/打包对话框
#include "console_panel.h"
#include "error_bar.h"         // 状态栏错误条
#include "node_canvas.h"
#include "node_library.h"
#include "node_registry.h"     // 配置表重载后重建 `provider` 参数枚举
#include "output_panel.h"
#include "property_panel.h"
#include "settings_panel.h"    // 设置面板 + 首启引导的「打开设置面板」
#include "toolbar.h"

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>  // ImGui_ImplOpenGL3_RenderDrawData（出帧）
#include <imgui_internal.h>     // 窗口栈看门狗（ImGuiContext::CurrentWindowStack）

#include <string>

#include "log.h"
#include "paths.h"

namespace aiwrite::ui {

using namespace app_detail;  // 原匿名命名空间函数（布局 / 状态栏文案 / 信息面板 / 首启引导）

// ============================================================================
//  工具栏 + DockSpace + 主窗口 Ctrl+S（原 run() 1190–1217）
// ============================================================================
void appFrameDockSpace(AppFrameContext& frame, const ImGuiViewport* viewport,
                       float status_height)
{
    GLFWwindow*       window                  = frame.window;
    EditorController& state                   = frame.state;
    bool&             layout_ready            = frame.layout_ready;
    bool&             request_layout_reset    = frame.request_layout_reset;
    bool&             request_first_run_guide = frame.request_first_run_guide;
        // ---- 工具栏（全部鼠标操作；设计 §6.7：MVP 不设快捷键）----
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
        if (ImGui::BeginChild("##toolbar", ImVec2(0.0f, ImGui::GetFrameHeight() + 10.0f), false)) {
            drawToolbarButtons(state);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        const ImGuiID dockspace_id = ImGui::GetID("AIwriteDockSpace");
        if (!layout_ready || request_layout_reset) {
            build_default_layout(dockspace_id, viewport->WorkSize);
            layout_ready         = true;
            request_layout_reset = false;
        }
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, -status_height),
                         ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();

    // ARCH-Z1：命令上下文与原文同式构造（原 cmd_ctx 与菜单段同作用域构造一次；
    //           菜单段已搬入 app_frame.cpp ⇒ 此处按字面同式重建，聚合体无副作用）
    const CommandContext cmd_ctx{window, &request_first_run_guide, &state};
        // ---- ARCH-R4：主窗口 Ctrl+S（**修 Ctrl+S**：主窗口此前无保存快捷键）----
        // 快捷键表驱动（CommandScope::Main · SaveWorkflow）；窗口条件 = 不在上级节点
        // 编辑器窗口 —— ce 窗口聚焦/悬停时 Ctrl+S 归 ce（ce_draw 的
        // handleShortcut(CompositeEditor) 处理保存上级节点，此处跳过避免双重触发）。
        // 通用隔离（文本输入 / 激活控件 / 弹层 / 左键按住）在 handleShortcut 内判定；
        // IsKeyChordPressed 同帧只触发一次，与各窗口内检测天然互斥、不会重复。
        handleShortcut(CommandScope::Main,
                       /*window_active=*/!(ceSession().active && compositeEditorWindowFocused()),
                       cmd_ctx,
                       /*only=*/AppCommand::SaveWorkflow);
}

// ============================================================================
//  面板接线（画布 / 节点库 / 参数 / Console / 输出 / 工作流信息 /
//            上级节点编辑器 + 对话框 / 首启引导 + 设置面板）
// ============================================================================
void appFramePanels(AppFrameContext& frame, const ReadOnlyView& view)
{
    Config&                           config                  = frame.config;
    EditorController&                 state                   = frame.state;
    const Fonts&                      fonts                   = frame.fonts;
    CanvasOptions&                    canvas_options          = frame.canvas_options;
    const utils::WebView2RuntimeInfo& webview2                = frame.webview2;
    bool&                             show_library            = frame.show_library;
    bool&                             show_params             = frame.show_params;
    bool&                             show_console            = frame.show_console;
    bool&                             show_output             = frame.show_output;
    bool&                             show_info               = frame.show_info;
    bool&                             show_composite_editor   = frame.show_composite_editor;
    bool&                             request_first_run_guide = frame.request_first_run_guide;
        // ---- 面板 ----
        ImGui::PushFont(fonts.ui);
        drawNodeCanvas(kWindowCanvas, canvas_options, view, state);
        ImGui::PopFont();

        if (show_library) {
            ImGui::PushFont(fonts.ui);
            drawNodeLibrary(kWindowLibrary, &show_library, state);
            ImGui::PopFont();
        }

        if (show_params) {
            ImGui::PushFont(fonts.ui);
            engine::Node* selected =
                state.selection().single().empty()
                    ? nullptr
                    : state.model().graph.findNode(state.selection().single());

            PropertyEditResult edit;
            draw_property_panel(kWindowParams, &show_params, selected, view, edit, state);

            // begin_edit 在控件被激活、值尚未变化时置位 → 此时压快照可正确回退
            if (edit.begin_edit) {
                state.snapshot("修改参数");
            }
            if (edit.changed) {
                state.setStatus("参数已修改（可用工具栏「撤销」回退）");
                // ARCH-E1：参数变化通知（未跟踪具体 key → 负载只带 node_id，通知「有参数变了」）
                if (selected != nullptr) {
                    state.model().event_bus().publish(EditEvent::node(EditEventKind::ParamChanged,
                                                              selected->id));
                }
            }
            if (edit.renamed) {
                state.setStatus("节点标题已修改");
                // ARCH-E1：标题变化通知
                if (selected != nullptr) {
                    state.model().event_bus().publish(EditEvent::node(EditEventKind::TitleChanged,
                                                              selected->id));
                }
            }
            ImGui::PopFont();
        }

        if (show_console) {
            ImGui::PushFont(fonts.console);
            // PA-08：初始高度来自 config.toml [ui] console_height（面板内容区 + 工具条空间）
            drawConsolePanel(kWindowConsole, &show_console, state.console_panel(),
                               static_cast<float>(config.ui.console_height) + 100.0f);
            ImGui::PopFont();
        }
        if (show_output) { // PA-02：输出面板（结果全文 / 复制全文·该节点 / 导出）
            ImGui::PushFont(fonts.ui);
            drawOutputPanel(kWindowOutput, &show_output, view, state);
            ImGui::PopFont();
        }
        if (show_info) {
            ImGui::PushFont(fonts.ui);
            draw_info_panel(kWindowInfo, &show_info, view, state);
            ImGui::PopFont();
        }

        // ---- CN：上级节点编辑器 + 新建/打包对话框 ----
        if (ceSession().active) {
            show_composite_editor = true; // 会话打开 → 自动显示窗口
        }
        if (show_composite_editor) {
            ImGui::PushFont(fonts.ui);
            // ARCH-Z2：每帧构建一次渲染只读视图（只存引用、零拷贝；对称 ReadOnlyView）
            const CeReadOnlyView ce_view(ceSession().model(), ceSession().selection(),
                                         ceSession().run_snapshot, config);
            drawCompositeEditor(kWindowCompositeEditor, &show_composite_editor, ce_view, state);
            ImGui::PopFont();
        }
        if (!ceSession().active) {
            show_composite_editor = false; // 会话关闭 → 隐藏窗口
        }
        drawCompositeDialogs(state);

        // ---- M6：首次使用引导 + 设置面板（未打开时为空操作 / 零副作用）----
        ImGui::PushFont(fonts.ui);
        draw_first_run_guide(config, state, webview2, request_first_run_guide);
        request_first_run_guide = false;
        const SettingsPanelResult settings_result =
            drawSettingsPanel(state.settings_panel(), config);
        ImGui::PopFont();
        if (settings_result.saved) {
            if (settings_result.visibility_changed) {
                // 面板可见性 / 画布选项在 app.cpp 里另有 local 副本 ⇒ 保存后同步
                show_library = config.ui.show_node_library;
                show_params  = config.ui.show_property_panel;
                show_console = config.ui.show_console;
                show_output  = config.ui.show_output_window;
                canvas_options.show_grid         = config.ui.show_grid;
                canvas_options.running_animation = config.ui.running_animation;
            }
            state.setStatus(settings_result.message);
        }
        if (settings_result.provider_table_reloaded) {
            // PB2-24 要点②：配置表重载 ⇒ 重建 `provider` 参数枚举（注册表 Definition + 已有节点实例）
            //  * 不刷新的话：刚新建的站点 id 会在运行前校验被判「取值不在枚举范围内」
            //  * 界面下拉本身读实时表（PB2-31 合并显示）⇒ 无需额外刷新
            engine::refreshProviderOptions(state.model().graph);
            state.setStatus("配置表已重载：提供商下拉 / 运行前校验已同步");
        }
}

// ============================================================================
//  状态栏（原 run() 1325–1374）
// ============================================================================
void appFrameStatusBar(AppFrameContext& frame, const ReadOnlyView& view,
                       const ImGuiViewport* viewport, float status_height)
{
    EditorController& state = frame.state;
    const Fonts&      fonts = frame.fonts;
    ImGuiIO&          io    = frame.io;
        // ---- 状态栏 ----
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
                                       viewport->WorkPos.y + viewport->WorkSize.y - status_height));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, status_height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 2.0f));
        if (ImGui::Begin("##StatusBar", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
            ImGui::PushFont(fonts.console);
            ImGui::Text("节点 %d  连线 %d  选中 %d  |  %.1f FPS",
                        static_cast<int>(view.nodes_for_render().size()),
                        static_cast<int>(view.edges_for_render().size()),
                        static_cast<int>(view.selection.nodes().size()), io.Framerate);
            ImGui::SameLine();
            // P7a-18：状态栏四项 —— 提供商 / 模式（含站点）/ 最后耗时（+ 节点统计）
            ImGui::TextDisabled("  %s", provider_summary_text(view).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("  %s", provider_mode_text(view).c_str());
            const std::string duration_text = last_run_duration_text(view);
            if (!duration_text.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("  %s", duration_text.c_str());
            }
            const std::string run_text = state.runStatusText();
            // ARCH-R2：状态栏订阅 RunEvent —— 运行中/刚结束时文本仍直接读 executor（实时秒表），
            //  但 executor 被 reset（切图/退出）后，改用 Finished 事件缓存的最后摘要（run_last_summary_）
            const std::string run_text_final =
                !run_text.empty() ? run_text
                                  : (state.run_finished_ ? state.run_last_summary_ : std::string());
            if (!run_text_final.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.40f, 0.72f, 1.0f, 1.0f), "  %s",
                                   run_text_final.c_str());
            }
            if (!state.status.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "  %s", state.status.c_str());
            }
            if (state.last_error.active) { // PA-06：失败运行 → 红色错误条（点击定位节点）
                ImGui::SameLine();
                drawErrorBar(state);
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "  日志: %s",
                               log::filePathString().c_str());
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
}

// ============================================================================
//  窗口栈看门狗 + 出帧（原 run() 1376–1407）
// ============================================================================
void appFrameEnd(AppFrameContext& frame)
{
    GLFWwindow*       window      = frame.window;
    const AppOptions& options     = frame.options;
    int&              frame_index = frame.frame_index;
        // [调试·窗口栈看门狗] 在 Render()（内部 EndFrame() 断言窗口栈）之前快照。
        //  * 常开：栈深≠1 时写入 windowstack_crash.txt（工作目录）+ app 日志 —— 这就是
        //    断言触发帧，栈顶即「没配 End/EndChild 的窗口」。
        //  * --auto-open-composite 时前 40 帧每帧都记录，便于观察编辑会话首帧前后窗口栈演变。
        {
            ImGuiContext* const watchdog_ctx = ImGui::GetCurrentContext();
            const int stack_size = (watchdog_ctx != nullptr) ? watchdog_ctx->CurrentWindowStack.Size : 0;
            const bool stack_bad = stack_size != 1;
            if (stack_bad || (options.auto_open_composite && frame_index <= 40)) {
                const std::string watchdog_dump = window_stack_dump();
                if (FILE* watchdog_file = std::fopen("windowstack_crash.txt", "a");
                    watchdog_file != nullptr) {
                    std::fprintf(watchdog_file, "=== 帧 %d ===\n%s", frame_index,
                                 watchdog_dump.c_str());
                    std::fclose(watchdog_file);
                }
                if (stack_bad) {
                    log::error("[窗口栈看门狗] 帧 " + std::to_string(frame_index) +
                               "：\n" + watchdog_dump);
                }
            }
        }

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
}
}  // namespace aiwrite::ui
