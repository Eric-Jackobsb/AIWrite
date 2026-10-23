#include "app_detail.h"  // ARCH-Z1：契约头（标题常量 / Fonts / 内部函数 / AppFrameContext）
#include "app.h"

#include "console_panel.h"
#include "composite_editor.h"   // CN：上级节点编辑器 / 打包 / 新建
#include "commands.h"           // ARCH-R4：命令路由层（菜单收敛 + 主窗口 Ctrl+S）
#include "app_context.h"        // ARCH-A3：启动组装（AppContext + EditorController）
#include "editor_controller.h"
#include "error_bar.h"
#include "node_canvas.h"
#include "node_library.h"
#include "output_panel.h"
#include "property_panel.h"
#include "readonly_view.h"      // ARCH-R3：渲染只读视图（主循环统一构建一次）
#include "texture_cache.h"
#include "toolbar.h"
#include "config.h"
#include "autosave.h"     // PC-01/02：自动保存 / 崩溃恢复（engine::autosave_*）
#include "credential.h"
#include "diagnostics.h"
#include "file_dialog.h"   // M6：菜单「帮助 → 打开日志/数据目录」（utils::open_folder）
#include "log.h"
#include "paths.h"
#include "text_view.h"     // M6：copy_text（菜单「帮助 → 复制诊断信息」）
#include "window_geometry.h"
#include "session_store.h"

#include "recent_files.h"
#include "provider_resolve.h"   // L4（PB2-27）：状态栏按生效条目站点判登录态

// ---- M6（非技术用户可安装化）----
#include "app_version.h"          // 版本号单一来源（帮助 → 关于 / 诊断报告 / 安装包同源）
#include "diagnostics_report.h"   // PM-04：一键复制诊断信息
#include "settings_panel.h"       // M6-01：设置面板
#include "node_registry.h"        // PB2-24：配置表重载后重建 `provider` 参数枚举
#include "webview2_runtime.h"     // PM-01：WebView2 运行时依赖自检

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// ---- Windows 基础类型（M6）：首启引导的「打开下载页」用 ShellExecuteW
//      （本工程全局 WIN32_LEAN_AND_MEAN：windows.h **不含** shellapi.h，须显式引入）
#include <windows.h>

#include <shellapi.h>

#include <GLFW/glfw3.h>

// GLFW 在 Windows 下已包含 GL/gl.h；显式引入以使用 glViewport/glClear*
#include <GL/gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace aiwrite::ui {

