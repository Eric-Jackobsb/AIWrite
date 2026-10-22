#include "app.h"

#include "console_panel.h"
#include "composite_editor.h"   // CN：上级节点编辑器 / 打包 / 新建
#include "editor_state.h"
#include "error_bar.h"
#include "node_canvas.h"
#include "node_library.h"
#include "output_panel.h"
#include "property_panel.h"
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
namespace {

constexpr const char* kAppTitle      = "AIwrite · AI 小说/剧本创作";
constexpr const char* kWindowCanvas  = "节点画布";
constexpr const char* kWindowLibrary = "节点库";
constexpr const char* kWindowParams  = "参数";
constexpr const char* kWindowConsole = "Console";
constexpr const char* kWindowOutput  = "输出";
constexpr const char* kWindowInfo    = "工作流信息";
constexpr const char* kWindowCompositeEditor = "上级节点编辑"; // CN

void glfw_error_callback(int code, const char* description)
{
    log::error("GLFW 错误 " + std::to_string(code) + ": " + (description ? description : ""));
}

// [调试·窗口栈看门狗] 快照 ImGui 当前窗口栈（Begin/BeginChild 未配对的定位工具）。
// 在 Render()（内部 EndFrame() 断言「Mismatched Begin/BeginChild vs End/EndChild」）
// 之前调用：栈深≠1 时，栈顶窗口就是「调了 Begin/BeginChild 却没配 End/EndChild」的那个。
std::string window_stack_dump()
{
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    std::string   dump;
    if (ctx == nullptr) {
        return dump + "(无 ImGui 上下文)";
    }
    dump += "栈深=" + std::to_string(ctx->CurrentWindowStack.Size) + "\n";
    for (int i = 0; i < ctx->CurrentWindowStack.Size; ++i) {
        const ImGuiWindow* w = ctx->CurrentWindowStack.Data[i].Window;
        dump += "  [" + std::to_string(i) + "] ";
        dump += (w != nullptr && w->Name != nullptr) ? w->Name : "(null)";
        if (w != nullptr) {
            dump += (w->Flags & ImGuiWindowFlags_ChildWindow) != 0 ? " [child]" : " [window]";
            dump += " BeginCount=" + std::to_string(w->BeginCount);
            if (w->Appearing) {
                dump += " Appearing";
            }
            if (!w->WasActive) {
                dump += " !WasActive";
            }
        }
        dump += "\n";
    }
    if (ctx->CurrentWindowStack.Size > 1) {
        const ImGuiWindow* top = ctx->CurrentWindowStack.back().Window;
        dump += "  → 顶层未 End 的窗口: ";
        dump += (top != nullptr && top->Name != nullptr) ? top->Name : "(null)";
        dump += "\n";
    }
    return dump;
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
        paths::assetsDir() / "fonts", std::filesystem::path("C:/Windows/Fonts")};
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
    ImGui::DockBuilderDockWindow(kWindowCompositeEditor, center); // CN：与画布同区（标签页切换）
    ImGui::DockBuilderDockWindow(kWindowLibrary, left);
    ImGui::DockBuilderDockWindow(kWindowConsole, bottom);
    ImGui::DockBuilderDockWindow(kWindowOutput, bottom); // PA-02：与 Console 同区（可拖动分离）
    ImGui::DockBuilderDockWindow(kWindowParams, right);
    ImGui::DockBuilderDockWindow(kWindowInfo, right);
    ImGui::DockBuilderFinish(dockspace_id);
}

// 工作流使用的推理后端模式（取第一个 ProviderConfig 节点）+ 网页版会话状态
//  * L4（PB2-27③ / 不变量 I15）：状态栏按**生效条目自己的站点键**判登录态
//    （不再读「默认槽」会话，也不再以 `userToken` 为判据 → 多站点下不再显示别的站点状态）
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
        const engine::EffectiveProvider effective = engine::resolveSelfProvider(node);
        const ai::ProviderWebSpec       site      = ai::strictWebSpecFor(effective.spec);
        const std::string               site_key  = web::siteKeyOf(site.login_url);
        const web::Session              session   = web::SessionStore::instance().snapshot(site_key);
        const ai::WebSessionVerdict     verdict =
            ai::webSessionState(effective.spec, web::webSessionEvidence(session));
        if (verdict.state == ai::WebSessionState::loggedIn) {
            return "推理模式: 网页版（已登录 " +
                   (site_key.empty() ? std::string("该站点") : site_key) + "，Cookie " +
                   std::to_string(session.cookieCount()) + " 条）";
        }
        return "推理模式: 网页版（" + ai::webSessionStateLabel(verdict.state) +
               (site_key.empty() ? std::string() : "：" + site_key) + "）";
    }
    return "推理模式: 未配置（缺少提供商配置节点）";
}

