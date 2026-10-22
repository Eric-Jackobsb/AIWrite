// =============================================================================
//  api_probe —— M1 技术验证命令行工具
//
//  验证项：
//    V-04  HTTP 请求（cpp-httplib + OpenSSL）
//    V-05  SHA3-256（OpenSSL EVP，对照标准测试向量）
//    V-06  DeepSeek API 最小调用（非流式，对应 M1-06）
//
//  用法：
//    api_probe --selftest                      # V-04 + V-05
//    api_probe --http <url>
//    api_probe --sha3 <text>
//    api_probe --chat "<prompt>" [--model deepseek-chat]
//                                [--api-base https://api.deepseek.com] [--insecure]
//    api_probe --chat "<prompt>" --dry-run      # 只打印/断言请求构造，不需要 Key
//    api_probe --selftest                      # V-04 + V-05 + V-06 请求构造 + V-08 配置往返

//
//  说明：API Key 只从环境变量 DEEPSEEK_API_KEY 读取，不写入磁盘、不硬编码。
// =============================================================================
#include "crypto.h"
#include "log.h"
#include "window_geometry.h"
#include "paths.h"

#include "graph.h"
#include "autosave.h"     // PC-01/02：自动保存 / 崩溃恢复（--autosave-assert）
#include "node_registry.h"
#include "recent_files.h"
#include "undo_stack.h"
#include "validate.h"
#include "provider_resolve.h" // M5-02：生效提供商 / 模型名断言
#include "workflow_io.h"
#include "migration.h"       // M_patchAB §4（PC-04）：版本迁移（workflow + config）离线断言
#include "composite_selftest.h" // CN：上级节点（Composition_node）自检
#include "nodes.h"
#include "config.h"
#include "output_archive.h"
#include "paths.h"
#include "session_store.h"
#include "web_owner.h"     // M_patchC（PW-01）：归属（纯函数断言）
#include "ready_level.h"   // M_patchC（PW-03）：就绪三级（纯函数断言）
#include "web_error.h"     // M_patchC（PW-05）：三类归因文案（纯函数断言）
#include "probe_verdict.h" // M_patchAB §2.2/§3.3（PB-07 / PB2-25）：会话诊断（判据收紧 + 错误串反查）
#include "window_table.h"  // M_patchC（PW-02 / PW-06 / PW-08）：窗口表 + 复位计划（纯函数断言）
#include "webview_host.h" // plan_session_boot / interactive_login_request（纯逻辑断言用）
#include "cancel_token.h"        // M_patchAB §2.4（PB-09 V-12）：取消令牌 / 取消监视断言
#include "retry_policy.h"        // M_patchAB §2.6（PB-08 残项）：重试策略断言
#include "deepseek_official_provider.h" // M5-02：多模态请求体断言
#include "provider_spec.h"              // M_patchB L1：Provider 配置表断言
#include "dom_web_client.h"             // L3（PB2-13/15）：DOM 适配器纯函数断言（VB2-22）
#include "webview_host.h"              // L4（PB2-28）：探测脚本适用性断言（VB2-25）
#include "channel_frames.h"           // M7B 批 1：管道协议 v1 帧（VB2-29 / VB2-32 · §6.1）
#include "session_snapshot.h"        // M7B 批 2：L2 快照只读视图（VB2-39⑥ · MB-D0-8）
#include "pydoll_channel.h"          // M7B 批 2：新通道如实报错断言（VB2-40④⑤ · I21）
#include "deepseek_web_client.h"       // L4（PB2-28④）：会话失效识别断言（VB2-27）
#include "image_decode.h"           // M7-04/05：图片格式嗅探 + 解码 + 失败文案断言
#include "image_convert.h"      // M8-14：图片准备（白名单 / 限额 → 缩放 + 转码）离线断言
#include "image_preprocess.h"   // M_8nodes_extension L1-c（§4.2）：图片预处理（用户参数 → 裁剪/缩放）
#include "upload_evidence.h"    // M8-14：上传双证据（I18）离线断言
#include "upload_registry.h"    // M8-14：站点上传注册表 + 契约离线断言
#include "asset_store.h"            // P7a-04：统一资源目录（令牌 / 归档 / 解析）断言
#include "app_version.h"        // M6：诊断报告断言（版本号单一来源）
#include "diagnostics_report.h" // M6（PM-04）：诊断报告离线断言
#include "settings_validation.h" // M6（M6-01）：设置面板校验纯函数断言
#include "webview2_runtime.h"    // M6（PM-01）：依赖自检（WebView2 运行时）
#include "site_template.h"        // PB2-24：自建网页版站点模板 + 落盘断言
#include "inference_provider.h"   // L2（B2-b · PB2-08/12）：统一推理接口 + 能力门控断言
#include "provider_factory.h"     // L2（B2-b · PB2-09）：按表 protocol 分派断言（R9）
#include "anthropic_provider.h"   // L2（B2-b · PB2-10）：Anthropic Messages 请求构造断言
#include "gemini_provider.h"      // L2（B2-b · PB2-11）：Gemini generateContent 请求构造断言
#include "http_json.h"            // L2（B2-b）：通用 JSON POST / query 拼接断言
#include "http_net.h"             // M_patchAB §2.3（PB-06 剩余）：代理 / 自签证书策略断言
#include "credential.h"           // M_patchAB §2.3（PB-06 剩余）：凭据库配置（目录 / 后端）
#include "text_export.h"          // M_8nodes_extension L1-b（§4.8）：N-08 导出模板 / 自动命名断言

