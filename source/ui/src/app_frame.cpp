#include "app_detail.h"

#include "app_version.h"       // kAppVersionLabel（帮助 → 关于）
#include "commands.h"          // CommandContext / dispatchCommand（菜单三入口统一路由）
#include "composite_editor.h"  // ceSession() / --auto-open-composite 复现入口
#include "texture_cache.h"     // state.textures().beginFrame()

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <chrono>
#include <string>
#include <thread>

#include "log.h"
#include "paths.h"             // paths::configFile（几何 / 可见性 / 恢复询问落盘）

namespace aiwrite::ui {

using namespace app_detail;  // 原匿名命名空间函数（窗口栈看门狗 dump）

// ============================================================================
//  帧首推进（原 run() 主循环体 762–890）
//  * 窗口几何节流保存 → 执行会话推进 → 自动保存节流 → 后台诊断结论 →
//    诊断心跳/内存采样 → 最小化跳过 → NewFrame/纹理标记 → --auto-open-composite 复现
//  * 返回 false = 原 `continue;`（窗口最小化：sleep 10ms 后跳过本帧）
// ============================================================================
bool appFrameBegin(AppFrameContext& frame)
{
    GLFWwindow*       window      = frame.window;
    Config&           config      = frame.config;
    EditorController& state       = frame.state;
    const AppOptions& options     = frame.options;
    int&              frame_index = frame.frame_index;

        // ---- 窗口几何节流保存（F3 / PD-04）：移动/缩放后静默 2s 落盘一次（避免拖拽期间频繁写文件）----
        {
            static auto last_geometry_change = std::chrono::steady_clock::now();
            static bool geometry_dirty       = false;
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) == 0) {
                const bool maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
                int        window_x  = 0;
                int        window_y  = 0;
                int        width     = 0;
                int        height    = 0;
                glfwGetWindowPos(window, &window_x, &window_y);
                glfwGetWindowSize(window, &width, &height);

                bool changed = (config.ui.window_maximized != maximized);
                config.ui.window_maximized = maximized;
                if (!maximized) {
                    // 最大化时不覆盖"还原尺寸/位置"，便于下次启动恢复
                    changed = changed || config.ui.window_pos_x != window_x ||
                              config.ui.window_pos_y != window_y || config.ui.window_width != width ||
                              config.ui.window_height != height;
                    config.ui.window_pos_x  = window_x;
                    config.ui.window_pos_y  = window_y;
                    config.ui.window_width  = width;
                    config.ui.window_height = height;
                }

                const auto now = std::chrono::steady_clock::now();
                if (changed) {
                    geometry_dirty       = true;
                    last_geometry_change = now;
                }
                else if (geometry_dirty &&
                         now - last_geometry_change >= std::chrono::seconds(2)) {
                    geometry_dirty = false;
                    if (saveConfig(paths::configFile(), config)) {
                        setAppConfig(config); // PA-07：保持进程缓存与磁盘一致
                        log::info("[窗口几何] 已保存到 config.toml（" + std::to_string(width) + "x" +
                                  std::to_string(height) + " @ " + std::to_string(window_x) + "," +
                                  std::to_string(window_y) + (maximized ? "，最大化=是" : "") + "）");
                    }
                    else {
                        log::warn("[窗口几何] 保存失败：config.toml 不可写？");
                    }
                }
            }
        }

        // ---- 执行会话推进（M2-04：一帧最多推进一个节点；无运行会话时为空操作）----
        state.tickRun();

        // ---- PC-01：自动保存节流（静默 3 秒后写快照；暂停/无变更时零写盘）----
        // ARCH-A2：EditorController autosave 方法已移除 —— 直连 state.autosave()
        // （tick 返回「本次真的写了快照」，据此写状态栏）
        if (state.autosave().tick(state.workflow_file().current_workflow_path())) {
            state.setStatus("已自动保存：未保存的更改已暂存（崩溃/退出后可恢复）");
        }

        // ---- M_patchAB §3.2 / §3.4（PB2-07 / PB2-30②）：后台诊断结论 → 状态栏 ----
        //  * 工作线程只**投递**结论（post_background_status）；此处（主线程）才写 UI 状态
        std::string background_status;
        if (state.takeBackgroundStatus(&background_status)) {
            state.setStatus(background_status);
        }

        // ---- 诊断（限流输出）：放在"最小化检查"之前，保证窗口不可见时也能采样 ----
        //  · 首异常累计（有变化才输出，限流）
        //  · 心跳（每 300 帧）：确认渲染循环真的在跑
        //  · 每 30 秒一次内存采样（工作集 / 私有 / 峰值 + 业务计数）
        // ARCH-A1：诊断归口 state.diagnostics()
        state.diagnostics().log_exception_summary();
        const bool window_iconified = (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0);
        if (frame_index <= 5 || frame_index % 3600 == 0) { // 启动前几帧 + 之后约每 1 分钟
            log::info("[诊断] 心跳 帧=" + std::to_string(frame_index) + " 最小化=" +
                      (window_iconified ? "是" : "否") + " FPS=" +
                      std::to_string(static_cast<int>(ImGui::GetIO().Framerate)));
        }
        static auto last_memory_sample = std::chrono::steady_clock::now();
        const auto diagnostic_now      = std::chrono::steady_clock::now();
        if (diagnostic_now - last_memory_sample >= std::chrono::seconds(30)) {
            last_memory_sample = diagnostic_now;
            const EditorController& diagnostic_state = state;
            state.diagnostics().log_memory_sample(
                "节点 " + std::to_string(diagnostic_state.model().graph.nodes.size()) + " / 连线 " +
                std::to_string(diagnostic_state.model().graph.edges.size()) + " / 撤销栈 " +
                std::to_string(diagnostic_state.model().undo.undoDepth()));
        }

        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
            // 最小化时降低 CPU 占用（不依赖后端辅助函数，保持版本兼容）
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return false;  // 原 `continue;`：最小化 ⇒ 跳过本帧（主循环据返回值 continue）
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // P0（2026-10-09）：纹理缓存「逐帧标记」——本帧用到的纹理本轮不淘汰。
        // 必须紧跟 NewFrame()（在画布/面板开始取纹理之前），见 ui/texture_cache.h 说明。
        state.textures().beginFrame();

        // [调试·自动复现] --auto-open-composite：程序化走「新建上级节点」路径。
        // 第 6 帧打开对话框并在其可见帧自动点「创建并打开编辑器」（与手工同路径）。
        if (options.auto_open_composite && frame_index == 6) {
            compositeEditorTestDriveNewDialog(state);
            log::info("[自动复现] 已触发「新建上级节点」对话框（第 6 帧）");
        }
        // 若新建因规格校验失败而未打开编辑器：第 12 帧再走「内存会话」路径（旧实现），
        // 以判定断言是否来自「无文件支撑的内存会话」（composite_editor_open 的 find 落空回退）。
        if (options.auto_open_composite && frame_index == 12 && !ceSession().active) {
            engine::CompositeSpec fresh;
            fresh.id   = engine::CompositeRegistry::instance().makeId();
            fresh.name = "内存会话复现";
            compositeEditorOpen(fresh.id);
            log::info("[自动复现] 走内存会话路径打开编辑器 " + fresh.id + "（第 12 帧）");
        }
        // 编辑器已打开后：第 16 帧添加一个内部节点（覆盖非空内部图的 BeginNode 绘制路径），
        // 第 24 帧点「保存」（应因未标记输出被严格校验拦截 → 属预期行为，非崩溃）。
        if (options.auto_open_composite && frame_index == 16 && ceSession().active) {
            const std::string nid = compositeEditorAddNode("TextInput", 0.0f, 0.0f);
            log::info("[自动复现] 编辑器内添加内部节点 " + nid + "（第 16 帧）");
        }
        if (options.auto_open_composite && frame_index == 24 && ceSession().active) {
            std::string save_err;
            const bool  saved = compositeEditorSave(&save_err, state);
            log::info(std::string("[自动复现] 编辑器保存（预期失败：未标记输出）ok=") +
                      (saved ? "1" : "0") + "  err=" + save_err + "（第 24 帧）");
        }
    return true;
}