// P7a-18：状态栏「提供商」文本（生效条目的显示名 + 模型；取第一个「提供商配置」节点）
std::string provider_summary_text(const EditorState& state)
{
    for (const engine::Node& node : state.graph.nodes) {
        if (node.type != "ProviderConfig") {
            continue;
        }
        const engine::EffectiveProvider effective = engine::resolveSelfProvider(node);
        if (!effective.resolved || effective.display.empty()) {
            return "提供商: 未选择";
        }
        return "提供商: " + effective.display + "（" + effective.provider + "）· 模型 " +
               effective.model;
    }
    return "提供商: 未配置";
}

// P7a-18：状态栏「最后耗时」（最慢节点 = 关键路径近似；另给累计）
std::string last_run_duration_text(const EditorState& state)
{
    const engine::RunSnapshot& snapshot = state.run_snapshot_view();
    if (snapshot.nodes.empty()) {
        return {};
    }
    double longest = 0.0;
    double total   = 0.0;
    for (const engine::RunNodeView& view : snapshot.nodes) {
        if (view.duration_ms > longest) {
            longest = view.duration_ms;
        }
        total += view.duration_ms;
    }
    return "耗时 " + std::to_string(static_cast<long long>(longest)) + " ms（累计 " +
           std::to_string(static_cast<long long>(total)) + " ms / " +
           std::to_string(static_cast<int>(snapshot.nodes.size())) + " 节点）";
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
        ImGui::TextWrapped("数据根目录 : %s", paths::dataRoot().string().c_str());
        ImGui::TextWrapped("配置文件   : %s", paths::configFile().string().c_str());
        ImGui::TextWrapped("日志文件   : %s", log::filePathString().c_str());
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

// ---- M6（PM-01 / PM-02）：依赖自检的「打开下载页」 + 首次使用引导 ----

// 用系统默认浏览器打开链接（URL 为 ASCII；失败只写日志，不弹窗打断）
void open_url(const std::string& url)
{
    const std::wstring wide(url.begin(), url.end());
    const HINSTANCE    instance =
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(instance) <= 32) {
        log::warn("[首启] 打开链接失败（ShellExecuteW）：" + url);
    }
    else {
        log::info("[首启] 已在浏览器中打开：" + url);
    }
}

