#pragma once

// ============================================================================
//  WebView2 Runtime 运行时探测（M6 `PM-01` / 依赖自检）
//
//  落位说明：放 `utils/`（**不是** `web/`）—— 这是**系统依赖探测**，
//  与具体网页站 / 适配器无关；诊断报告（`utils/diagnostics_report.cpp`）与
//  首启引导（`ui/app.cpp`）都要用，放 `web/` 会造成 utils → web 的反向依赖。
//
//  背景（M6 面向非技术用户）：网页版站点依赖系统里的 **Microsoft Edge WebView2
//  Runtime**。Win11 与较新的 Win10 自带，但**旧 Win10 / 精简版系统 / LTSC 可能没有**
//  ⇒ 没有它时网页版相关功能不可用，程序必须能**自己说清**「缺什么、去哪装」，
//    而不是让用户看到一句英文报错。
//
//  探测顺序（**只读**，不下载不安装）：
//    ① 环境变量 `WEBVIEW2_BROWSER_EXECUTABLE_FOLDER`（固定版本部署，优先）
//    ② 加载器 API `GetAvailableCoreWebView2BrowserVersionString`（**优先采信**）
//    ③ 注册表 `EdgeUpdate\Clients\{F3017226-...}` 的 `pv`（API 不可用时的兜底）
//  * 实测记录：走哪一路会写进 `source` 字段（`api_probe --dep-check` 与诊断报告里可见，
//    不靠推测）；首次实测后把命中值回填到本注释。
//  * 三路都失败 ⇒ `installed = false` + `error` 写明原因（可操作）
// ============================================================================

#include <string>

namespace aiwrite::utils {

struct WebView2RuntimeInfo {
    bool        installed = false;
    std::string version; // 例 "126.0.2592.87"；查不到具体版本时为空
    std::string source;  // "fixed(env)" | "api" | "registry:hklm" | "registry:hklm32" | "registry:hkcu" | 空
    std::string error;   // 未安装 / 探测失败的原因（可读，可直接上界面）
};

// 探测一次（纯读取；快，可在界面每帧之外调用一次后缓存）
WebView2RuntimeInfo detect_webview2_runtime();

// 一行结论（界面 / 日志 / 诊断报告共用同一措辞）
//  已装：`WebView2 运行时 126.0.2592.87（api）`
//  未装：`WebView2 运行时 未安装 —— 网页版站点不可用，请安装：<下载页>`
std::string webview2_runtime_summary(const WebView2RuntimeInfo& info);

// 未安装时的建议（多行；含官方下载链接 + 说明「装完重启程序」）
std::string webview2_install_advice();

// 官方下载页（Microsoft Edge WebView2「Evergreen Bootstrapper」，与线上文档一致）
const char* webview2_download_url();

// 随包附带的 Evergreen Bootstrapper 路径（`PQ-02`「安装包附带」）：
//   `<exe 目录>/webview2/MicrosoftEdgeWebview2Setup.exe`
//  * 返回**空串** = 本包未附带（此时界面/文档只给官方下载页 —— 不假装有）
//  * 返回非空 = 文件确实存在（界面可提供「随包安装」按钮；是否需要管理员由该程序自己决定）
//  * 注意：本机**不**代为下载、**不**静默安装（对方是第三方安装程序，须用户点击同意）
std::string bundled_bootstrapper_path();

} // namespace aiwrite::utils