// ============================================================================
//  菜单栏（原 run() 的 DockHost 宿主窗口 + BeginMenuBar 全段 902–1092）
//  * 命令上下文 `cmd_ctx` 与原文同式构造（原同一作用域内构造一次；聚合体无副作用）
// ============================================================================
void appFrameMenuBar(AppFrameContext& frame, const ImGuiViewport* viewport)
{
    GLFWwindow*       window                  = frame.window;
    Config&           config                  = frame.config;
    EditorController& state                   = frame.state;
    bool&             show_library            = frame.show_library;
    bool&             show_params             = frame.show_params;
    bool&             show_console            = frame.show_console;
    bool&             show_output             = frame.show_output;
    bool&             show_info               = frame.show_info;
    bool&             request_layout_reset    = frame.request_layout_reset;
    bool&             request_first_run_guide = frame.request_first_run_guide;
        // ---- 全屏 DockSpace 宿主窗口 ----
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        const ImGuiWindowFlags host_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;

        ImGui::Begin("##DockHost", nullptr, host_flags);
        ImGui::PopStyleVar(3);

        // ARCH-R4：命令路由上下文（承载主循环局部状态：退出窗口 / 首启引导标志）。
        // 菜单 / 工具栏 / 快捷键三入口统一经 dispatchCommand —— 同一实现、行为一致。
        const CommandContext cmd_ctx{window, &request_first_run_guide, &state};

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("文件")) {
                if (ImGui::MenuItem("新建（清空画布）")) {
                    dispatchCommand(AppCommand::NewWorkflow, cmd_ctx);
                }
                if (ImGui::MenuItem("示例工作流")) {
                    dispatchCommand(AppCommand::SampleWorkflow, cmd_ctx);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("打开工作流…")) {
                    dispatchCommand(AppCommand::OpenWorkflow, cmd_ctx);
                }
                if (ImGui::MenuItem("保存工作流…")) {
                    dispatchCommand(AppCommand::SaveWorkflow, cmd_ctx);
                }
                if (ImGui::BeginMenu("最近打开")) {
                    // M_patchAB §4（PC-03 / FEA-M2-02）：打开前**校验存在性** ——
                    // 失效项灰显（不可点）+ 悬停解释 + 底部一键「清理失效条目」
                    // ARCH-A1：最近列表归口 state.workflow_file().recent_files()
                    const std::vector<engine::RecentStatus> recent =
                        state.workflow_file().recent_files().with_status();
                    if (recent.empty()) {
                        ImGui::MenuItem("（暂无记录）", nullptr, false, false);
                    }
                    for (const engine::RecentStatus& status : recent) {
                        const engine::RecentEntry& entry = status.entry;
                        const std::string label = entry.name + "##" + entry.path; // ## 保证 id 唯一
                        if (ImGui::MenuItem(label.c_str(), nullptr, /*selected=*/false,
                                            /*enabled=*/status.exists)) {
                            std::string error;
                            if (!state.openWorkflowFrom(entry.path, &error)) {
                                state.setStatus("打开失败：" + error);
                            }
                        }
                        if (ImGui::IsItemHovered()) {
                            if (status.exists) {
                                ImGui::SetTooltip("%s\n最后打开：%s", entry.path.c_str(),
                                                  entry.opened_at.c_str());
                            }
                            else {
                                ImGui::SetTooltip("%s\n**文件已不存在**（灰显不可点）——\n"
                                                  "可用下方「清理失效条目」把它从列表移除",
                                                  entry.path.c_str());
                            }
                        }
                    }
                    const std::size_t missing = state.workflow_file().recent_files().count_missing();
                    if (missing > 0) {
                        ImGui::Separator();
                        if (ImGui::MenuItem(("清理失效条目（" + std::to_string(missing) + "）").c_str())) {
                            const std::size_t removed = state.workflow_file().recent_files().prune_missing();
                            state.setStatus("已清理 " + std::to_string(removed) + " 条失效的最近记录");
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("清空列表")) {
                        state.workflow_file().recent_files().clear();
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("设置…")) { // M6-01：config.toml 的可调项集中在这里
                    dispatchCommand(AppCommand::Settings, cmd_ctx);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("退出")) {
                    dispatchCommand(AppCommand::Exit, cmd_ctx);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("编辑")) {
                if (ImGui::MenuItem("撤销", nullptr, false, state.model().undo.canUndo())) {
                    dispatchCommand(AppCommand::Undo, cmd_ctx);
                }
                if (ImGui::MenuItem("重做", nullptr, false, state.model().undo.canRedo())) {
                    dispatchCommand(AppCommand::Redo, cmd_ctx);
                }
                ImGui::Separator();
                // 设计 §6.1：复制（节点）/ 粘贴（画布）——菜单入口与右键菜单一致
                if (ImGui::MenuItem("复制", nullptr, false, state.has_selection())) {
                    dispatchCommand(AppCommand::Copy, cmd_ctx);
                }
                if (ImGui::MenuItem("粘贴", nullptr, false, state.has_clipboard())) {
                    dispatchCommand(AppCommand::Paste, cmd_ctx);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("删除选中", nullptr, false, state.has_selection())) {
                    dispatchCommand(AppCommand::DeleteSelection, cmd_ctx);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("视图")) {
                const bool library_toggled = ImGui::MenuItem("节点库", nullptr, &show_library);
                const bool params_toggled  = ImGui::MenuItem("参数面板", nullptr, &show_params);
                const bool console_toggled = ImGui::MenuItem("Console", nullptr, &show_console);
                const bool output_toggled  = ImGui::MenuItem("输出", nullptr, &show_output);
                ImGui::MenuItem("工作流信息", nullptr, &show_info); // 不入 config：仅本次会话生效
                ImGui::Separator();
                if (ImGui::MenuItem("重置布局")) {
                    request_layout_reset = true;
                }
                ImGui::EndMenu();

                // 可见性持久化（设计 §20.2：启动读取 config.toml）——让手动切换被记住；
                // M6-06 设置面板落地后改由设置面板统一保存
                if (library_toggled || params_toggled || console_toggled || output_toggled) {
                    config.ui.show_node_library   = show_library;
                    config.ui.show_property_panel = show_params;
                    config.ui.show_console        = show_console;
                    config.ui.show_output_window  = show_output;
                    if (saveConfig(paths::configFile(), config)) {
                        setAppConfig(config); // PA-07：保持进程缓存与磁盘一致
                        log::info(std::string("界面可见性已保存到 config.toml（节点库=") +
                                  (show_library ? "显示" : "隐藏") + "，参数面板=" +
                                  (show_params ? "显示" : "隐藏") + "，Console=" +
                                  (show_console ? "显示" : "隐藏") + "）");
                    }
                    else {
                        log::warn("界面可见性保存失败：config.toml 不可写？");
                    }
                }
            }

            if (ImGui::BeginMenu("上级节点")) {
                if (ImGui::MenuItem("新建上级节点…")) {
                    dispatchCommand(AppCommand::CompositeNew, cmd_ctx);
                }
                if (ImGui::MenuItem("打包选区为上级节点…", nullptr, false,
                                    state.selection().nodes().size() >= 2)) {
                    dispatchCommand(AppCommand::CompositePack, cmd_ctx);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("在画布上框选/多选至少 2 个节点后打包");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("打开上级节点编辑器")) {
                    // 一律走文件路径：弹新建对话框 → 落盘空白规格 JSON → 打开编辑器读取
                    // （不直接创建无文件支撑的内存会话，避免依赖 imgui-node-editor 上下文时序）
                    dispatchCommand(AppCommand::CompositeOpenEditor, cmd_ctx);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("帮助")) {
                // M6：版本号取自 app_version.h（与诊断报告 / 安装包同源，避免三处不一致）
                ImGui::MenuItem((std::string("关于 AIwrite ") + kAppVersionLabel).c_str(), nullptr,
                                false, false);
                ImGui::Separator();
                if (ImGui::MenuItem("复制诊断信息")) { // PM-04：给非技术用户的一键求助入口
                    dispatchCommand(AppCommand::CopyDiagnostics, cmd_ctx);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("版本 / 系统 / 依赖 / 路径 / 配置 / 站点表摘要\n"
                                      "（**不含**任何密钥或 Cookie 内容）");
                }
                if (ImGui::MenuItem("首次使用引导")) {
                    dispatchCommand(AppCommand::FirstRunGuide, cmd_ctx);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("打开日志目录")) {
                    dispatchCommand(AppCommand::OpenLogDir, cmd_ctx);
                }
                if (ImGui::MenuItem("打开数据目录")) {
                    dispatchCommand(AppCommand::OpenDataDir, cmd_ctx);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
}

// ============================================================================
//  恢复未保存的更改（PC-02 模态；宿主 = DockHost 窗口上下文，OpenPopup 需在同一窗口内）
// ============================================================================
void appFrameRestorePopup(AppFrameContext& frame)
{
    Config&               config                = frame.config;
    EditorController&     state                 = frame.state;
    engine::AutosaveInfo& autosave_info         = frame.autosave_info;
    std::string&          autosave_reason       = frame.autosave_reason;
    bool&                 autosave_ask          = frame.autosave_ask;
    bool&                 autosave_popup_opened = frame.autosave_popup_opened;
        // ---- PC-02：恢复未保存的更改（模态；宿主 = DockHost 窗口上下文，OpenPopup 需在同一窗口内）
        if (autosave_ask) {
            const char* kRestorePopup = "恢复未保存的更改？##autosave_restore";
            if (!autosave_popup_opened) {
                ImGui::OpenPopup(kRestorePopup);
                autosave_popup_opened = true;
            }
            ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal(kRestorePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("上次退出 / 退出前，当前工作流还有未保存的更改。");
                ImGui::Separator();
                ImGui::Text("快照时间：%s", autosave_info.saved_at.empty()
                                                ? "（未知）"
                                                : autosave_info.saved_at.c_str());
                ImGui::Text("工作流名：%s",
                            autosave_info.name.empty() ? "未命名" : autosave_info.name.c_str());
                ImGui::Text("内容：节点 %d / 连线 %d", autosave_info.nodes, autosave_info.edges);
                ImGui::TextWrapped("原文件：%s", autosave_info.workflow_path.empty()
                                                     ? "（快照未关联工作流文件）"
                                                     : autosave_info.workflow_path.c_str());
                ImGui::TextDisabled("判据：%s", autosave_reason.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("「恢复」= 载入快照并另存为新文件（不会覆盖原文件）；"
                                    "「忽略」= 删除该快照，按当前内容继续。");

                const auto finish_without_restore = [&](const char* status_text) {
                    state.autosave().clear_snapshot(); // 用户明确拒绝 ⇒ 快照不再保留（否则下次又问）
                    state.autosave().mark_baseline();
                    state.autosave().pause(false);
                    autosave_ask = false;
                    state.setStatus(status_text);
                    ImGui::CloseCurrentPopup();
                };

                // ARCH-A2：恢复编排随 EditorController::restoreAutosave 一并移除，落点改在 app 层
                //（恢复弹窗所在 = 唯一调用方）。数据操作 = engine::autosaveLoad + model().load()；
                // 路径/基线 = workflow_file + autosave；编排 = abortRunIfAny / selection / 状态栏。
                // 快照信息复用启动时 probe 的 autosave_info（弹窗期间 autosave 暂停，快照不可变）。
                const auto do_restore = [&]() -> bool {
                    engine::Graph loaded;
                    std::string   load_error;
                    if (!engine::autosaveLoad(loaded, &load_error)) {
                        log::error("[自动保存] 恢复失败：" + load_error);
                        state.setStatus("恢复失败：" + load_error);
                        return false;
                    }
                    state.abortRunIfAny("恢复自动保存");
                    // ARCH-E3：整图替换（替换数据 + 清空撤销栈 + 逐节点 diff 事件）归 WorkflowModel::load
                    state.model().load(std::move(loaded));
                    state.selection().clear(); // ARCH-E4：选中独立对象（全清：nodes + links + single）
                    state.request_navigate_to_content = true;
                    // PC-02：**不覆盖原文件** —— 恢复后当前路径留空 ⇒ 保存必然走「另存为」
                    state.workflow_file().set_restored_from_autosave_path(
                        autosave_info.workflow_path);
                    state.workflow_file().set_current_workflow_path("");
                    // ARCH-E2：整图替换通知已在 model().load 内发出（在 baseline 之前发）
                    state.autosave().mark_baseline(); // 恢复内容 = 当前状态（避免立刻再写同一快照）
                    const std::string& restored_path =
                        state.workflow_file().restored_from_autosave_path();
                    log::info("[自动保存] 已恢复快照（节点 " +
                              std::to_string(state.model().graph.nodes.size()) + " / 连线 " +
                              std::to_string(state.model().graph.edges.size()) + "）" +
                              (restored_path.empty() ? "（快照未关联工作流文件）"
                                                     : "（原文件 " + restored_path +
                                                           "，保存将另存为新文件）"));
                    state.setStatus("已恢复未保存的更改（另存为新文件，不会覆盖原工作流）");
                    return true;
                };

                if (ImGui::Button("恢复", ImVec2(160.0f, 0.0f))) {
                    if (do_restore()) {
                        state.autosave().pause(false);
                        autosave_ask = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("忽略（保留当前内容）", ImVec2(200.0f, 0.0f))) {
                    finish_without_restore("已忽略未保存的更改（快照已删除）");
                }
                ImGui::SameLine();
                if (ImGui::Button("忽略，且不再提示", ImVec2(180.0f, 0.0f))) {
                    config.ui.autosave_restore_prompt = false; // PC-02：「可禁用该提示」
                    if (saveConfig(paths::configFile(), config)) {
                        setAppConfig(config);
                        log::info("[自动保存] 已写入 ui.autosave_restore_prompt = false（不再提示恢复）");
                    }
                    else {
                        log::warn("[自动保存] 「不再提示」写盘失败：config.toml 不可写？");
                    }
                    finish_without_restore("已忽略未保存的更改，且不再提示（可在设置面板重新打开）");
                }
                ImGui::EndPopup();
            }
        }
}
}  // namespace aiwrite::ui
