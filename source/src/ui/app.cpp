#include "ui/app.h"

#include "ui/console_panel.h"
#include "ui/editor_state.h"
#include "ui/node_canvas.h"
#include "ui/node_library.h"
#include "ui/output_panel.h"
#include "ui/property_panel.h"
#include "ui/toolbar.h"
#include "utils/config.h"
#include "utils/diagnostics.h"
#include "utils/log.h"
#include "utils/paths.h"
#include "utils/window_geometry.h"
#include "web/session_store.h"

#include "engine/recent_files.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

// GLFW 在 Windows 下已包含 GL/gl.h；显式引入以使用 glViewport/glClear*
#include <GL/gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace aiwrite::ui {
namespace {

constexpr const char* kAppTitle      = "AIwrite · AI 小说/剧本创作";
constexpr const char* kWindowCanvas  = "节点画布";
constexpr const char* kWindowLibrary = "节点库";
constexpr const char* kWindowParams  = "参数";
constexpr const char* kWindowConsole = "Console";
constexpr const char* kWindowOutput  = "输出";
constexpr const char* kWindowInfo    = "工作流信息";

void glfw_error_callback(int code, const char* description)
{
    log::error("GLFW 错误 " + std::to_string(code) + ": " + (description ? description : ""));
}

// 在候选目录中按顺序查找第一个存在的字体文件
std::filesystem::path find_font(const std::vector<std::string>& names,
                                const std::vector<std::filesystem::path>& dirs)
{
    for (const auto& dir : dirs) {
        if (dir.empty() || !std::filesystem::exists(dir)) {
            continue;
        }
        for (const auto& name : names) {
            const std::filesystem::path candidate = dir / name;
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
    }
    return {};
}

struct Fonts {
    ImFont* ui      = nullptr;
    ImFont* console = nullptr;
};

// 设计文档 14.5：UI 主字体 微软雅黑 14px、Console Consolas 13px
// 优先取 <exe>/assets/fonts，其次系统字体（不把字体文件放进仓库）
Fonts load_fonts(float scale)
{
    ImGuiIO& io = ImGui::GetIO();
    Fonts fonts;

    const std::vector<std::filesystem::path> dirs = {
        paths::assets_dir() / "fonts", std::filesystem::path("C:/Windows/Fonts")};
    const std::vector<std::string> cjk_names = {
        "msyh.ttc", "msyh.ttf", "simhei.ttf", "Deng.ttf", "segoeui.ttf"};

    const std::filesystem::path cjk = find_font(cjk_names, dirs);
    if (!cjk.empty()) {
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.PixelSnapH  = true;
        fonts.ui = io.Fonts->AddFontFromFileTTF(cjk.string().c_str(), 14.0f * scale, &config,
                                               io.Fonts->GetGlyphRangesChineseFull());
        log::info("UI 字体: " + cjk.string());
    }

    if (fonts.ui == nullptr) {
        io.Fonts->AddFontDefault();
        fonts.ui = io.Fonts->Fonts[0];
        log::warn("未找到中文字体，使用 ImGui 默认字体（中文可能显示为方块）");
    }

    const std::filesystem::path mono = find_font({"consola.ttf", "CascadiaMono.ttf"}, dirs);
    if (!mono.empty()) {
        fonts.console = io.Fonts->AddFontFromFileTTF(mono.string().c_str(), 13.0f * scale, nullptr,
                                                    io.Fonts->GetGlyphRangesDefault());
        if (fonts.console != nullptr && !cjk.empty()) {
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(cjk.string().c_str(), 13.0f * scale, &merge,
                                        io.Fonts->GetGlyphRangesChineseFull());
            log::info("Console 字体: " + mono.string() + "（合并中文字形）");
        }
    }
    else {
        fonts.console = fonts.ui;
    }

    return fonts;
}

ImVec4 rgb(int r, int g, int b, float alpha = 1.0f)
{
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                  static_cast<float>(b) / 255.0f, alpha);
}

// 深色主题（设计文档 14.3 / 14.4）
void apply_dark_theme()
{
    ImGui::StyleColorsDark();

    ImGuiStyle& style       = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.WindowPadding     = ImVec2(8.0f, 8.0f);
    style.FramePadding      = ImVec2(8.0f, 4.0f);
    style.ItemSpacing       = ImVec2(8.0f, 6.0f);
    style.ScrollbarRounding = 4.0f;

    style.Colors[ImGuiCol_WindowBg]          = rgb(0x25, 0x25, 0x25); // 面板背景
    style.Colors[ImGuiCol_ChildBg]           = rgb(0x1A, 0x1A, 0x1A); // Console 背景
    style.Colors[ImGuiCol_Border]            = rgb(0x3A, 0x3A, 0x3A);
    style.Colors[ImGuiCol_TitleBg]           = rgb(0x1E, 0x1E, 0x1E);
    style.Colors[ImGuiCol_TitleBgActive]     = rgb(0x2D, 0x2D, 0x2D);
    style.Colors[ImGuiCol_MenuBarBg]         = rgb(0x1E, 0x1E, 0x1E);
    style.Colors[ImGuiCol_Separator]         = rgb(0x3A, 0x3A, 0x3A);
    style.Colors[ImGuiCol_FrameBg]           = rgb(0x1E, 0x1E, 0x1E);
    style.Colors[ImGuiCol_Button]            = rgb(0x3A, 0x3A, 0x3A);
    style.Colors[ImGuiCol_Header]            = rgb(0x4A, 0x90, 0xE2, 0.35f);
    style.Colors[ImGuiCol_HeaderHovered]     = rgb(0x4A, 0x90, 0xE2, 0.55f);
    style.Colors[ImGuiCol_HeaderActive]      = rgb(0x4A, 0x90, 0xE2, 0.75f);
    style.Colors[ImGuiCol_ButtonHovered]     = rgb(0x4A, 0x90, 0xE2, 0.60f);
    style.Colors[ImGuiCol_ButtonActive]      = rgb(0x4A, 0x90, 0xE2, 0.90f);
    style.Colors[ImGuiCol_Tab]               = rgb(0x2D, 0x2D, 0x2D);
    style.Colors[ImGuiCol_TabHovered]        = rgb(0x4A, 0x90, 0xE2, 0.50f);
    style.Colors[ImGuiCol_TabActive]         = rgb(0x3A, 0x6E, 0xA5);
    style.Colors[ImGuiCol_TabUnfocused]      = rgb(0x2D, 0x2D, 0x2D);
    style.Colors[ImGuiCol_TabUnfocusedActive] = rgb(0x3A, 0x6E, 0xA5, 0.75f);
    style.Colors[ImGuiCol_DockingPreview]    = rgb(0x4A, 0x90, 0xE2, 0.70f);
    style.Colors[ImGuiCol_DockingEmptyBg]    = rgb(0x1E, 0x1E, 0x1E);
}

// 默认停靠布局：中=节点画布，下=Console，右=工作流信息
void build_default_layout(ImGuiID dockspace_id, const ImVec2& size)
{
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);

