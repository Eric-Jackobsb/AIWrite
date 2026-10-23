#pragma once

// ============================================================================
//  主窗口 / 启动组装 —— **模块内契约**(ARCH-Z1 第 3 组)
//
//  * 只允许 `ui/src/app*.cpp` 自己 include；**不是**公开 API（公开入口 = `app.h`）
//  * 逐字搬迁（零语义变化）：原 `app.cpp` 1454 行的常量 / 函数体均原样搬出，
//    跨 TU 只留 [标题常量 7] + [内部函数 13] + [窗口级局部状态 `AppFrameContext`] + [帧函数 7]
//  * 拆分版图：
//      `app.cpp`          启动组装 + 主渲染循环骨架 + 退出清理（`run()`）
//      `app_helpers.cpp`  字体 / 深色主题 / 默认停靠布局 / 状态栏文案 / 信息面板 / 首启引导
//      `app_frame.cpp`    帧首推进（计时 · 自动保存 · 诊断 · NewFrame · 自动复现）+ 菜单栏 + 恢复弹窗
//      `app_panels.cpp`   工具栏 + DockSpace + 主窗口 Ctrl+S + 各面板接线 + 状态栏 + 窗口栈看门狗 + 出帧
//
//  **为什么需要 `AppFrameContext`**：原 `run()` 是 948 行单体（启动 + 每帧全部 UI + 退出），
//  单函数内无法按 TU 切分；把「每帧都要读写的局部状态」一次性收进一个对象（值成员 + 引用成员），
//  各帧函数取 `AppFrameContext&` ⇒ 原局部变量以 `auto&` 别名逐字复原，函数体文本零改动。
// ============================================================================

#include "app.h"                // AppOptions
#include "config.h"             // Config（aiwrite::Config）
#include "editor_controller.h"  // EditorController（含各面板状态对象）
#include "node_canvas.h"        // CanvasOptions
#include "readonly_view.h"      // ReadOnlyView
#include "autosave.h"           // engine::AutosaveInfo
#include "webview2_runtime.h"   // utils::WebView2RuntimeInfo

#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

struct GLFWwindow;  // 只存指针（不把 GLFW 头引进契约头）

namespace aiwrite::ui {

// ---- 窗口 / 面板标题（原 `app.cpp` 匿名命名空间常量；只读常量表 ⇒ 规则 6 合法）----
constexpr const char* kAppTitle      = "AIwrite · AI 小说/剧本创作";
constexpr const char* kWindowCanvas  = "节点画布";
constexpr const char* kWindowLibrary = "节点库";
constexpr const char* kWindowParams  = "参数";
constexpr const char* kWindowConsole = "Console";
constexpr const char* kWindowOutput  = "输出";
constexpr const char* kWindowInfo    = "工作流信息";
constexpr const char* kWindowCompositeEditor = "上级节点编辑"; // CN

// 字体集（原匿名命名空间内的 `Fonts`；`load_fonts` 的唯一返回类型）
struct Fonts {
    ImFont* ui      = nullptr;
    ImFont* console = nullptr;
};

// ---- 内部函数（原 `app.cpp` 匿名命名空间的 13 个自由函数）----
namespace app_detail {

void glfw_error_callback(int code, const char* description);
std::string window_stack_dump();
std::filesystem::path find_font(const std::vector<std::string>&           names,
                                const std::vector<std::filesystem::path>& dirs);
Fonts load_fonts(float scale);
ImVec4 rgb(int r, int g, int b, float alpha = 1.0f);
void apply_dark_theme();
void build_default_layout(ImGuiID dockspace_id, const ImVec2& size);
std::string provider_mode_text(const ReadOnlyView& view);
std::string provider_summary_text(const ReadOnlyView& view);
std::string last_run_duration_text(const ReadOnlyView& view);
void draw_info_panel(const char* title, bool* open, const ReadOnlyView& view,
                     EditorController& state);
void open_url(const std::string& url);
void draw_first_run_guide(Config& config, EditorController& state,
                          const utils::WebView2RuntimeInfo& detected, bool force);

}  // namespace app_detail

// ============================================================================
//  `AppFrameContext` —— 主循环的「窗口级局部状态」
//
//  * 引用成员 = 整场不变的上下文（窗口 / 配置 / 编排器 / 字体 / ImGuiIO / 依赖自检 / 启动参数）
//  * 值成员   = 每帧读写的 UI 状态；**初值逐字取自原 `run()` 局部的初始化式**
//    （`show_console = config.ui.show_console` 等 9 条 = 原文搬入；`autosave_ask` 等 3 条同）
//  * 声明顺序 = 原 `run()` 里局部变量的首次出现顺序（构造顺序与原来一致）
// ============================================================================
struct AppFrameContext {
    // ---- 整场不变（引用 / 指针）----
    GLFWwindow*                       window = nullptr;  // 主窗口
    Config&                           config;
    EditorController&                 state;
    const Fonts&                      fonts;
    ImGuiIO&                          io;
    const utils::WebView2RuntimeInfo& webview2;
    const AppOptions&                 options;

    // ---- 每帧 / 本会话状态（值）----
    CanvasOptions canvas_options;  // 画布选项（原 run() 局部；启动时由 config 赋值）
    bool show_console          = config.ui.show_console;
    bool show_output           = config.ui.show_output_window;
    bool show_library          = config.ui.show_node_library;
    bool show_params           = config.ui.show_property_panel;
    bool show_info             = true;   // 工作流信息面板不在 config 中：始终显示
    bool show_composite_editor = false;  // CN：上级节点编辑器（会话打开时自动显示）
    bool layout_ready          = false;
    bool request_layout_reset  = false;
    // M6：菜单「帮助 → 首次使用引导」置位（重置本会话「已看过」标记；不影响 first_run_done）
    bool request_first_run_guide = false;

    // ---- 自动保存恢复询问（PC-01/02）----
    engine::AutosaveInfo autosave_info{};
    std::string          autosave_reason;
    bool                 autosave_ask            = false;  // 需要弹恢复询问
    bool                 autosave_popup_opened   = false;

    // ---- 帧计数（原 run() 局部 `int frame_index = 0;`）----
    int frame_index = 0;
};

// ---- 帧函数（原 `run()` 主循环体的逐字搬迁；调用顺序 = 原语句顺序）----
//  true = 本帧继续；false = 原 `continue`（窗口最小化分支）
bool appFrameBegin(AppFrameContext& frame);
void appFrameMenuBar(AppFrameContext& frame, const ImGuiViewport* viewport);
void appFrameRestorePopup(AppFrameContext& frame);
void appFrameDockSpace(AppFrameContext& frame, const ImGuiViewport* viewport,
                       float status_height);
void appFramePanels(AppFrameContext& frame, const ReadOnlyView& view);
void appFrameStatusBar(AppFrameContext& frame, const ReadOnlyView& view,
                       const ImGuiViewport* viewport, float status_height);
void appFrameEnd(AppFrameContext& frame);

}  // namespace aiwrite::ui