#include <fstream>
#include <sstream>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 引擎层位于 namespace aiwrite::engine，这里起别名便于书写
namespace engine = aiwrite::engine;

#include "api_internal.h"

int main(int argc, char** argv)
{
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %s 缺少取值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--selftest") {
            options.selftest = true;
        }
        else if (arg == "--dry-run") {
            options.dry_run = true;
        }
        else if (arg == "--graph-selftest") {
            options.graph_selftest = true;
        }
        else if (arg == "--exec-selftest") {
            options.exec_selftest = true;
        }
        else if (arg == "--image-decode") {
            options.image_path = next("--image-decode");
        }
        else if (arg == "--insecure") {
            options.insecure = true;
        }
        else if (arg == "--sha3") {
            options.sha3_text = next("--sha3");
        }
        else if (arg == "--http") {
            options.http_url = next("--http");
        }
        else if (arg == "--chat") {
            options.chat_prompt = next("--chat");
        }
        else if (arg == "--model") {
            options.model = next("--model");
        }
        else if (arg == "--api-base") {
            options.api_base = next("--api-base");
        }
        else if (arg == "--diag-dump") { // M6：打印诊断报告
            options.diag_dump = true;
        }
        else if (arg == "--diag-assert") { // M6：诊断报告离线断言
            options.diag_assert = true;
        }
        else if (arg == "--dep-check") { // M6：依赖自检（WebView2 运行时）
            options.dep_check = true;
        }
        else if (arg == "--settings-assert") { // M6：设置面板校验断言
            options.settings_assert = true;
        }
        else if (arg == "--site-assert") { // PB2-24：自建站点闭环断言
            options.site_assert = true;
        }
        else if (arg == "--official-assert") { // 自定义 AI 收口：自建官方 API 提供商闭环断言
            options.official_assert = true;
        }
        else if (arg == "--autosave-assert") { // PC-01/02：自动保存 / 崩溃恢复断言
            options.autosave_assert = true;
        }
        else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            printUsage();
            return 2;
        }
    }

    aiwrite::log::init(false);
    aiwrite::log::info("api_probe 启动（M1 技术验证）");

    // PA-07：工具侧也载入真实配置到进程缓存（只读用途；provider 等后续补丁读取）
    {
        aiwrite::Config probe_config;
        if (aiwrite::loadConfig(aiwrite::paths::configFile(), probe_config)) {
            aiwrite::setAppConfig(probe_config);
        }
    }

    if (options.sha3_text == "" && !options.selftest && !options.graph_selftest &&
        !options.exec_selftest && options.http_url.empty() && options.chat_prompt.empty() &&
        options.image_path.empty() && !options.diag_dump && !options.diag_assert &&
        !options.dep_check && !options.settings_assert && !options.site_assert &&
        !options.official_assert && !options.autosave_assert) {
        printUsage();
        aiwrite::log::shutdown();
        return 2;
    }

    int exit_code = 0;

    if (!options.sha3_text.empty()) {
        const std::string actual = aiwrite::crypto::sha3_256(options.sha3_text);
        std::printf("SHA3-256(\"%s\") = %s\n", options.sha3_text.c_str(), actual.c_str());
    }

    if (options.selftest) {
        std::printf("=== AIwrite M1 自检 ===\n");
        const bool sha3_ok   = test_sha3();
        const int  http_code = test_http("https://api.deepseek.com", options.insecure);

        Options request_options = options;
        if (request_options.chat_prompt.empty()) {
            request_options.chat_prompt = "用一句话介绍你自己";
        }
        const bool request_ok = test_chat_request_shape(request_options);
        const bool config_ok  = test_config_roundtrip();
        const bool web_ok     = test_web_session_and_visibility();
        const bool exec_ok =
            (executor_selftest() == 0) && (execution_selftest() == 0); // P3-1 + P3-2（离线）
        const bool files_ok = test_workflow_files();                   // M2-05（离线）
        // M_patchAB §2.4（PB-09）：V-11 无 Key 请求构造 / V-12 取消与超时语义（七组 → 九组）
        const bool nokey_ok  = test_nokey_request_shape();
        const bool cancel_ok = test_cancel_and_timeout();
        // M_patchAB §3.1（`B2-b` L2 底座）：V-13 两协议请求构造 / V-14 工厂分派与能力门控 /
        //   V-15 回环端到端（九组 → 十二组）
        const bool protocol_ok = test_protocol_requests();
        const bool factory_ok  = test_factory_and_capability();
        const bool loopback_ok = test_protocol_loopback();
        // M_patchAB §2.3（`PB-06` 剩余）：V-16 代理 / 证书 / 凭据配置（十二组 → 十三组）
        const bool net_ok = test_network_and_credentials();

        const char* http_text =
            (http_code == 0) ? "PASS" : (http_code == 2 ? "SKIP(网络不可达)" : "FAIL");
        std::printf("=== 结果: SHA3 %s / HTTP %s / 请求构造 %s / 配置往返 %s / 会话与可见性 %s / "
                    "执行器 %s / 工作流文件 %s / 无 Key 请求构造 %s / 取消与超时 %s / "
                    "L2 请求构造 %s / L2 工厂分派 %s / L2 回环 %s / 网络与凭据 %s ===\n",
                    sha3_ok ? "PASS" : "FAIL", http_text, request_ok ? "PASS" : "FAIL",
                    config_ok ? "PASS" : "FAIL", web_ok ? "PASS" : "FAIL", exec_ok ? "PASS" : "FAIL",
                    files_ok ? "PASS" : "FAIL", nokey_ok ? "PASS" : "FAIL",
                    cancel_ok ? "PASS" : "FAIL", protocol_ok ? "PASS" : "FAIL",
                    factory_ok ? "PASS" : "FAIL", loopback_ok ? "PASS" : "FAIL",
                    net_ok ? "PASS" : "FAIL");
        if (!sha3_ok || http_code == 1 || !request_ok || !config_ok || !web_ok || !exec_ok ||
            !files_ok || !nokey_ok || !cancel_ok || !protocol_ok || !factory_ok || !loopback_ok ||
            !net_ok) {
            exit_code = 1;
        }
    }
    else if (!options.http_url.empty()) {
        const int http_code = test_http(options.http_url, options.insecure);
        exit_code           = (http_code == 0) ? 0 : (http_code == 2 ? 2 : 1);
    }

    if (options.graph_selftest) {
        if (graph_selftest() != 0) {
            exit_code = 1;
        }
    }

    if (options.exec_selftest) {
        if (executor_selftest() != 0) {
            exit_code = 1;
        }
        if (composite_selftest() != 0) { // CN：上级节点自检（展开执行等）
            exit_code = 1;
        }
        if (execution_selftest() != 0) {
            exit_code = 1;
        }
    }

    if (!options.image_path.empty()) { // M7：图片诊断（只读，不联网）
        const int image_code = image_decode_report(options.image_path);
        if (image_code != 0) {
            exit_code = image_code;
        }
    }

    if (!options.chat_prompt.empty()) {
        const int chat_code = test_chat(options);
        if (chat_code == 1) {
            exit_code = 1;
        }
    }

    // ---- M6（非技术用户可安装化）：诊断 / 依赖 / 设置校验 ----
    if (options.diag_dump) { // PM-04：诊断报告（只读采集；可直接 > 文件）
        std::printf("%s", aiwrite::utils::diagnosticsReportText().c_str());
    }
    if (options.diag_assert) { // PM-04：报告离线断言
        if (!test_diag_report()) {
            exit_code = 1;
        }
    }
    if (options.dep_check) { // PM-01：依赖自检（缺 WebView2 ⇒ 1）
        if (!test_dep_check()) {
            exit_code = 1;
        }
    }
    if (options.settings_assert) { // M6-01：设置校验断言
        if (!test_settings_assert()) {
            exit_code = 1;
        }
    }
    if (options.site_assert) { // PB2-24：自建站点闭环断言（自清理）
        if (!test_site_assert()) {
            exit_code = 1;
        }
    }
    if (options.official_assert) { // 自定义 AI 收口：自建官方 API 提供商闭环断言（自清理）
        if (!test_official_assert()) {
            exit_code = 1;
        }
    }
    if (options.autosave_assert) { // PC-01/02：自动保存 / 崩溃恢复断言（自清理）
        if (!test_autosave_assert()) {
            exit_code = 1;
        }
    }

    aiwrite::log::info("api_probe 结束，返回码 " + std::to_string(exit_code));
    aiwrite::log::shutdown();
    return exit_code;
}