    ImGuiID center = dockspace_id;
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
    const ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
    const ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);

    ImGui::DockBuilderDockWindow(kWindowCanvas, center);
    ImGui::DockBuilderDockWindow(kWindowLibrary, left);
    ImGui::DockBuilderDockWindow(kWindowConsole, bottom);
    ImGui::DockBuilderDockWindow(kWindowOutput, bottom); // PA-02：与 Console 同区（可拖动分离）
    ImGui::DockBuilderDockWindow(kWindowParams, right);
    ImGui::DockBuilderDockWindow(kWindowInfo, right);
    ImGui::DockBuilderFinish(dockspace_id);
}

// 工作流使用的推理后端模式（取第一个 ProviderConfig 节点）+ 网页版会话状态
std::string provider_mode_text(const EditorState& state)
{
    for (const engine::Node& node : state.graph.nodes) {
        if (node.type != "ProviderConfig") {
            continue;
        }
        const engine::Param* mode         = node.findParam("mode");
        const std::string    mode_value   = mode != nullptr ? mode->text() : "official";
        if (mode_value != "web") {
            return "推理模式: 官方 API";
        }
        const web::Session session = web::SessionStore::instance().snapshot();
        return session.logged_in
                   ? ("推理模式: 网页版（已登录 " + std::to_string(session.cookie_count()) +
                      " 条 Cookie）")
                   : std::string("推理模式: 网页版（未登录）");
    }
    return "推理模式: 未配置（缺少提供商配置节点）";
}

