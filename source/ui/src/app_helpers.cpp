#include "app_detail.h"

// ---- 平台 / 图形 ----
//  * WIN32_LEAN_AND_MEAN 下 windows.h **不含** shellapi.h ⇒ 显式引入（open_url 用 ShellExecuteW）
#include <windows.h>

#include <shellapi.h>

#include <GL/gl.h>  // glGetString（工作流信息面板「运行环境」段）

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder*（默认停靠布局）

#include <filesystem>
#include <string>
#include <vector>

#include "log.h"
#include "paths.h"
#include "provider_resolve.h"  // L4（PB2-27）：状态栏按生效条目站点判登录态
#include "provider_spec.h"     // strictWebSpecFor / webSessionState
#include "session_store.h"
#include "settings_panel.h"    // 首启引导的「打开设置面板」

namespace aiwrite::ui {
namespace app_detail {

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

ImVec4 rgb(int r, int g, int b, float alpha)
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
// ARCH-R3：渲染读取走 ReadOnlyView（graph 数据经 nodes_for_render）
std::string provider_mode_text(const ReadOnlyView& view)
{
    for (const engine::Node& node : view.nodes_for_render()) {
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
// ARCH-R3：渲染读取走 ReadOnlyView（graph 数据经 nodes_for_render）
std::string provider_summary_text(const ReadOnlyView& view)
{
    for (const engine::Node& node : view.nodes_for_render()) {
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
// ARCH-R3：渲染读取走 ReadOnlyView（快照）
std::string last_run_duration_text(const ReadOnlyView& view)
{
    const engine::RunSnapshot& snapshot = view.snapshot;
    if (snapshot.nodes.empty()) {
        return {};
    }
    double longest = 0.0;
    double total   = 0.0;
    for (const engine::RunNodeView& node_view : snapshot.nodes) {
        if (node_view.duration_ms > longest) {
            longest = node_view.duration_ms;
        }
        total += node_view.duration_ms;
    }
    return "耗时 " + std::to_string(static_cast<long long>(longest)) + " ms（累计 " +
           std::to_string(static_cast<long long>(total)) + " ms / " +
           std::to_string(static_cast<int>(snapshot.nodes.size())) + " 节点）";
}

// 工作流信息面板（环境 + 当前工作流统计；数据来自 ReadOnlyView）
// ARCH-R3：graph / 选中读取走 view；撤销栈深度仍读 state（undo 不在 ReadOnlyView 字段内，
//          E3 拆出 WorkflowModel 后归入文档级对象）
void draw_info_panel(const char* title, bool* open, const ReadOnlyView& view,
                      EditorController& state)
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

    // ARCH-A3：状态来自启动注入的 EditorController（函数参数）
    if (ImGui::CollapsingHeader("当前工作流", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("节点     : %d", static_cast<int>(view.nodes_for_render().size()));
        ImGui::Text("连线     : %d", static_cast<int>(view.edges_for_render().size()));
        ImGui::Text("选中     : %d", static_cast<int>(view.selection.nodes().size()));
        ImGui::Text("撤销栈   : %d（可重做 %d）", static_cast<int>(state.model().undo.undoDepth()),
                    static_cast<int>(state.model().undo.redoDepth()));
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
void draw_first_run_guide(Config& config, EditorController& state,
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
            openSettingsPanel(state.settings_panel());
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
}  // namespace app_detail
}  // namespace aiwrite::ui