using namespace app_detail;  // 原匿名命名空间函数（同 TU 内直接调用，名字查找不变）
int run(const AppOptions& options)
{
    (void)options;

    glfwSetErrorCallback(glfw_error_callback);
    if (glfwInit() == GLFW_FALSE) {
        log::error("GLFW 初始化失败");
        return 1;
    }

    // ARCH-A3：**启动组装**（唯一构造点）—— 按 §1.8.11 依赖顺序创建全部长生命周期对象，
    // 之后只把引用交给各 UI 模块（判据 8：运行时不再回取 AppContext）。
    // 构造不依赖 GLFW / 窗口（订阅 run 事件 / autosave 编辑事件），因此可以提前到这里。
    AppContext app_context;
    EditorController& state = app_context.editor;

    // 配置（M1-07：读配置 / 写日志）—— 必须早于窗口创建：窗口几何（F3 / PD-04）来自 config.toml [ui]
    Config& config = app_context.config;
    (void)loadConfig(paths::configFile(), config);
    // PA-07：灌入进程级缓存，供非 UI 代码（provider / 工具）读取
    setAppConfig(config);

    // PA-08：明确"哪些配置已生效 / 哪些还没接线"，消除"改了 config 却没效果"的困惑
    {
        log::info("[配置] 已接线：ui.show_grid / ui.console_height / ui.running_animation / "
                  "面板可见性 / 窗口几何（window_*）/ output.archive_dir 等");
        std::string unwired;
        for (const std::string& field : unwiredConfigFields()) {
            unwired += (unwired.empty() ? "" : "、") + field;
        }
        log::info("[配置] 以下字段尚未生效（归口补丁见使用说明）：" + unwired);
    }

    // PB-06 / M_patchAB §2.3（`PB-06` 剩余）：凭据库启动惰性清理（TTL 取自 `[credentials].ttl_days`）
    {
        const int   ttl_days = aiwrite::appConfig().credentials.ttl_days;
        std::string purge_error;
        const int   removed = utils::purgeExpiredCredentials(ttl_days, &purge_error);
        if (removed > 0) {
            log::info("[凭据] 启动清理：已移除 " + std::to_string(removed) + " 个过期条目（TTL " +
                      std::to_string(ttl_days) + " 天）");
        }
    }

    // ---- M6：依赖自检（WebView2 运行时）----
    //  * 面向非技术用户：缺依赖时**日志写明「缺什么、去哪装」**，界面另有首启引导窗口
    //  * 探测是只读的（不下载、不安装、不改配置）
    const utils::WebView2RuntimeInfo webview2 = utils::detectWebview2Runtime();
    log::info("[依赖] " + utils::webview2_runtime_summary(webview2));
    if (!webview2.installed) {
        log::warn("[依赖] 网页版站点不可用：请安装 WebView2 运行时"
                  "（帮助 → 首次使用引导 里有下载链接与步骤）");
    }

    // ---- M6 接线：`advanced.log_ttl_days`（启动清理旧日志；活动文件永不删）----
    {
        std::string purge_error;
        const int   removed = log::purgeOldFiles(config.advanced.log_ttl_days, &purge_error);
        if (removed > 0) {
            log::info("[日志] 已清理 " + std::to_string(removed) + " 个超过 " +
                      std::to_string(config.advanced.log_ttl_days) + " 天的旧日志（" +
                      paths::logsDir().string() + "）");
        }
        if (!purge_error.empty()) {
            log::warn("[日志] 旧日志清理未全部成功：" + purge_error);
        }
    }

    // OpenGL 3.3 Core（M1 Action Plan M1-02）
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // ---- 窗口几何（F3 / PD-04）----
    //  1) 基准尺寸：工作区 90%（上限 1600x1000）
    //  2) config 记录过尺寸则优先；未记录位置（pos < 0）→ 用工作区左上
    //  3) 一律经 fit_window_to_workarea 越屏矫正（纯函数，api_probe 有断言覆盖）
    int window_width  = 1600;
    int window_height = 1000;
    utils::WindowRect workarea{};
    GLFWmonitor*      primary_monitor = glfwGetPrimaryMonitor();
    if (primary_monitor != nullptr) {
        glfwGetMonitorWorkarea(primary_monitor, &workarea.x, &workarea.y, &workarea.width,
                               &workarea.height);
        if (workarea.width > 0 && workarea.height > 0) {
            window_width  = ((workarea.width * 9) / 10 > 1600) ? 1600 : (workarea.width * 9) / 10;
            window_height = ((workarea.height * 9) / 10 > 1000) ? 1000 : (workarea.height * 9) / 10;
        }
    }

    const bool        has_saved_geometry = config.ui.window_width > 0 && config.ui.window_height > 0;
    const bool        has_saved_position =
        utils::hasPosition(utils::WindowRect{config.ui.window_pos_x, config.ui.window_pos_y, 0, 0});
    utils::WindowRect desired{};
    desired.width  = has_saved_geometry ? config.ui.window_width : window_width;
    desired.height = has_saved_geometry ? config.ui.window_height : window_height;
    desired.x      = has_saved_position ? config.ui.window_pos_x : workarea.x;
    desired.y      = has_saved_position ? config.ui.window_pos_y : workarea.y;

    const utils::FitResult geometry = utils::fit_window_to_workarea(desired, workarea);
    window_width                    = geometry.rect.width;
    window_height                   = geometry.rect.height;

    GLFWwindow* window = glfwCreateWindow(window_width, window_height, kAppTitle, nullptr, nullptr);
    if (window == nullptr) {
        log::error("创建 GLFW 窗口失败（OpenGL 3.3 上下文不可用）");
        glfwTerminate();
        return 1;
    }

    glfwSetWindowPos(window, geometry.rect.x, geometry.rect.y);
    if (config.ui.window_maximized) {
        glfwMaximizeWindow(window);
    }
    // 矫正结果落回内存配置：即便本次没有移动窗口，退出时也会写盘（下次启动即为矫正后的几何）
    config.ui.window_width  = geometry.rect.width;
    config.ui.window_height = geometry.rect.height;
    config.ui.window_pos_x  = geometry.rect.x;
    config.ui.window_pos_y  = geometry.rect.y;
    log::info("窗口几何（config.toml [ui]）：尺寸 " + std::to_string(geometry.rect.width) + "x" +
              std::to_string(geometry.rect.height) + " 位置 " + std::to_string(geometry.rect.x) + "," +
              std::to_string(geometry.rect.y) +
              (has_saved_geometry ? "（来自配置）" : "（默认：工作区 90%）") +
              (geometry.changed ? "｜已越屏矫正" : "") +
              (config.ui.window_maximized ? "｜最大化=是" : ""));

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // 停靠（设计文档 13.1）
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // 布局文件放在数据目录，避免污染程序目录
    static std::string ini_path = (paths::dataRoot() / "imgui.ini").string();
    io.IniFilename = ini_path.c_str();

    float xscale = 1.0f;
    float yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    const float ui_scale = (yscale > 0.0f) ? yscale : 1.0f;

    apply_dark_theme();
    if (ui_scale != 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(ui_scale);
    }

    const Fonts fonts = load_fonts(ui_scale);
    io.FontDefault = fonts.ui; // DEBT-L11（M6-05）：显式指定默认字体（= CJK 主字体；回退路径亦正确）

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ========================================================================
    //  ARCH-Z1（第 3 组）：窗口级局部状态一次性收口
    //   * 原 run() 的 UI 局部变量（面板可见性 / 自动保存询问 / 帧计数 / 画布选项）
    //     搬进 `AppFrameContext`（初值与原文逐字一致）；此处只留 `auto&` 别名，
    //     因此后续语句与主循环骨架保持原文文本。
    //   * 构造顺序 = 原局部声明顺序（窗口 → 配置 / 编排器 / 字体 / IO / 依赖 / 参数）。
    // ========================================================================
    AppFrameContext frame{window, config, state, fonts, io, webview2, options};
    CanvasOptions& canvas_options = frame.canvas_options;
    int&           frame_index    = frame.frame_index;
    canvas_options.show_grid         = config.ui.show_grid;
    canvas_options.running_animation = config.ui.running_animation; // PA-08：执行态动效开关

    // 面板可见性来自 config.toml（设计 §7.2：节点库/参数面板默认隐藏、可切换；
    // M6-06 设置面板落地前，手动切换会写回 [ui] 段以便下次记住）
    // 面板可见性（原局部 ⇒ AppFrameContext 成员；初值原文搬入，别名逐字复原）
    bool& show_console = frame.show_console;
    bool& show_output  = frame.show_output;
    bool& show_library = frame.show_library;
    bool& show_params  = frame.show_params;
    log::info(std::string("界面可见性（config.toml [ui]）: 节点库=") +
              (show_library ? "显示" : "隐藏") + "，参数面板=" + (show_params ? "显示" : "隐藏") +
              "，Console=" + (show_console ? "显示" : "隐藏") + "，输出=" +
              (show_output ? "显示" : "隐藏") + "，网格=" +
              (canvas_options.show_grid ? "开" : "关"));

    // 启动即创建示例工作流，便于直接验证节点操作（可用工具栏「新建（清空）」重来）
    state.createSampleWorkflow();

    // --open <路径>：启动即打开指定工作流（覆盖上面的示例工作流）
    //  * 存在性/解析失败只告警并保留示例工作流（不阻塞启动）
    if (!options.open_workflow.empty()) {
        std::string open_error;
        if (state.openWorkflowFrom(options.open_workflow, &open_error)) {
            log::info("[启动] 已按 --open 打开工作流：" + options.open_workflow + "（节点 " +
                      std::to_string(state.model().graph.nodes.size()) + " / 连线 " +
                      std::to_string(state.model().graph.edges.size()) + "）");
        }
        else {
            log::warn("[启动] --open 打开失败：" + options.open_workflow + "（" + open_error +
                      "）—— 已回退到内置示例工作流");
        }
    }

    // ---- PC-01/02：自动保存快照的启动处理（M_patchAB §4）----
    //  ① 清理上次中断（进程被杀）残留的 `.tmp`：半成品**永不**参与恢复（恢复只读 autosave.json）
    //  ② 判定是否需要询问「恢复未保存的更改？」，判据见 engine::autosave_should_offer_restore：
    //     快照比工作流文件新 / 原文件不存在 / 快照未关联工作流 ⇒ 问一次（其余情况不打扰）
    //  ③ 自动保存**先暂停**（`autosave_paused` 默认 true）：否则启动时刚创建的示例工作流会在
    //     3 秒后把真快照覆盖掉 —— 询问有结论（或无需询问）时才放行
    std::string autosave_temp_error;
    const int   autosave_temp_removed = engine::autosave_cleanup_temp(&autosave_temp_error);
    if (autosave_temp_removed > 0) {
        log::info("[自动保存] 已清理上次中断残留的临时文件 " + std::to_string(autosave_temp_removed) +
                  " 个（快照本身未受影响）");
    }
    if (!autosave_temp_error.empty()) {
        log::warn("[自动保存] " + autosave_temp_error);
    }

    // 自动保存快照状态（原局部 ⇒ AppFrameContext 成员；别名 + probe 赋值逐字复原）
    engine::AutosaveInfo& autosave_info   = frame.autosave_info;
    std::string&          autosave_reason = frame.autosave_reason;
    bool&                 autosave_ask    = frame.autosave_ask;
    autosave_info = engine::autosaveProbe();
    if (autosave_info.ok) {
        log::info("[自动保存] 发现快照：" + autosave_info.saved_at + " · " +
                  (autosave_info.name.empty() ? "未命名" : autosave_info.name) + " · 节点 " +
                  std::to_string(autosave_info.nodes) + " / 连线 " +
                  std::to_string(autosave_info.edges) +
                  (autosave_info.workflow_path.empty()
                       ? std::string(" · 未关联工作流文件")
                       : " · 关联 " + autosave_info.workflow_path));
    }
    const bool autosave_should_ask =
        config.ui.autosave_restore_prompt &&
        engine::autosaveShouldOfferRestore(autosave_info, &autosave_reason);
    if (autosave_should_ask) {
        autosave_ask = true;
        log::info("[自动保存] 将询问是否恢复未保存的更改（判据：" + autosave_reason + "）");
    }
    else {
        // 无需询问（无快照 / 快照不比工作流新 / 用户选过「不再提示」）⇒ 直接放行自动保存
        log::info("[自动保存] 无需恢复询问（" + autosave_reason + "）→ 自动保存已放行");
        // ARCH-A2：EditorController autosave 方法已移除 —— 直连 state.autosave()
        state.autosave().mark_baseline();
        state.autosave().pause(false);
    }

    // 诊断：安装首异常计数器（VEH）——用于定位第三方依赖反复抛异常 / 内存增长问题，
    // 结果会以「[诊断]」行写入 app.log（带限流）。ARCH-A1：归口 state.diagnostics()
    state.diagnostics().install_exception_counter();
    state.diagnostics().log_memory_sample("启动基线");
    log::info("主窗口已创建 " + std::to_string(window_width) + "x" + std::to_string(window_height) +
              "，DPI 缩放 " + std::to_string(ui_scale));

    log::info("界面初始化完成，进入渲染循环（节点画布 + Console + 工作流信息）");

    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
        const bool first_frame = (frame_index++ == 0);
        glfwPollEvents();
        // ---- ARCH-Z1（第 3 组）：主循环体分段 —— 逐字搬入 app_frame / app_panels，
        //      调用顺序 = 原语句顺序（帧首推进 → 菜单栏 → 恢复弹窗 → 工具栏/DockSpace →
        //      面板 → 状态栏 → 看门狗/出帧）。返回 false = 原「最小化 → continue」分支。
        if (!appFrameBegin(frame)) {
            continue;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float status_height     = ImGui::GetFrameHeight();

        // ARCH-R3：主循环**统一构建一次**渲染只读视图（ARCH-D30），传给所有渲染模块
        //  * 字段 = 三个核心 + 配置（ARCH-D31）：工作流数据 / 选中 / 运行快照 / 配置
        //  * 只存引用、零拷贝（ARCH-D34）；快照空对象 = 未运行（ARCH-D32）
        // ARCH-E4：选中独立对象（SelectionState）；ReadOnlyView 直接引用 state.selection()
        const ReadOnlyView view{state.model().graph, state.selection(), state.run_snapshot_view(),
                                config};
        appFrameMenuBar(frame, viewport);
        appFrameRestorePopup(frame);
        appFrameDockSpace(frame, viewport, status_height);
        appFramePanels(frame, view);
        appFrameStatusBar(frame, view, viewport, status_height);
        appFrameEnd(frame);
        if (first_frame) {
            log::info("[首帧] 渲染完成（节点画布与各面板均已就绪）");
        }
    }
    log::info("主窗口关闭，开始清理");
    state.stopRunAsync(); // PB-01：退出前 join 工作线程（幂等）
    compositeEditorStopRun(); // 上级节点「执行连线」：退出前 join 试运行线程（幂等）

    // ---- PC-01：退出前补写快照（有未保存的更改才写；正常退出与崩溃都能在下一次启动恢复）----
    // ARCH-A2：EditorController autosave 方法已移除 —— 直连 state.autosave()
    state.autosave().flush(state.workflow_file().current_workflow_path());

    // ---- 窗口几何退出落盘（F3 / PD-04）：节流保存之外的最终兜底 ----
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) == 0) {
        const bool maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
        if (!maximized) {
            glfwGetWindowPos(window, &config.ui.window_pos_x, &config.ui.window_pos_y);
            glfwGetWindowSize(window, &config.ui.window_width, &config.ui.window_height);
        }
        config.ui.window_maximized = maximized;
        if (saveConfig(paths::configFile(), config)) {
            setAppConfig(config);
            log::info("[窗口几何] 退出时已保存到 config.toml（" +
                      std::to_string(config.ui.window_width) + "x" +
                      std::to_string(config.ui.window_height) + " @ " +
                      std::to_string(config.ui.window_pos_x) + "," +
                      std::to_string(config.ui.window_pos_y) +
                      (maximized ? "，最大化=是" : "") + "）");
        }
        else {
            log::warn("[窗口几何] 退出保存失败：config.toml 不可写？");
        }
    }

    // M5-03：纹理缓存持有 GL 资源 → 必须先于上下文销毁释放
    state.textures().releaseAll();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
}  // namespace aiwrite::ui