// 工作流信息面板（环境 + 当前工作流统计；数据来自 EditorState）
void draw_info_panel(const char* title, bool* open)
{
    if (open != nullptr && !*open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(400.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.29f, 0.56f, 0.89f, 1.0f), "AIwrite 0.2.0");
    ImGui::TextDisabled("M1 骨架 + M2 节点系统（P1 数据层 / P2 画布交互）");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("运行环境", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("ImGui    : %s", IMGUI_VERSION);
        ImGui::Text("后端     : GLFW + OpenGL3");
        if (const GLubyte* renderer = glGetString(GL_RENDERER); renderer != nullptr) {
            ImGui::TextWrapped("显卡     : %s", reinterpret_cast<const char*>(renderer));
        }
        if (const GLubyte* version = glGetString(GL_VERSION); version != nullptr) {
            ImGui::Text("OpenGL   : %s", reinterpret_cast<const char*>(version));
        }
        ImGui::Text("FPS      : %.1f", ImGui::GetIO().Framerate);
    }

    if (ImGui::CollapsingHeader("数据目录", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("数据根目录 : %s", paths::data_root().string().c_str());
        ImGui::TextWrapped("配置文件   : %s", paths::config_file().string().c_str());
        ImGui::TextWrapped("日志文件   : %s", log::file_path_string().c_str());
    }

    const EditorState& state = editor();
    if (ImGui::CollapsingHeader("当前工作流", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("节点     : %d", static_cast<int>(state.graph.nodes.size()));
        ImGui::Text("连线     : %d", static_cast<int>(state.graph.edges.size()));
        ImGui::Text("选中     : %d", static_cast<int>(state.selected_nodes.size()));
        ImGui::Text("撤销栈   : %d（可重做 %d）", static_cast<int>(state.undo.undoDepth()),
                    static_cast<int>(state.undo.redoDepth()));
        if (!state.status.empty()) {
            ImGui::TextWrapped("状态     : %s", state.status.c_str());
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("拖拽标题栏可停靠至任意位置；");
    ImGui::TextDisabled("菜单「视图 → 重置布局」恢复默认布局。");

    ImGui::End();
}

} // namespace

int run(const AppOptions& options)
{
    (void)options;

    glfwSetErrorCallback(glfw_error_callback);
    if (glfwInit() == GLFW_FALSE) {
        log::error("GLFW 初始化失败");
        return 1;
    }

    // 配置（M1-07：读配置 / 写日志）—— 必须早于窗口创建：窗口几何（F3 / PD-04）来自 config.toml [ui]
    Config config;
    (void)load_config(paths::config_file(), config);
    // PA-07：灌入进程级缓存，供非 UI 代码（provider / 工具）读取
    set_app_config(config);

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
        utils::has_position(utils::WindowRect{config.ui.window_pos_x, config.ui.window_pos_y, 0, 0});
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
    static std::string ini_path = (paths::data_root() / "imgui.ini").string();
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

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    CanvasOptions canvas_options;
    canvas_options.show_grid = config.ui.show_grid;

    // 面板可见性来自 config.toml（设计 §7.2：节点库/参数面板默认隐藏、可切换；
    // M6-06 设置面板落地前，手动切换会写回 [ui] 段以便下次记住）
    bool show_console         = config.ui.show_console;
    bool show_output          = config.ui.show_output_window; // PA-02：首次接线该配置项
    bool show_library         = config.ui.show_node_library;
    bool show_params          = config.ui.show_property_panel;
    bool show_info            = true; // 工作流信息面板不在 config 中：始终显示
    bool layout_ready         = false;
    bool request_layout_reset = false;

    log::info(std::string("界面可见性（config.toml [ui]）: 节点库=") +
              (show_library ? "显示" : "隐藏") + "，参数面板=" + (show_params ? "显示" : "隐藏") +
              "，Console=" + (show_console ? "显示" : "隐藏") + "，输出=" +
              (show_output ? "显示" : "隐藏") + "，网格=" +
              (canvas_options.show_grid ? "开" : "关"));

    // 启动即创建示例工作流，便于直接验证节点操作（可用工具栏「新建（清空）」重来）
    editor().create_sample_workflow();

    // 诊断：安装首异常计数器（VEH）——用于定位第三方依赖反复抛异常 / 内存增长问题，
    // 结果会以「[诊断]」行写入 app.log（带限流）
    utils::diagnostics::install_exception_counter();
    utils::diagnostics::log_memory_sample("启动基线");

    log::info("主窗口已创建 " + std::to_string(window_width) + "x" + std::to_string(window_height) +
              "，DPI 缩放 " + std::to_string(ui_scale));

    log::info("界面初始化完成，进入渲染循环（节点画布 + Console + 工作流信息）");

    int frame_index = 0;

    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
        const bool first_frame = (frame_index++ == 0);
        glfwPollEvents();

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
                    if (save_config(paths::config_file(), config)) {
                        set_app_config(config); // PA-07：保持进程缓存与磁盘一致
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
        editor().tick_run();

        // ---- 诊断（限流输出）：放在"最小化检查"之前，保证窗口不可见时也能采样 ----
        //  · 首异常累计（有变化才输出，限流）
        //  · 心跳（每 300 帧）：确认渲染循环真的在跑
        //  · 每 30 秒一次内存采样（工作集 / 私有 / 峰值 + 业务计数）
        utils::diagnostics::log_exception_summary();
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
            const EditorState& diagnostic_state = editor();
            utils::diagnostics::log_memory_sample(
                "节点 " + std::to_string(diagnostic_state.graph.nodes.size()) + " / 连线 " +
                std::to_string(diagnostic_state.graph.edges.size()) + " / 撤销栈 " +
                std::to_string(diagnostic_state.undo.undoDepth()));
        }

        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
            // 最小化时降低 CPU 占用（不依赖后端辅助函数，保持版本兼容）
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float status_height     = ImGui::GetFrameHeight();
        EditorState& state            = editor();

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

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("文件")) {
                if (ImGui::MenuItem("新建（清空画布）")) {
                    state.clear_workflow();
                }
                if (ImGui::MenuItem("示例工作流")) {
                    state.create_sample_workflow();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("打开工作流…")) {
                    open_workflow_dialog();
                }
                if (ImGui::MenuItem("保存工作流…")) {
                    save_workflow_dialog();
                }
                if (ImGui::BeginMenu("最近打开")) {
                    const std::vector<engine::RecentEntry> recent = state.recent_workflows();
                    if (recent.empty()) {
                        ImGui::MenuItem("（暂无记录）", nullptr, false, false);
                    }
                    for (const engine::RecentEntry& entry : recent) {
                        const std::string label = entry.name + "##" + entry.path; // ## 保证 id 唯一
                        if (ImGui::MenuItem(label.c_str())) {
                            std::string error;
                            if (!state.open_workflow_from(entry.path, &error)) {
                                state.set_status("打开失败：" + error);
                            }
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s\n最后打开：%s", entry.path.c_str(),
                                              entry.opened_at.c_str());
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("清空列表")) {
                        engine::clear_recent_files();
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("退出")) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("编辑")) {
                if (ImGui::MenuItem("撤销", nullptr, false, state.undo.canUndo())) {
                    state.undo_once();
                }
                if (ImGui::MenuItem("重做", nullptr, false, state.undo.canRedo())) {
                    state.redo_once();
                }
                ImGui::Separator();
                // 设计 §6.1：复制（节点）/ 粘贴（画布）——菜单入口与右键菜单一致
                if (ImGui::MenuItem("复制", nullptr, false, state.has_selection())) {
                    state.copy_selection();
                }
                if (ImGui::MenuItem("粘贴", nullptr, false, state.has_clipboard())) {
                    state.paste_clipboard();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("删除选中", nullptr, false, state.has_selection())) {
                    state.delete_selected();
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
                    if (save_config(paths::config_file(), config)) {
                        set_app_config(config); // PA-07：保持进程缓存与磁盘一致
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

            if (ImGui::BeginMenu("帮助")) {
                ImGui::MenuItem("关于 AIwrite 0.2.0（M1 + M2 P1/P2）", nullptr, false, false);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // ---- 工具栏（全部鼠标操作；设计 §6.7：MVP 不设快捷键）----
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
        if (ImGui::BeginChild("##toolbar", ImVec2(0.0f, ImGui::GetFrameHeight() + 10.0f), false)) {
            draw_toolbar_buttons();
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

        // ---- 面板 ----
        ImGui::PushFont(fonts.ui);
        draw_node_canvas(kWindowCanvas, canvas_options);
        ImGui::PopFont();

        if (show_library) {
            ImGui::PushFont(fonts.ui);
            draw_node_library(kWindowLibrary, &show_library);
            ImGui::PopFont();
        }

        if (show_params) {
            ImGui::PushFont(fonts.ui);
            engine::Node* selected =
                state.selected_node.empty() ? nullptr : state.graph.findNode(state.selected_node);

            PropertyEditResult edit;
            draw_property_panel(kWindowParams, &show_params, selected, edit);

            // begin_edit 在控件被激活、值尚未变化时置位 → 此时压快照可正确回退
            if (edit.begin_edit) {
                state.snapshot("修改参数");
            }
            if (edit.changed) {
                state.set_status("参数已修改（可用工具栏「撤销」回退）");
            }
            if (edit.renamed) {
                state.set_status("节点标题已修改");
            }
            ImGui::PopFont();
        }

        if (show_console) {
            ImGui::PushFont(fonts.console);
            draw_console_panel(kWindowConsole, &show_console);
            ImGui::PopFont();
        }
        if (show_output) { // PA-02：输出面板（结果全文 / 复制全文·该节点 / 导出）
            ImGui::PushFont(fonts.ui);
            draw_output_panel(kWindowOutput, &show_output, state);
            ImGui::PopFont();
        }
        if (show_info) {
            ImGui::PushFont(fonts.ui);
            draw_info_panel(kWindowInfo, &show_info);
            ImGui::PopFont();
        }

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
                        static_cast<int>(state.graph.nodes.size()),
                        static_cast<int>(state.graph.edges.size()),
                        static_cast<int>(state.selected_nodes.size()), io.Framerate);
            ImGui::SameLine();
            ImGui::TextDisabled("  %s", provider_mode_text(state).c_str());
            const std::string run_text = state.run_status_text();
            if (!run_text.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.40f, 0.72f, 1.0f, 1.0f), "  %s", run_text.c_str());
            }
            if (!state.status.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "  %s", state.status.c_str());
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "  日志: %s",
                               log::file_path_string().c_str());
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        if (first_frame) {
            log::info("[首帧] 渲染完成（节点画布与各面板均已就绪）");
        }
    }

    log::info("主窗口关闭，开始清理");

    // ---- 窗口几何退出落盘（F3 / PD-04）：节流保存之外的最终兜底 ----
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) == 0) {
        const bool maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
        if (!maximized) {
            glfwGetWindowPos(window, &config.ui.window_pos_x, &config.ui.window_pos_y);
            glfwGetWindowSize(window, &config.ui.window_width, &config.ui.window_height);
        }
        config.ui.window_maximized = maximized;
        if (save_config(paths::config_file(), config)) {
            set_app_config(config);
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

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

} // namespace aiwrite::ui