// 首次使用引导（M6 `PM-02`）
//  * 触发：`ui.first_run_done == false`（一次性；升级后的老用户也会看到一次），
//    或菜单「帮助 → 首次使用引导」（force = true，重置会话内标记）
//  * 结束方式（开始使用 / 跳过 / 右上角 ×）都会写 `first_run_done = true` 落盘
//  * 窗口是**普通窗口**（不阻塞画布），一次只出现一次，可随时关掉
void draw_first_run_guide(Config& config, EditorState& state,
                          const utils::WebView2RuntimeInfo& detected, bool force)
{
    static bool                        g_done      = false;
    static bool                        g_dep_ready = false;
    static utils::WebView2RuntimeInfo  g_dep;

    if (force) {
        g_done = false; // 菜单手动打开：允许再次显示
    }
    if (!g_dep_ready) {
        g_dep       = detected;
        g_dep_ready = true;
    }
    if (g_done || (config.ui.first_run_done && !force)) {
        return;
    }

    const auto finish = [&](bool started) {
        g_done                   = true;
        config.ui.first_run_done = true;
        if (saveConfig(paths::configFile(), config)) {
            setAppConfig(config);
            log::info("[首启] 引导已结束（" + std::string(started ? "开始使用" : "跳过") +
                      "）；已写入 ui.first_run_done = true");
        }
        else {
            log::warn("[首启] 引导结束但 config.toml 写入失败（下次启动会再显示）");
        }
        state.setStatus(started ? "欢迎使用！建议先点工具栏「运行」跑一次示例"
                                 : "已跳过首次引导（帮助 → 首次使用引导 可再看）");
    };

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(700.0f, 560.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("首次使用引导##firstrun", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(0.40f, 0.72f, 1.0f, 1.0f), "欢迎使用 AIwrite");
        ImGui::TextWrapped("这个窗口只在首次使用时出现一次，带你把「能跑起来」的最小步骤走完（随时可关）。");
        ImGui::Separator();

        ImGui::Text("第 1 步 · 环境检查（网页版模型站点需要 Edge WebView2 运行时）");
        ImGui::TextColored(g_dep.installed ? ImVec4(0.40f, 0.85f, 0.45f, 1.0f)
                                          : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "    %s", utils::webview2_runtime_summary(g_dep).c_str());
        if (!g_dep.installed) {
            ImGui::TextWrapped("%s", utils::webview2_install_advice().c_str());
            // PQ-02：安装包**附带** Bootstrapper 时给「随包安装」；未附带则只给官方下载页
            const std::string bundled = utils::bundledBootstrapperPath();
            if (!bundled.empty()) {
                if (ImGui::Button("安装 WebView2 运行时（随包附带）")) {
                    open_url(bundled); // 交给对方安装程序；需要管理员时 Windows 会弹 UAC
                    log::info("[首启] 已启动随包附带的 WebView2 引导程序：" + bundled);
                }
                ImGui::SameLine();
            }
            if (ImGui::Button("打开下载页")) {
                open_url(utils::webview2_download_url());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("（装完回来点「重新检测」；系统通常已自带）");
        }
        if (ImGui::Button("重新检测")) {
            g_dep = utils::detectWebview2Runtime();
            log::info("[首启] 重新检测依赖：" + utils::webview2_runtime_summary(g_dep));
        }
        ImGui::Separator();

        ImGui::Text("第 2 步 · 用示例工作流试跑一次");
        ImGui::TextWrapped("先不配任何 Key 也能看到界面与流程：点下面的按钮载入示例，再点工具栏「运行」。");
        if (ImGui::Button("载入示例工作流")) {
            state.createSampleWorkflow();
            state.setStatus("已载入示例工作流：点工具栏「运行」试跑");
        }
        ImGui::Separator();

        ImGui::Text("第 3 步 · 选择模型与凭据");
        ImGui::TextWrapped(
            "在画布上选中 ProviderConfig 节点 → 右侧「参数」面板里选提供商与模型。\n"
            "官方 API 需要 Key（存进本地凭据库，加密保存、不写明文）；网页版站点用内嵌浏览器登录一次即可。");
        if (ImGui::Button("打开设置面板")) {
            openSettingsPanel();
        }
        ImGui::Separator();

        if (ImGui::Button("开始使用", ImVec2(140.0f, 0.0f))) {
            finish(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("跳过", ImVec2(120.0f, 0.0f))) {
            finish(false);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("以后可从「帮助 → 首次使用引导」再看");
    }
    ImGui::End();

    if (!open) {
        finish(false); // 右上角 ×：等同「跳过」（否则窗口会一直挡住画布）
    }
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

    CanvasOptions canvas_options;
    canvas_options.show_grid         = config.ui.show_grid;
    canvas_options.running_animation = config.ui.running_animation; // PA-08：执行态动效开关

    // 面板可见性来自 config.toml（设计 §7.2：节点库/参数面板默认隐藏、可切换；
    // M6-06 设置面板落地前，手动切换会写回 [ui] 段以便下次记住）
    bool show_console         = config.ui.show_console;
    bool show_output          = config.ui.show_output_window; // PA-02：首次接线该配置项
    bool show_library         = config.ui.show_node_library;
    bool show_params          = config.ui.show_property_panel;
    bool show_info            = true; // 工作流信息面板不在 config 中：始终显示
    bool show_composite_editor = false; // CN：上级节点编辑器（会话打开时自动显示）
    bool layout_ready         = false;
    bool request_layout_reset = false;
    // M6：菜单「帮助 → 首次使用引导」置位（重置本会话「已看过」标记；不影响 first_run_done）
    bool request_first_run_guide = false;

    log::info(std::string("界面可见性（config.toml [ui]）: 节点库=") +
              (show_library ? "显示" : "隐藏") + "，参数面板=" + (show_params ? "显示" : "隐藏") +
              "，Console=" + (show_console ? "显示" : "隐藏") + "，输出=" +
              (show_output ? "显示" : "隐藏") + "，网格=" +
              (canvas_options.show_grid ? "开" : "关"));

    // 启动即创建示例工作流，便于直接验证节点操作（可用工具栏「新建（清空）」重来）
    editor().createSampleWorkflow();

    // --open <路径>：启动即打开指定工作流（覆盖上面的示例工作流）
    //  * 存在性/解析失败只告警并保留示例工作流（不阻塞启动）
    if (!options.open_workflow.empty()) {
        std::string open_error;
        if (editor().openWorkflowFrom(options.open_workflow, &open_error)) {
            log::info("[启动] 已按 --open 打开工作流：" + options.open_workflow + "（节点 " +
                      std::to_string(editor().graph.nodes.size()) + " / 连线 " +
                      std::to_string(editor().graph.edges.size()) + "）");
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

    engine::AutosaveInfo autosave_info   = engine::autosaveProbe();
    std::string          autosave_reason;
    bool                 autosave_ask    = false; // 需要弹恢复询问
    bool                 autosave_popup_opened = false;
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
        editor().markAutosaveBaseline();
        editor().pauseAutosave(false);
    }

    // 诊断：安装首异常计数器（VEH）——用于定位第三方依赖反复抛异常 / 内存增长问题，
    // 结果会以「[诊断]」行写入 app.log（带限流）
    utils::diagnostics::installExceptionCounter();
    utils::diagnostics::logMemorySample("启动基线");

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
        editor().tickRun();

        // ---- PC-01：自动保存节流（1 Hz 探测 + 静默 3 秒后写快照；暂停/无变更时零写盘）----
        editor().tickAutosave();

        // ---- M_patchAB §3.2 / §3.4（PB2-07 / PB2-30②）：后台诊断结论 → 状态栏 ----
        //  * 工作线程只**投递**结论（post_background_status）；此处（主线程）才写 UI 状态
        std::string background_status;
        if (editor().takeBackgroundStatus(&background_status)) {
            editor().setStatus(background_status);
        }

        // ---- 诊断（限流输出）：放在"最小化检查"之前，保证窗口不可见时也能采样 ----
        //  · 首异常累计（有变化才输出，限流）
        //  · 心跳（每 300 帧）：确认渲染循环真的在跑
        //  · 每 30 秒一次内存采样（工作集 / 私有 / 峰值 + 业务计数）
        utils::diagnostics::logExceptionSummary();
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
            utils::diagnostics::logMemorySample(
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

        // P0（2026-10-09）：纹理缓存「逐帧标记」——本帧用到的纹理本轮不淘汰。
        // 必须紧跟 NewFrame()（在画布/面板开始取纹理之前），见 ui/texture_cache.h 说明。
        ui::beginFrame();

        // [调试·自动复现] --auto-open-composite：程序化走「新建上级节点」路径。
        // 第 6 帧打开对话框并在其可见帧自动点「创建并打开编辑器」（与手工同路径）。
        if (options.auto_open_composite && frame_index == 6) {
            compositeEditorTestDriveNewDialog();
            log::info("[自动复现] 已触发「新建上级节点」对话框（第 6 帧）");
        }
        // 若新建因规格校验失败而未打开编辑器：第 12 帧再走「内存会话」路径（旧实现），
        // 以判定断言是否来自「无文件支撑的内存会话」（composite_editor_open 的 find 落空回退）。
        if (options.auto_open_composite && frame_index == 12 && !compositeEditorState().active) {
            engine::CompositeSpec fresh;
            fresh.id   = engine::CompositeRegistry::instance().makeId();
            fresh.name = "内存会话复现";
            compositeEditorOpen(fresh.id);
            log::info("[自动复现] 走内存会话路径打开编辑器 " + fresh.id + "（第 12 帧）");
        }
        // 编辑器已打开后：第 16 帧添加一个内部节点（覆盖非空内部图的 BeginNode 绘制路径），
        // 第 24 帧点「保存」（应因未标记输出被严格校验拦截 → 属预期行为，非崩溃）。
        if (options.auto_open_composite && frame_index == 16 && compositeEditorState().active) {
            const std::string nid = compositeEditorAddNode("TextInput", 0.0f, 0.0f);
            log::info("[自动复现] 编辑器内添加内部节点 " + nid + "（第 16 帧）");
        }
        if (options.auto_open_composite && frame_index == 24 && compositeEditorState().active) {
            std::string save_err;
            const bool  saved = compositeEditorSave(&save_err);
            log::info(std::string("[自动复现] 编辑器保存（预期失败：未标记输出）ok=") +
                      (saved ? "1" : "0") + "  err=" + save_err + "（第 24 帧）");
        }

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
                    state.clearWorkflow();
                }
                if (ImGui::MenuItem("示例工作流")) {
                    state.createSampleWorkflow();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("打开工作流…")) {
                    openWorkflowDialog();
                }
                if (ImGui::MenuItem("保存工作流…")) {
                    saveWorkflowDialog();
                }
                if (ImGui::BeginMenu("最近打开")) {
                    // M_patchAB §4（PC-03 / FEA-M2-02）：打开前**校验存在性** ——
                    // 失效项灰显（不可点）+ 悬停解释 + 底部一键「清理失效条目」
                    const std::vector<engine::RecentStatus> recent =
                        engine::recentWithStatus();
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
                    const std::size_t missing = engine::countMissingRecent();
                    if (missing > 0) {
                        ImGui::Separator();
                        if (ImGui::MenuItem(("清理失效条目（" + std::to_string(missing) + "）").c_str())) {
                            const std::size_t removed = engine::pruneMissingRecent();
                            state.setStatus("已清理 " + std::to_string(removed) + " 条失效的最近记录");
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("清空列表")) {
                        engine::clearRecentFiles();
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("设置…")) { // M6-01：config.toml 的可调项集中在这里
                    openSettingsPanel();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("退出")) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("编辑")) {
                if (ImGui::MenuItem("撤销", nullptr, false, state.undo.canUndo())) {
                    state.undoOnce();
                }
                if (ImGui::MenuItem("重做", nullptr, false, state.undo.canRedo())) {
                    state.redoOnce();
                }
                ImGui::Separator();
                // 设计 §6.1：复制（节点）/ 粘贴（画布）——菜单入口与右键菜单一致
                if (ImGui::MenuItem("复制", nullptr, false, state.has_selection())) {
                    state.copySelection();
                }
                if (ImGui::MenuItem("粘贴", nullptr, false, state.has_clipboard())) {
                    state.pasteClipboard();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("删除选中", nullptr, false, state.has_selection())) {
                    state.deleteSelected();
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
                    compositeNewDialog();
                }
                if (ImGui::MenuItem("打包选区为上级节点…", nullptr, false,
                                    state.selected_nodes.size() >= 2)) {
                    compositePackDialog();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("在画布上框选/多选至少 2 个节点后打包");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("打开上级节点编辑器")) {
                    // 一律走文件路径：弹新建对话框 → 落盘空白规格 JSON → 打开编辑器读取。
                    // 不直接创建无文件支撑的内存会话（避免依赖 imgui-node-editor 上下文时序）。
                    if (!compositeEditorState().active) {
                        compositeNewDialog();
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("帮助")) {
                // M6：版本号取自 app_version.h（与诊断报告 / 安装包同源，避免三处不一致）
                ImGui::MenuItem((std::string("关于 AIwrite ") + kAppVersionLabel).c_str(), nullptr,
                                false, false);
                ImGui::Separator();
                if (ImGui::MenuItem("复制诊断信息")) { // PM-04：给非技术用户的一键求助入口
                    copyText(utils::diagnosticsReportText(), "[帮助] 诊断");
                    state.setStatus("诊断信息已复制到剪贴板（可直接粘贴给支持人员）");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("版本 / 系统 / 依赖 / 路径 / 配置 / 站点表摘要\n"
                                      "（**不含**任何密钥或 Cookie 内容）");
                }
                if (ImGui::MenuItem("首次使用引导")) {
                    request_first_run_guide = true; // 重置「已看过」的会话内标记
                }
                ImGui::Separator();
                if (ImGui::MenuItem("打开日志目录")) {
                    if (utils::openFolder(paths::logsDir().string())) {
                        state.setStatus("已打开日志目录：" + paths::logsDir().string());
                    }
                    else {
                        state.setStatus("打开日志目录失败（可能尚未创建）");
                    }
                }
                if (ImGui::MenuItem("打开数据目录")) {
                    if (utils::openFolder(paths::dataRoot().string())) {
                        state.setStatus("已打开数据目录：" + paths::dataRoot().string());
                    }
                    else {
                        state.setStatus("打开数据目录失败（可能尚未创建）");
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

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
                    engine::autosaveClear(nullptr); // 用户明确拒绝 ⇒ 快照不再保留（否则下次又问）
                    state.markAutosaveBaseline();
                    state.pauseAutosave(false);
                    autosave_ask = false;
                    state.setStatus(status_text);
                    ImGui::CloseCurrentPopup();
                };

                if (ImGui::Button("恢复", ImVec2(160.0f, 0.0f))) {
                    std::string restore_error;
                    if (state.restoreAutosave(&restore_error)) {
                        state.pauseAutosave(false);
                        autosave_ask = false;
                        ImGui::CloseCurrentPopup();
                    }
                    else {
                        state.setStatus("恢复失败：" + restore_error);
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

        // ---- 工具栏（全部鼠标操作；设计 §6.7：MVP 不设快捷键）----
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
        if (ImGui::BeginChild("##toolbar", ImVec2(0.0f, ImGui::GetFrameHeight() + 10.0f), false)) {
            drawToolbarButtons();
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
        drawNodeCanvas(kWindowCanvas, canvas_options);
        ImGui::PopFont();

        if (show_library) {
            ImGui::PushFont(fonts.ui);
            drawNodeLibrary(kWindowLibrary, &show_library);
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
                state.setStatus("参数已修改（可用工具栏「撤销」回退）");
            }
            if (edit.renamed) {
                state.setStatus("节点标题已修改");
            }
            ImGui::PopFont();
        }

        if (show_console) {
            ImGui::PushFont(fonts.console);
            // PA-08：初始高度来自 config.toml [ui] console_height（面板内容区 + 工具条空间）
            drawConsolePanel(kWindowConsole, &show_console,
                               static_cast<float>(config.ui.console_height) + 100.0f);
            ImGui::PopFont();
        }
        if (show_output) { // PA-02：输出面板（结果全文 / 复制全文·该节点 / 导出）
            ImGui::PushFont(fonts.ui);
            drawOutputPanel(kWindowOutput, &show_output, state);
            ImGui::PopFont();
        }
        if (show_info) {
            ImGui::PushFont(fonts.ui);
            draw_info_panel(kWindowInfo, &show_info);
            ImGui::PopFont();
        }

        // ---- CN：上级节点编辑器 + 新建/打包对话框 ----
        if (compositeEditorState().active) {
            show_composite_editor = true; // 会话打开 → 自动显示窗口
        }
        if (show_composite_editor) {
            ImGui::PushFont(fonts.ui);
            drawCompositeEditor(kWindowCompositeEditor, &show_composite_editor);
            ImGui::PopFont();
        }
        if (!compositeEditorState().active) {
            show_composite_editor = false; // 会话关闭 → 隐藏窗口
        }
        drawCompositeDialogs();

        // ---- M6：首次使用引导 + 设置面板（未打开时为空操作 / 零副作用）----
        ImGui::PushFont(fonts.ui);
        draw_first_run_guide(config, state, webview2, request_first_run_guide);
        request_first_run_guide = false;
        const SettingsPanelResult settings_result = drawSettingsPanel(config);
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
            engine::refreshProviderOptions(state.graph);
            state.setStatus("配置表已重载：提供商下拉 / 运行前校验已同步");
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
            // P7a-18：状态栏四项 —— 提供商 / 模式（含站点）/ 最后耗时（+ 节点统计）
            ImGui::TextDisabled("  %s", provider_summary_text(state).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("  %s", provider_mode_text(state).c_str());
            const std::string duration_text = last_run_duration_text(state);
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
        if (first_frame) {
            log::info("[首帧] 渲染完成（节点画布与各面板均已就绪）");
        }
    }

    log::info("主窗口关闭，开始清理");
    editor().stopRunAsync(); // PB-01：退出前 join 工作线程（幂等）
    compositeEditorStopRun(); // 上级节点「执行连线」：退出前 join 试运行线程（幂等）

    // ---- PC-01：退出前补写快照（有未保存的更改才写；正常退出与崩溃都能在下一次启动恢复）----
    editor().flushAutosaveOnExit();

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
    ui::releaseTextures();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

} // namespace aiwrite::ui
