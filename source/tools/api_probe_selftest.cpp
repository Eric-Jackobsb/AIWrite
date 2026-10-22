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




// NIST FIPS 202 标准测试向量
constexpr const char* kSha3Abc   = "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532";
constexpr const char* kSha3Empty = "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a";

// ============================================================================
//  M_patchAB §2.4（PB-09）：V-11 无 Key 请求构造 / V-12 取消与超时语义
//
//  * 全部**离线**：只连本机回环（`127.0.0.1` 上临时起 httplib::Server）
//  * 动机：`PB-02`（真取消）与「无 Key 时不构造凭证头」这两条此前**没有自动化证据**
//  * V-12 判据（把 `AB-01` 离线化）：取消后 **1.5 秒内**返回且 `result.cancelled == true`
// ============================================================================


bool has_header_named(const std::vector<std::pair<std::string, std::string>>& headers,
                      const std::string& name)
{
    for (const std::pair<std::string, std::string>& header : headers) {
        if (header.first == name) {
            return true;
        }
    }
    return false;
}

std::string chat_completion_body(const std::string& text)
{
    nlohmann::json body;
    body["choices"] = nlohmann::json::array();
    body["choices"].push_back(nlohmann::json{{"message", nlohmann::json{{"content", text}}}});
    return body.dump();
}

// 回环请求构造：端点 = `/slow` 或 `/fast`
aiwrite::ai::OfficialChatRequest loopback_request(int port, const std::string& path)
{
    aiwrite::ai::OfficialChatRequest request;
    request.api_base          = "http://127.0.0.1:" + std::to_string(port);
    request.options.api_base  = request.api_base;
    request.options.chat_path = path;
    request.api_key           = "sk-selftest-local";
    request.model             = "deepseek-chat";
    request.prompt            = "请只回复 pong";
    return request;
}


// ---- V-11：无 Key 请求构造（纯函数，不联网）----
bool test_nokey_request_shape()
{
    std::printf("\n===== V-11 无 Key 请求构造（离线）=====\n");
    int        passed = 0;
    int        failed = 0;
    const auto check  = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };

    aiwrite::ai::ProviderOptions options; // 默认 = 改造前行为
    const auto no_key = aiwrite::ai::build_auth_headers(options, std::string());
    check(!has_header_named(no_key, "Authorization"),
          "无 Key ⇒ **不**构造 Authorization 头（不会出现空 Bearer）");
    bool all_non_empty = true;
    for (const std::pair<std::string, std::string>& header : no_key) {
        all_non_empty = all_non_empty && !header.second.empty();
    }
    check(all_non_empty, "无 Key ⇒ 不产生任何空值请求头");

    const auto with_key = aiwrite::ai::build_auth_headers(options, "sk-selftest");
    bool       bearer_ok = false;
    for (const std::pair<std::string, std::string>& header : with_key) {
        if (header.first == "Authorization" && header.second == "Bearer sk-selftest") {
            bearer_ok = true;
        }
    }
    check(bearer_ok, "有 Key ⇒ Authorization: Bearer <key>（形状正确）");

    aiwrite::ai::ProviderOptions api_key_style;
    api_key_style.auth_style  = "api-key";
    api_key_style.auth_header = "api-key";
    check(has_header_named(aiwrite::ai::build_auth_headers(api_key_style, "sk-x"), "api-key"),
          "auth_style=api-key（Anthropic 口径）⇒ 按表生成 api-key 头（PB2-04 复核）");

    check(aiwrite::ai::resolveChatPath("/v1/messages", "claude") == "/v1/messages" &&
              aiwrite::ai::resolveChatPath("/v1beta/models/{model}:generateContent", "gemini-pro") ==
                  "/v1beta/models/gemini-pro:generateContent",
          "{model} 占位替换可用（Anthropic / Gemini 路径参数化）");

    aiwrite::ai::OfficialChatRequest request;
    request.model  = "deepseek-chat";
    request.prompt = "ping";
    const nlohmann::json body = aiwrite::ai::buildRequestBody(request);
    check(body.contains("model") && body.contains("messages") &&
              body.dump().find("sk-") == std::string::npos,
          "请求体：model / messages 就位，且**不含**任何 Key 片段");

    std::printf("  V-11 小计：%d 通过 / %d 失败\n", passed, failed);
    return failed == 0;
}

// ---- V-12：取消与超时语义（本机回环；不开浏览器、不出网）----
bool test_cancel_and_timeout()
{
    std::printf("\n===== V-12 取消与超时语义（回环）=====\n");
    int        passed = 0;
    int        failed = 0;
    const auto check  = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };

    // ---- ① 令牌本身（不联网）----
    aiwrite::ai::CancelTokenPtr token = std::make_shared<aiwrite::ai::CancelToken>();
    check(!token->cancelled(), "令牌：初始未取消");
    token->request_cancel();
    check(token->cancelled(), "令牌：request_cancel() 后置位（跨线程可见）");
    token->request_cancel();
    check(token->cancelled(), "令牌：重复置位幂等（不抛、不翻转）");

    // ---- ② CancelWatch：置位才调用中断动作，且只调一次 ----
    std::atomic<int> abort_calls{0};
    {
        aiwrite::ai::CancelTokenPtr watch_token = std::make_shared<aiwrite::ai::CancelToken>();
        aiwrite::ai::CancelWatch    quiet(watch_token.get(), [&abort_calls] { ++abort_calls; }, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        check(abort_calls.load() == 0, "监视：未取消 ⇒ **不**触发中断动作");
        watch_token->request_cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        check(abort_calls.load() == 1 && quiet.aborted(), "监视：置位后触发一次中断动作并置 aborted()");
    }
    check(abort_calls.load() == 1, "监视：析构后不再触发（线程已收尾）");

    // ---- ③ 回环服务器（/slow 4 秒、/fast 立即）----
    httplib::Server  server;
    std::atomic<int> slow_hits{0};
    std::atomic<int> fast_hits{0};
    server.Post("/slow", [&slow_hits](const httplib::Request&, httplib::Response& res) {
        slow_hits.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::seconds(4));
        res.set_content(chat_completion_body("pong"), "application/json");
    });
    server.Post("/fast", [&fast_hits](const httplib::Request&, httplib::Response& res) {
        fast_hits.fetch_add(1);
        res.set_content(chat_completion_body("pong"), "application/json");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("  ✗ 回环服务器未起（端口不可用）—— V-12 无法继续\n");
        return false;
    }
    std::thread server_thread([&server] { server.listen_after_bind(); });
    server.wait_until_ready();

    // ---- ④ 正常路径不受影响（无令牌）----
    {
        const aiwrite::ai::OfficialChatResult result =
            aiwrite::ai::officialChat(loopback_request(port, "/fast"));
        check(result.ok && !result.cancelled && result.text == "pong",
              "正常路径：无令牌 ⇒ 请求成功且 cancelled=false（HTTP " +
                  std::to_string(result.http_status) + " / " +
                  std::to_string(static_cast<long long>(result.elapsed_ms)) + " ms）");
    }

    // ---- ⑤ 发起前已取消 ⇒ 不发请求 ----
    {
        const int hits_before                    = slow_hits.load();
        aiwrite::ai::OfficialChatRequest request = loopback_request(port, "/slow");
        request.cancel                           = token; // 已置位
        const auto started = std::chrono::steady_clock::now();
        const aiwrite::ai::OfficialChatResult result = aiwrite::ai::officialChat(request);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        check(result.cancelled && ms < 300.0 && slow_hits.load() == hits_before,
              "取消（发起前）：立即返回且 **服务器未收到请求**（" +
                  std::to_string(static_cast<long long>(ms)) + " ms）");
    }

    // ---- ⑥ 请求进行中取消 ⇒ 主动关 socket，< 1.5 s 返回 ----
    {
        aiwrite::ai::CancelTokenPtr      live    = std::make_shared<aiwrite::ai::CancelToken>();
        aiwrite::ai::OfficialChatRequest request = loopback_request(port, "/slow");
        request.cancel                           = live;
        std::thread canceller([live] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            live->request_cancel();
        });
        const auto started = std::chrono::steady_clock::now();
        const aiwrite::ai::OfficialChatResult result = aiwrite::ai::officialChat(request);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        canceller.join();
        check(result.cancelled && !result.ok && ms < 1500.0,
              "取消（请求中）：**可取消执行** ⇒ 1.5 s 内返回且 cancelled=true（" +
                  std::to_string(static_cast<long long>(ms)) + " ms）");
    }

    // ---- ⑦ 读超时语义（`timeout.*` 生效的直接证据）----
    {
        aiwrite::ai::OfficialChatRequest request = loopback_request(port, "/slow");
        request.options.read_timeout_s           = 1;
        const auto started = std::chrono::steady_clock::now();
        const aiwrite::ai::OfficialChatResult result = aiwrite::ai::officialChat(request);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        check(!result.ok && !result.cancelled && result.error.find("超时") != std::string::npos &&
                  ms < 3000.0,
              "读超时：1 秒读超时对 4 秒响应 ⇒ 可操作错误（含「超时」，" +
                  std::to_string(static_cast<long long>(ms)) + " ms）");
    }

    // ---- ⑧ 取消不污染后续调用（新令牌路径仍通）----
    {
        const aiwrite::ai::OfficialChatResult result =
            aiwrite::ai::officialChat(loopback_request(port, "/fast"));
        check(result.ok && !result.cancelled && fast_hits.load() >= 1,
              "取消后新调用：**新令牌**路径不受上一轮取消影响");
    }

    // ---- ⑨ `PB-08` 残项：重试策略（可重试状态 / 退避 / 次数钳制 / 可中断等待）----
    check(aiwrite::ai::httpStatusRetryable(429) && aiwrite::ai::httpStatusRetryable(500) &&
              aiwrite::ai::httpStatusRetryable(503) && aiwrite::ai::httpStatusRetryable(0),
          "重试策略：429 / 5xx / 网络超时（0）⇒ **可重试**");
    check(!aiwrite::ai::httpStatusRetryable(400) && !aiwrite::ai::httpStatusRetryable(401) &&
              !aiwrite::ai::httpStatusRetryable(402) && !aiwrite::ai::httpStatusRetryable(403) &&
              !aiwrite::ai::httpStatusRetryable(404) && !aiwrite::ai::httpStatusRetryable(200),
          "重试策略：请求不合法 / 凭证 / 余额 / 风控 / 成功 ⇒ **不重试**（避免加剧风控）");
    check(aiwrite::ai::retryDelayMs(2000, 0) == 2000 &&
              aiwrite::ai::retryDelayMs(2000, 1) == 4000 &&
              aiwrite::ai::retryDelayMs(2000, 99) == 30000 &&
              aiwrite::ai::retryDelayMs(0, 0) == 2000,
          "重试策略：线性退避 + 上限 30 s（base<=0 ⇒ 2000）");
    check(aiwrite::ai::clampRetryCount(false, 3) == 0 &&
              aiwrite::ai::clampRetryCount(true, -1) == 0 &&
              aiwrite::ai::clampRetryCount(true, 2) == 2 &&
              aiwrite::ai::clampRetryCount(true, 99) == 5,
          "重试策略：`error.retry_enabled=false` ⇒ 0；负数 ⇒ 0；上限 5（钳制）");
    {
        aiwrite::ai::CancelTokenPtr sleep_token = std::make_shared<aiwrite::ai::CancelToken>();
        std::thread                 trigger([sleep_token] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            sleep_token->request_cancel();
        });
        const auto started = std::chrono::steady_clock::now();
        const bool interrupted = aiwrite::ai::sleepInterruptible(sleep_token.get(), 2000, 20);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        trigger.join();
        check(interrupted && ms < 800.0,
              "可中断等待：2 s 等待在取消后 < 0.8 s 返回（重试间隔不被取消拖住，实测 " +
                  std::to_string(static_cast<long long>(ms)) + " ms）");
        check(!aiwrite::ai::sleepInterruptible(nullptr, 60),
              "可中断等待：无令牌 ⇒ 普通睡眠且不报中断");
    }

    std::printf("  V-12 小计：%d 通过 / %d 失败\n", passed, failed);
    server.stop();
    server_thread.join();
    return failed == 0;
}

// ============================================================================
//  L2 底座（`B2-b`）离线断言 —— V-13 / V-14 / V-15
//
//  * V-13 Anthropic（Messages）/ Gemini（generateContent）**请求构造与响应解析**（纯函数）
//  * V-14 工厂分派（按表 protocol）+ 能力门控（`PB2-09` / `PB2-12` / `R9`）
//  * V-15 两条新协议的**回环端到端**（本机 httplib 服务器；不出网、不碰真实站点）
// ============================================================================

// ---- V-13：两条新协议的请求构造 / 响应解析（纯函数，不联网）----
bool test_protocol_requests()
{
    std::printf("\n===== V-13 Anthropic / Gemini 请求构造与解析（离线）=====\n");
    int        passed = 0;
    int        failed = 0;
    const auto check  = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };
    const auto has = [](const std::string& text, const std::string& needle) {
        return text.find(needle) != std::string::npos;
    };

    // ---- ① 端点拼接（含 `{model}` 占位 / 尾斜杠 / query）----
    check(aiwrite::ai::anthropic_build_endpoint("https://api.anthropic.com/", "", "claude-x") ==
              "https://api.anthropic.com/v1/messages",
          "Anthropic 端点：尾部斜杠归一 + 默认 /v1/messages");
    check(aiwrite::ai::anthropic_build_endpoint("https://api.anthropic.com", "/v1/messages",
                                                "claude-3-5") ==
              "https://api.anthropic.com/v1/messages",
          "Anthropic 端点：显式路径（无 {model} 占位）原样拼接");
    check(aiwrite::ai::gemini_build_endpoint("https://generativelanguage.googleapis.com", "",
                                             "gemini-2.0-flash", "key", "sk-g") ==
              "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:"
              "generateContent?key=sk-g",
          "Gemini 端点：`{model}` 替换 + `?key=` 查询参数（auth_style=query）");
    check(aiwrite::ai::gemini_build_endpoint("https://x.example.com", "", "m", "key", "") ==
              "https://x.example.com/v1beta/models/m:generateContent",
          "Gemini 端点：无 Key ⇒ 不拼 query（不会出现空 `key=`）");
    check(aiwrite::ai::append_query_param("https://x.example.com/a?api-version=1", "key", "k") ==
              "https://x.example.com/a?api-version=1&key=k" &&
              aiwrite::ai::append_query_param("https://x.example.com/a", "key", "") ==
                  "https://x.example.com/a",
          "query 拼接：已有 `?` ⇒ 用 `&`；空值 ⇒ 原样返回");

    // ---- ② 能力参数（seed 支持 / 不支持两种口径）----
    aiwrite::ai::GenerateParams params;
    params.prompt        = "请只回复 pong";
    params.system_prompt = "你是测试助手";
    params.temperature   = 2.0; // Anthropic 取值域 0…1 ⇒ 期望钳制到 1
    params.max_tokens    = 0;   // 缺省 ⇒ 期望兜底 2048（Anthropic 必填）
    params.top_p         = 0.3;
    params.seed          = 42;

    const nlohmann::json anthropic_body =
        aiwrite::ai::anthropic_build_body(params, "claude-sonnet-4-5", false, {});
    check(anthropic_body.value("model", std::string()) == "claude-sonnet-4-5" &&
              anthropic_body.value("max_tokens", 0) == 2048,
          "Anthropic 请求体：model + `max_tokens` 必填（缺省兜底 2048）");
    check(anthropic_body.contains("temperature") &&
              anthropic_body["temperature"].get<double>() == 1.0,
          "Anthropic 请求体：temperature 越界（2.0）**钳制**到 1.0");
    check(anthropic_body.value("system", std::string()) == "你是测试助手" &&
              !anthropic_body["messages"][0].contains("system"),
          "Anthropic 请求体：system 是**顶层字段**（不在 messages 里）");
    check(anthropic_body["messages"][0]["role"].get<std::string>() == "user" &&
              anthropic_body["messages"][0]["content"].is_string(),
          "Anthropic 请求体：纯文本路径 content = 字符串（与官方示例一致）");
    check(!anthropic_body.contains("seed") && anthropic_body["top_p"].get<double>() == 0.3,
          "Anthropic 请求体：**无 seed 字段**（capabilities.seed=false）+ top_p 透传");

    // ---- ③ 图片块形状（两条协议各不相同）----
    const std::string    data_url = "data:image/png;base64,QUJD";
    const nlohmann::json anthropic_vision =
        aiwrite::ai::anthropic_build_body(params, "claude-sonnet-4-5", false, {data_url});
    const nlohmann::json& blocks = anthropic_vision["messages"][0]["content"];
    check(blocks.is_array() && blocks.size() == 2 &&
              blocks[0]["type"].get<std::string>() == "image" &&
              blocks[0]["source"]["type"].get<std::string>() == "base64" &&
              blocks[0]["source"]["media_type"].get<std::string>() == "image/png" &&
              blocks[0]["source"]["data"].get<std::string>() == "QUJD",
          "Anthropic 视觉：`content[0] = {type:image, source:{type:base64, media_type, data}}`"
          "（data **不带** data URL 前缀）");
    check(blocks[1]["type"].get<std::string>() == "text" &&
              blocks[1]["text"].get<std::string>() == params.prompt,
          "Anthropic 视觉：文本块在图片之后（顺序稳定）");

    const nlohmann::json gemini_body = aiwrite::ai::gemini_build_body(params, true, {data_url});
    check(gemini_body["contents"][0]["role"].get<std::string>() == "user" &&
              gemini_body["contents"][0]["parts"][0]["text"].get<std::string>() == params.prompt &&
              gemini_body["contents"][0]["parts"][1]["inline_data"]["mime_type"]
                      .get<std::string>() == "image/png" &&
              gemini_body["contents"][0]["parts"][1]["inline_data"]["data"].get<std::string>() ==
                  "QUJD",
          "Gemini 请求体：`parts[0].text` + `parts[1].inline_data{mime_type,data}`");
    check(gemini_body["systemInstruction"]["parts"][0]["text"].get<std::string>() == "你是测试助手" &&
              gemini_body["generationConfig"].value("maxOutputTokens", 0) == 2048 &&
              gemini_body["generationConfig"].value("seed", 0) == 42,
          "Gemini 请求体：systemInstruction + generationConfig（maxOutputTokens / seed）");
    const nlohmann::json gemini_no_seed = aiwrite::ai::gemini_build_body(params, false, {});
    check(!gemini_no_seed["generationConfig"].contains("seed") &&
              gemini_no_seed["contents"][0]["parts"].size() == 1,
          "Gemini 请求体：能力表不允许 seed / 无图片时不产生多余字段");

    // ---- ④ 响应解析（多块拼接 / 忽略非文本块 / 候选路径）----
    const nlohmann::json anthropic_resp = nlohmann::json::parse(R"JSON({
      "id": "msg_1", "model": "claude-sonnet-4-5",
      "content": [ {"type":"text","text":"po"}, {"type":"tool_use","name":"x"},
                   {"type":"text","text":"ng"} ],
      "stop_reason": "end_turn"
    })JSON");
    check(aiwrite::ai::anthropicExtractText(anthropic_resp) == "pong",
          "Anthropic 响应解析：`content[]` 的 text 块**逐块拼接**且忽略非文本块");

    const nlohmann::json gemini_resp = nlohmann::json::parse(R"JSON({
      "candidates": [ { "content": { "role":"model", "parts": [ {"text":"po"}, {"text":"ng"} ] },
                        "finishReason": "STOP" } ]
    })JSON");
    check(aiwrite::ai::geminiExtractText(gemini_resp) == "pong",
          "Gemini 响应解析：`candidates[0].content.parts[*].text` 逐块拼接");

    // ---- ⑤ 错误 / 拦截原因（可操作，不吞掉原因）----
    const nlohmann::json anthropic_error = nlohmann::json::parse(
        R"JSON({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})JSON");
    const std::string anthropic_error_message =
        aiwrite::ai::anthropicErrorText(401, anthropic_error);
    check(has(anthropic_error_message, "invalid x-api-key") &&
              has(anthropic_error_message, "authentication_error") &&
              has(anthropic_error_message, "401"),
          "Anthropic 错误：HTTP 状态 + 错误类型 + 消息都带出来（可操作）");
    const nlohmann::json gemini_blocked = nlohmann::json::parse(
        R"JSON({"promptFeedback":{"blockReason":"SAFETY"},"candidates":[]})JSON");
    check(has(aiwrite::ai::geminiBlockReason(gemini_blocked), "SAFETY"),
          "Gemini 拦截：`promptFeedback.blockReason` ⇒ 明确指出被站点安全策略拦截");
    const nlohmann::json gemini_finish = nlohmann::json::parse(
        R"JSON({"candidates":[{"content":{"parts":[]},"finishReason":"MAX_TOKENS"}]})JSON");
    check(has(aiwrite::ai::geminiBlockReason(gemini_finish), "MAX_TOKENS"),
          "Gemini 未出文本：`finishReason != STOP` ⇒ 给出原因而不是「无内容」");
    check(aiwrite::ai::jsonErrorMessage(nlohmann::json::parse(
              R"JSON({"error":{"code":400,"message":"API key not valid"}})JSON")) ==
              "API key not valid",
          "错误提取：兼容三种形状（OpenAI 兼容 / Anthropic / Gemini）");

    // ---- ⑥ data URL 拆分（两种协议共用）----
    std::string mime;
    std::string base64;
    check(aiwrite::ai::splitDataUrl("data:image/jpeg;base64,AAA", &mime, &base64) &&
              mime == "image/jpeg" && base64 == "AAA",
          "data URL 拆分：`data:image/jpeg;base64,AAA` → (image/jpeg, AAA)");
    check(!aiwrite::ai::splitDataUrl("data:image/png,AAA", &mime, &base64) &&
              !aiwrite::ai::splitDataUrl("/tmp/a.png", &mime, &base64),
          "data URL 拆分：缺 base64 声明 / 非 data URL ⇒ false（不静默冒充图片）");

    std::printf("  V-13 小计：%d 通过 / %d 失败\n", passed, failed);
    return failed == 0;
}

// ---- V-14：工厂分派 + 能力门控（纯离线；含对**随程序发布**的 providers.json 的 R9 核对）----
bool test_factory_and_capability()
{
    std::printf("\n===== V-14 工厂分派 / 能力门控（离线）=====\n");
    int        passed = 0;
    int        failed = 0;
    const auto check  = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };
    const auto has = [](const std::string& text, const std::string& needle) {
        return text.find(needle) != std::string::npos;
    };

    // ---- ① 工厂清单与实现清单一致（单体真值 = 工厂；`R9`）----
    bool factory_subset = true;
    for (const std::string& protocol : aiwrite::ai::factoryProtocols()) {
        bool found = false;
        for (const std::string& item : aiwrite::ai::implementedProtocols()) {
            found = found || (item == protocol);
        }
        factory_subset = factory_subset && found;
    }
    check(factory_subset && aiwrite::ai::factoryImplements("openai") &&
              aiwrite::ai::factoryImplements("anthropic") &&
              aiwrite::ai::factoryImplements("gemini") &&
              aiwrite::ai::factoryImplements("deepseek-web") &&
              !aiwrite::ai::factoryImplements("acme-v2"),
          "工厂：实现清单 ⊂ 展示清单，且 openai/anthropic/gemini/deepseek-web 均实现（`acme-v2` 否）");

    // ---- ② 随程序发布的表：每条 protocol 都必须有实现（R9 的真闸门）----
    const aiwrite::ai::ProviderSpecs table = aiwrite::ai::loadProviderSpecs();
    check(!table.items.empty(), "表加载：条目不空（实际 " + std::to_string(table.items.size()) + " 条）");
    std::string unimplemented;
    for (const aiwrite::ai::ProviderSpec& spec : table.items) {
        bool found = false;
        for (const std::string& item : aiwrite::ai::implementedProtocols()) {
            found = found || (item == spec.protocol);
        }
        if (!found) {
            unimplemented += (unimplemented.empty() ? "" : "、") + spec.id + "(" + spec.protocol + ")";
        }
    }
    check(unimplemented.empty(),
          "R9 核对：表里每条 protocol 都有实现（未实现的条目：" +
              (unimplemented.empty() ? std::string("无") : unimplemented) + "）");

    // ---- ③ 分派：按表 protocol 建对象（能力快照取自**本条**）----
    bool dispatch_ok = true;
    for (const aiwrite::ai::ProviderSpec& spec : table.items) {
        if (spec.kind == "web") {
            continue; // 网页版见 ④
        }
        aiwrite::ai::ProviderRuntimeConfig config;
        config.api_key = "sk-selftest-dispatch"; // 只建对象、不发请求
        const aiwrite::ai::ProviderCreation created = aiwrite::ai::createProvider(spec, config);
        dispatch_ok = dispatch_ok && created.provider != nullptr &&
                      created.provider->protocol() == spec.protocol &&
                      !created.provider->name().empty() && created.error.empty();
        if (created.provider != nullptr) {
            // 能力快照必须等于**表里本条**的 capabilities（`PB2-12`：能力来自表）
            const aiwrite::ai::ProviderCaps caps = created.provider->caps();
            dispatch_ok = dispatch_ok && caps.vision == spec.caps.vision &&
                          caps.seed == spec.caps.seed &&
                          caps.system_role == spec.caps.system_role;
        }
    }
    check(dispatch_ok, "分派：official 条目全部建出对象，protocol() / caps() 与表一致");

    // ---- ④ 网页版 / dom / 未知 protocol 的**明确报错**（不静默回落）----
    const aiwrite::ai::ProviderSpec* web_spec = table.find("deepseek-web");
    if (web_spec != nullptr) {
        aiwrite::ai::ProviderRuntimeConfig no_session;
        const aiwrite::ai::ProviderCreation created =
            aiwrite::ai::createProvider(*web_spec, no_session);
        check(created.provider == nullptr && has(created.error, "网页版会话"),
              "分派：deepseek-web 无会话来源 ⇒ 明确报错（说明缺什么）");
        aiwrite::ai::ProviderRuntimeConfig with_session;
        with_session.session_supplier = [] { return aiwrite::web::Session{}; };
        const aiwrite::ai::ProviderCreation ready =
            aiwrite::ai::createProvider(*web_spec, with_session);
        check(ready.provider != nullptr && ready.provider->protocol() == "deepseek-web",
              "分派：deepseek-web 有会话来源 ⇒ 建出内置网页版实现");
    }
    const aiwrite::ai::ProviderSpec* dom_spec = table.find("kimi-web");
    if (dom_spec != nullptr) {
        aiwrite::ai::ProviderRuntimeConfig config;
        const aiwrite::ai::ProviderCreation created =
            aiwrite::ai::createProvider(*dom_spec, config);
        check(created.provider == nullptr && !created.unsupported_protocol &&
                  has(created.error, "DOM"),
              "分派：dom ⇒ 明确说明由 DOM 适配器承载（**不算**未实现协议）");
    }
    aiwrite::ai::ProviderSpec unknown = {};
    unknown.id       = "acme";
    unknown.display  = "Acme";
    unknown.kind     = "official";
    unknown.protocol = "acme-v2";
    const aiwrite::ai::ProviderCreation unknown_created =
        aiwrite::ai::createProvider(unknown, aiwrite::ai::ProviderRuntimeConfig{});
    check(unknown_created.provider == nullptr && unknown_created.unsupported_protocol &&
              has(unknown_created.error, "acme-v2") && has(unknown_created.error, "已实现"),
          "分派：未知 protocol ⇒ 明确报错 + `unsupported_protocol=true` + 列出已实现（R9）");

    // ---- ⑤ 能力门控（`PB2-12`）：纯函数，节点 / 界面 / 自检共用同一判据 ----
    const aiwrite::ai::ProviderCaps no_vision = aiwrite::ai::capsFromSpec(table.find("deepseek-web"));
    aiwrite::ai::GenerateParams   vision_params;
    vision_params.prompt       = "看图";
    vision_params.system_prompt = "你是助手";
    vision_params.seed          = 7;
    const aiwrite::ai::CapabilityGate blocked =
        aiwrite::ai::capability_gate(no_vision, vision_params, true);
    check(blocked.blocking.size() == 1 && has(blocked.blocking[0], "不支持图片输入") &&
              blocked.warnings.size() == 2 && blocked.system_prompt_inlined,
          "门控：vision=false + 图片 ⇒ **拦路**；seed / system_role 同轮给出提示（不拦路）");

    const aiwrite::ai::ProviderCaps permissive = aiwrite::ai::capsFromSpec(nullptr);
    const aiwrite::ai::CapabilityGate open =
        aiwrite::ai::capability_gate(permissive, vision_params, true);
    check(open.blocking.empty() && open.warnings.empty() && !open.system_prompt_inlined &&
              permissive.vision && permissive.seed && permissive.system_role,
          "门控：**无表条目**（spec=nullptr）⇒ 宽松默认，不拦任何能力（老工作流不受影响，I19）");

    const aiwrite::ai::ProviderCaps no_seed_only = [] {
        aiwrite::ai::ProviderCaps caps;
        caps.seed = false;
        return caps;
    }();
    const aiwrite::ai::CapabilityGate seed_gate =
        aiwrite::ai::capability_gate(no_seed_only, vision_params, false);
    check(seed_gate.blocking.empty() && seed_gate.warnings.size() == 1 &&
              has(seed_gate.warnings[0], "已忽略 seed") && !seed_gate.system_prompt_inlined,
          "门控：seed 不支持 ⇒ 只提示「已忽略」（**不拦路**，与既有网页版口径一致）");

    const aiwrite::ai::ProviderCaps no_system_role = [] {
        aiwrite::ai::ProviderCaps caps;
        caps.system_role = false;
        return caps;
    }();
    const aiwrite::ai::CapabilityGate system_gate =
        aiwrite::ai::capability_gate(no_system_role, vision_params, false);
    check(system_gate.system_prompt_inlined && has(system_gate.warnings[0], "前置拼接"),
          "门控：system_role=false ⇒ 指示调用方**前置拼接**系统提示词并提示");

    check(aiwrite::ai::protocolNeedsKey(aiwrite::ai::ProviderOptions{}) &&
              !aiwrite::ai::protocolNeedsKey([] {
                  aiwrite::ai::ProviderOptions options;
                  options.auth_style = "none";
                  return options;
              }()),
          "凭证：bearer 类协议需要 Key；`auth_style=none`（Ollama）不需要");

    std::printf("  V-14 小计：%d 通过 / %d 失败\n", passed, failed);
    return failed == 0;
}
// ---- V-16：网络（代理 / 自签证书）与凭据库配置（M_patchAB §2.3 · `PB-06` 剩余）----
//  * 纯函数：`host_of_url` / `no_proxy_matches` / `resolve_proxy` 优先级 / `describe_http_net` 打码
//  * **回环证明**：把代理指到本机 `httplib::Server` ⇒ 服务器侧回读**绝对 URI**
//    （只有真走了代理才会这样）＋ `NO_PROXY` 命中 ⇒ 代理**一个请求都收不到**
//  * 凭据：`[credentials].dir` / `backend` 真的生效（目录可改；未实现后端**明确报错**）
bool test_network_and_credentials()
{
    std::printf("\n===== V-16 代理 / 证书 / 凭据配置（离线）=====\n");
    int        passed = 0;
    int        failed = 0;
    const auto check  = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };
    const auto has = [](const std::string& text, const std::string& needle) {
        return text.find(needle) != std::string::npos;
    };

    // ---- ① 主机名提取（去 scheme / userinfo / 端口 / 路径；小写）----
    check(aiwrite::ai::hostOfUrl("https://API.DeepSeek.com/v1/chat?x=1") == "api.deepseek.com" &&
              aiwrite::ai::hostOfUrl("http://user:pass@127.0.0.1:8080/x") == "127.0.0.1" &&
              aiwrite::ai::hostOfUrl("http://127.0.0.1:11434/v1/chat/completions") == "127.0.0.1",
          "host_of_url：scheme / userinfo / 端口 / 路径全部剥掉并小写");

    // ---- ② NO_PROXY 判据（`*` / 精确 / 后缀 / host:port / 逗号分隔）----
    const std::string loopback_url = "http://127.0.0.1:8080/v1/messages";
    check(aiwrite::ai::noProxyMatches(loopback_url, "*") &&
              aiwrite::ai::noProxyMatches(loopback_url, "localhost, 127.0.0.1") &&
              !aiwrite::ai::noProxyMatches(loopback_url, "example.com"),
          "NO_PROXY：`*` 与逗号清单（含空格）都命中；无关域名不命中");
    check(aiwrite::ai::noProxyMatches("https://a.corp.com/x", ".corp.com") &&
              aiwrite::ai::noProxyMatches("https://a.corp.com/x", "corp.com") &&
              !aiwrite::ai::noProxyMatches("https://corp.com.evil.com/x", "corp.com"),
          "NO_PROXY：`.corp.com` 匹配子域且**不**被 `corp.com.evil.com` 骗过（后缀按域名边界）");
    check(aiwrite::ai::noProxyMatches("http://127.0.0.1:9/v1", "127.0.0.1:9") &&
              !aiwrite::ai::noProxyMatches("http://127.0.0.1:8080/v1", "127.0.0.1:9"),
          "NO_PROXY：`host:port` 必须**连端口一起**匹配（端口不同 ⇒ 不命中）");

    // ---- ③ 代理优先级（配置 > 环境；无 `NO_PROXY` 命中时才看）----
    const auto fake_env = [](const std::string& https_proxy, const std::string& http_proxy,
                             const std::string& all_proxy, const std::string& no_proxy) {
        return [https_proxy, http_proxy, all_proxy, no_proxy](const char* name) -> std::string {
            const std::string key = (name != nullptr) ? std::string(name) : std::string();
            if (key == "HTTPS_PROXY") {
                return https_proxy;
            }
            if (key == "HTTP_PROXY") {
                return http_proxy;
            }
            if (key == "ALL_PROXY") {
                return all_proxy;
            }
            if (key == "NO_PROXY") {
                return no_proxy;
            }
            return {};
        };
    };
    std::string source;
    const auto  env = fake_env("http://env-https:1", "http://env-http:2", "http://env-all:3", "");
    check(aiwrite::ai::resolve_proxy("https://api.deepseek.com/x", "http://cfg:9", true, "", env,
                                     &source) == "http://cfg:9" &&
              source == "配置",
          "代理优先级：显式配置值**优先于**环境变量（来源标为「配置」）");
    check(aiwrite::ai::resolve_proxy("https://api.deepseek.com/x", "", true, "", env, &source) ==
              "http://env-https:1" &&
              source == "HTTPS_PROXY" &&
              aiwrite::ai::resolve_proxy("http://api.deepseek.com/x", "", true, "", env, &source) ==
                  "http://env-http:2" &&
              source == "HTTP_PROXY",
          "代理优先级：按目标 scheme 取 `HTTPS_PROXY` / `HTTP_PROXY`（https / http 各一条）");
    check(aiwrite::ai::resolve_proxy("https://a.example.com/x", "", true, "",
                                     fake_env("", "", "http://env-all:3", ""), &source) ==
              "http://env-all:3" &&
              source == "ALL_PROXY" &&
              aiwrite::ai::resolve_proxy("https://a.example.com/x", "http://cfg:9", true,
                                         "a.example.com", env, &source)
                      .empty() &&
              source == "NO_PROXY",
          "代理优先级：兜底 `ALL_PROXY`；**`NO_PROXY` 命中 ⇒ 直达**（连配置值也不用）");
    check(aiwrite::ai::resolve_proxy("https://a.example.com/x", "http://cfg:9", true,
                                     "other.example.com",
                                     fake_env("", "", "", "*.example.com"), &source) == "http://cfg:9",
          "代理优先级：`[network].no_proxy` 非空 ⇒ **覆盖**环境变量 `NO_PROXY`");
    check(aiwrite::ai::resolve_proxy("https://a.example.com/x", "", false, "", env, &source).empty() &&
              source == "无",
          "代理：`proxy_from_env=false` 且未手填 ⇒ 不代理（企业里可强制直连）");

    // ---- ④ 证书校验：默认**严格**；只有显式打开自签兼容才关 ----
    aiwrite::Config net_config;
    const aiwrite::ai::HttpNetOptions strict_net =
        aiwrite::ai::resolveHttpNet("https://api.deepseek.com", net_config,
                                      fake_env("", "", "", ""));
    net_config.network.insecure_tls = true;
    const aiwrite::ai::HttpNetOptions loose_net =
        aiwrite::ai::resolveHttpNet("https://api.deepseek.com", net_config,
                                      fake_env("", "", "", ""));
    check(strict_net.verify_tls && !loose_net.verify_tls,
          "证书：默认**严格校验**；`[network].insecure_tls=true` ⇒ 关闭校验（唯一开关）");
    const aiwrite::ai::HttpNetOptions masked_net = aiwrite::ai::resolveHttpNet(
        "https://api.deepseek.com", net_config, fake_env("http://user:secret@proxy:8080", "", "", ""));
    const std::string masked_text = aiwrite::ai::describeHttpNet(masked_net);
    check(has(masked_text, "***") && !has(masked_text, "secret") && has(masked_text, "已关闭"),
          "诊断文案：代理口令**打码**（`user:***@`），且如实说明证书校验已关");

    // ---- ⑤ 回环证明：代理**真的生效**（服务器侧回读绝对 URI）----
    struct ProxySeen {
        std::mutex  mutex;
        int         count = 0;
        std::string target;
    };
    const auto proxy_seen = std::make_shared<ProxySeen>();
    httplib::Server  proxy_server;
    const auto       proxy_body =
        std::string(R"JSON({"choices":[{"message":{"content":"pong"}}]})JSON");
    const auto proxy_handler = [proxy_seen, proxy_body](const httplib::Request& req,
                                                        httplib::Response&       res) {
        std::lock_guard<std::mutex> lock(proxy_seen->mutex);
        ++proxy_seen->count;
        proxy_seen->target = req.target;
        res.set_content(proxy_body, "application/json");
    };
    proxy_server.Post("/v1/messages", proxy_handler);
    proxy_server.Post(R"(.*)", proxy_handler); // 代理形态 = 绝对 URI 目标，走 catch-all
    const int proxy_port = proxy_server.bind_to_any_port("127.0.0.1");
    if (proxy_port <= 0) {
        std::printf("  ✗ 回环代理未起（端口不可用）—— V-16 回环部分跳过\n");
        return failed == 0;
    }
    std::thread proxy_thread([&proxy_server] { proxy_server.listen_after_bind(); });
    proxy_server.wait_until_ready();

    // 目标用**不存在的地址**（`api.example.invalid`）：若真经代理，请求根本不会到那儿
    const std::string target_url = "http://api.example.invalid/v1/messages";
    {
        aiwrite::ai::HttpJsonRequest request;
        request.url = target_url;
        request.body = R"JSON({"prompt":"ping"})JSON";
        // 显式注入网络策略（确定性：不受开发机 `HTTPS_PROXY` / `NO_PROXY` 干扰）
        request.net = aiwrite::ai::HttpNetOptions{
            "http://127.0.0.1:" + std::to_string(proxy_port), "自检", true};
        const aiwrite::ai::HttpJsonResult via_proxy = aiwrite::ai::httpPostJson(request);
        std::string                      seen_target;
        int                              seen_count = 0;
        {
            std::lock_guard<std::mutex> lock(proxy_seen->mutex);
            seen_target = proxy_seen->target;
            seen_count  = proxy_seen->count;
        }
        check(via_proxy.ok && via_proxy.http_status == 200 && seen_count == 1 &&
                  has(seen_target, "http://api.example.invalid") &&
                  has(seen_target, "/v1/messages"),
              "回环证明：配置的代理**真的生效** —— 目标是不存在的域名，请求仍成功；"
              "代理服务器侧回读绝对 URI「" + seen_target + "」");
    }
    {
        // 反面：`NO_PROXY` 命中 ⇒ **直达**（代理收不到请求；因目标不存在 ⇒ 调用失败）
        aiwrite::ai::HttpJsonRequest request;
        request.url  = target_url;
        request.body = R"JSON({"prompt":"ping"})JSON";
        request.net  = aiwrite::ai::HttpNetOptions{"", "NO_PROXY", true};
        const aiwrite::ai::HttpJsonResult direct = aiwrite::ai::httpPostJson(request);
        int seen_count = 0;
        {
            std::lock_guard<std::mutex> lock(proxy_seen->mutex);
            seen_count = proxy_seen->count;
        }
        check(!direct.ok && !direct.cancelled && seen_count == 1,
              "回环反面：网络策略为「直达」时**代理不再收到请求**（直接连目标，失败如实返回）");
    }
    proxy_server.stop();
    proxy_thread.join();

    // ---- ⑥ 凭据库配置（`[credentials].dir` / `backend` **真的生效**）----
    const aiwrite::Config before_credentials = aiwrite::appConfig();
    {
        const std::filesystem::path temp_dir =
            std::filesystem::temp_directory_path() / "aiwrite-v16-credentials";
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);

        aiwrite::Config cred_config    = before_credentials;
        cred_config.credentials.dir    = temp_dir.string();
        cred_config.credentials.backend = "kms"; // 故意写一个**未实现**的后端
        aiwrite::setAppConfig(cred_config);
        check(aiwrite::utils::credentialsDir() == temp_dir.string(),
              "凭据目录：`[credentials].dir` 真的生效（" + aiwrite::utils::credentialsDir() + "）");

        const std::string backend_error = aiwrite::utils::credentialsBackendError();
        std::string       save_error;
        const bool        saved = aiwrite::utils::saveCredential(
            "aiwrite/selftest-v16", "sk-should-never-be-written", &save_error);
        bool any_file = false;
        if (std::filesystem::exists(temp_dir, ec)) {
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(temp_dir, ec)) {
                (void)entry;
                any_file = true;
            }
        }
        check(!backend_error.empty() && has(backend_error, "kms") && !saved &&
                  has(save_error, "尚未实现") && !any_file,
              "凭据后端：未实现的 backend ⇒ **明确报错**、拒绝保存、**零文件留痕**（不静默降级）");
        std::string load_error;
        const std::string loaded =
            aiwrite::utils::loadCredential("aiwrite/selftest-v16", &load_error);
        check(loaded.empty() && has(load_error, "尚未实现"),
              "凭据后端：读取同一后端也给出**同一条**可操作原因（不是「凭据不存在」）");
        std::filesystem::remove_all(temp_dir, ec);
    }
    aiwrite::setAppConfig(before_credentials); // 还原进程配置缓存（不影响其它组）

    std::printf("  V-16 小计：%d 通过 / %d 失败\n", passed, failed);
    return failed == 0;
}

// ---- V-15：两条新协议的**回环端到端**（本机 httplib 服务器；不出网、不碰真实站点）----
//  * 断言「表驱动的端点 / 认证头 / extra_headers / 请求体」**真的上了线**（服务器侧回读）
//  * 同时覆盖：HTTP 错误分类（401 形状）、读超时、取消语义（`PB-02` 口径）
bool test_protocol_loopback()
{
    std::printf("\n===== V-15 Anthropic / Gemini 回环端到端（离线）=====\n");
    int        passed = 0;
    int        failed = 0;
    const auto check  = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };
    const auto has = [](const std::string& text, const std::string& needle) {
        return text.find(needle) != std::string::npos;
    };

    // 服务器侧「收到了什么」（证明请求真的照表构造出来）
    struct Seen {
        std::mutex                         mutex;
        int                                count = 0;
        std::string                        target; // 原始 target（含 query）
        std::string                        body;
        std::map<std::string, std::string> headers;
    };
    const auto anthropic_seen = std::make_shared<Seen>();
    const auto gemini_seen    = std::make_shared<Seen>();
    const auto slow_hits      = std::make_shared<std::atomic<int>>(0);
    const auto anthropic_body =
        std::string(R"JSON({"id":"msg_1","model":"claude-selftest","content":[{"type":"text","text":"pong"}],"stop_reason":"end_turn"})JSON");
    const auto gemini_body =
        std::string(R"JSON({"candidates":[{"content":{"role":"model","parts":[{"text":"pong"}]},"finishReason":"STOP"}]})JSON");
    const auto anthropic_error_body =
        std::string(R"JSON({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})JSON");
    const auto slow_body =
        std::string(R"JSON({"content":[{"type":"text","text":"late"}]})JSON");

    httplib::Server server;
    server.Post("/v1/messages",
                [anthropic_seen, anthropic_body](const httplib::Request& req,
                                                 httplib::Response&       res) {
                    std::lock_guard<std::mutex> lock(anthropic_seen->mutex);
                    ++anthropic_seen->count;
                    anthropic_seen->target                       = req.target;
                    anthropic_seen->body                         = req.body;
                    anthropic_seen->headers["x-api-key"]         = req.get_header_value("x-api-key");
                    anthropic_seen->headers["anthropic-version"] =
                        req.get_header_value("anthropic-version");
                    anthropic_seen->headers["authorization"] = req.get_header_value("Authorization");
                    res.set_content(anthropic_body, "application/json");
                });
    server.Post(R"(/v1beta/models/([^:]+):generateContent)",
                [gemini_seen, gemini_body](const httplib::Request& req, httplib::Response& res) {
                    std::lock_guard<std::mutex> lock(gemini_seen->mutex);
                    ++gemini_seen->count;
                    gemini_seen->target                   = req.target;
                    gemini_seen->body                     = req.body;
                    gemini_seen->headers["authorization"] = req.get_header_value("Authorization");
                    res.set_content(gemini_body, "application/json");
                });
    server.Post("/v1/messages-unauthorized",
                [anthropic_error_body](const httplib::Request&, httplib::Response& res) {
                    res.status = 401;
                    res.set_content(anthropic_error_body, "application/json");
                });
    server.Post("/v1/messages-slow",
                [slow_hits, slow_body](const httplib::Request&, httplib::Response& res) {
                    slow_hits->fetch_add(1);
                    std::this_thread::sleep_for(std::chrono::seconds(4));
                    res.set_content(slow_body, "application/json");
                });

    const int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("  ✗ 回环服务器未起（端口不可用）—— V-15 无法继续\n");
        return false;
    }
    std::thread server_thread([&server] { server.listen_after_bind(); });
    server.wait_until_ready();

    // 表条目 → 本机地址（其余字段**全部照表**：这是「表驱动」的证明）
    const aiwrite::ai::ProviderSpecs table          = aiwrite::ai::loadProviderSpecs();
    const aiwrite::ai::ProviderSpec* anthropic_spec = table.find("anthropic");
    const aiwrite::ai::ProviderSpec* gemini_spec    = table.find("gemini");
    if (anthropic_spec == nullptr || gemini_spec == nullptr) {
        std::printf("  ✗ 表里缺少 anthropic / gemini 条目 —— V-15 无法继续\n");
        server.stop();
        server_thread.join();
        return false;
    }

    // ================= Anthropic =================
    aiwrite::ai::ProviderSpec local_anthropic = *anthropic_spec;
    local_anthropic.api_base                  = "http://127.0.0.1:" + std::to_string(port);

    aiwrite::ai::ProviderRuntimeConfig anthropic_config;
    anthropic_config.api_base = local_anthropic.api_base;
    anthropic_config.model    = "claude-selftest";
    anthropic_config.api_key  = "sk-selftest-anthropic";
    anthropic_config.options  = aiwrite::ai::providerOptionsFrom(local_anthropic);

    const aiwrite::ai::ProviderCreation anthropic_provider =
        aiwrite::ai::createProvider(local_anthropic, anthropic_config);
    check(anthropic_provider.provider != nullptr, "Anthropic：工厂按表建出实现");

    aiwrite::ai::GenerateParams anthropic_params;
    anthropic_params.prompt        = "请只回复 pong";
    anthropic_params.system_prompt = "你是测试助手";
    anthropic_params.max_tokens    = 64;

    if (anthropic_provider.provider != nullptr) {
        const aiwrite::ai::GenerateResult result =
            anthropic_provider.provider->generate(anthropic_params);
        check(result.ok && result.text == "pong" && result.http_status == 200 &&
                  result.protocol == "anthropic",
              "Anthropic 端到端：整链路成功（HTTP " + std::to_string(result.http_status) + " / " +
                  result.provider_name + " / " +
                  std::to_string(static_cast<long long>(result.elapsed_ms)) + " ms / 文本 " +
                  std::to_string(result.text.size()) + " 字节" +
                  (result.error.empty() ? "" : (" / 错误：" + result.error)) + "）");
        std::string api_key_header;
        std::string version_header;
        std::string auth_header;
        std::string request_body;
        int         count = 0;
        {
            std::lock_guard<std::mutex> lock(anthropic_seen->mutex);
            api_key_header = anthropic_seen->headers["x-api-key"];
            version_header = anthropic_seen->headers["anthropic-version"];
            auth_header    = anthropic_seen->headers["authorization"];
            request_body   = anthropic_seen->body;
            count          = anthropic_seen->count;
        }
        check(count == 1 && api_key_header == "sk-selftest-anthropic" &&
                  version_header == "2023-06-01" && auth_header.empty(),
              "Anthropic 上线校验：`x-api-key` + 表里的 `anthropic-version` 都到了，"
              "且**未**发 `Authorization`（口径正确）");
        check(has(request_body, "\"system\"") && has(request_body, "\"max_tokens\":64") &&
                  has(request_body, "请只回复 pong"),
              "Anthropic 上线校验：正文里 system 是顶层字段 + max_tokens 生效");

        // ---- HTTP 错误（401 + Anthropic 错误形状）----
        aiwrite::ai::ProviderSpec bad_spec            = local_anthropic;
        bad_spec.chat_path                            = "/v1/messages-unauthorized";
        aiwrite::ai::ProviderRuntimeConfig bad_config = anthropic_config;
        bad_config.options                            = aiwrite::ai::providerOptionsFrom(bad_spec);
        const aiwrite::ai::ProviderCreation bad_provider =
            aiwrite::ai::createProvider(bad_spec, bad_config);
        const aiwrite::ai::GenerateResult bad_result =
            bad_provider.provider->generate(anthropic_params);
        check(!bad_result.ok && bad_result.http_status == 401 &&
                  has(bad_result.error, "invalid x-api-key"),
              "Anthropic 端到端：401 ⇒ 失败但**原因可操作**（带出站点原话）");

        // ---- 读超时（表驱动超时被本次调用覆盖为 1 s）----
        aiwrite::ai::ProviderSpec slow_spec = local_anthropic;
        slow_spec.chat_path                 = "/v1/messages-slow";
        aiwrite::ai::ProviderRuntimeConfig slow_config = anthropic_config;
        slow_config.options = aiwrite::ai::providerOptionsFrom(slow_spec);
        const aiwrite::ai::ProviderCreation slow_provider =
            aiwrite::ai::createProvider(slow_spec, slow_config);
        aiwrite::ai::GenerateParams slow_params = anthropic_params;
        slow_params.read_timeout_s              = 1;
        const auto slow_started                 = std::chrono::steady_clock::now();
        const aiwrite::ai::GenerateResult slow_result =
            slow_provider.provider->generate(slow_params);
        const double slow_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - slow_started)
                .count();
        check(!slow_result.ok && !slow_result.cancelled && slow_ms < 3000.0,
              "Anthropic 端到端：1 秒读超时对 4 秒响应 ⇒ 按时失败（" +
                  std::to_string(static_cast<long long>(slow_ms)) + " ms / 错误：" +
                  slow_result.error + "）");

        // ---- 取消（`PB-02`：取消 ≠ 站点错误，1.5 s 内返回）----
        aiwrite::ai::CancelTokenPtr cancel = std::make_shared<aiwrite::ai::CancelToken>();
        std::thread                 canceller([cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            cancel->request_cancel();
        });
        aiwrite::ai::GenerateParams cancel_params = anthropic_params;
        cancel_params.cancel                      = cancel;
        const auto cancel_started                 = std::chrono::steady_clock::now();
        const aiwrite::ai::GenerateResult cancel_result =
            slow_provider.provider->generate(cancel_params);
        const double cancel_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                     cancel_started)
                .count();
        canceller.join();
        check(cancel_result.cancelled && !cancel_result.ok && cancel_ms < 1500.0,
              "Anthropic 端到端：请求中取消 ⇒ 1.5 s 内返回且 `cancelled=true`（" +
                  std::to_string(static_cast<long long>(cancel_ms)) + " ms）");
    }

    // ================= Gemini =================
    aiwrite::ai::ProviderSpec local_gemini = *gemini_spec;
    local_gemini.api_base                  = "http://127.0.0.1:" + std::to_string(port);

    aiwrite::ai::ProviderRuntimeConfig gemini_config;
    gemini_config.api_base = local_gemini.api_base;
    gemini_config.model    = "gemini-selftest";
    gemini_config.api_key  = "sk-selftest-gemini";
    gemini_config.options  = aiwrite::ai::providerOptionsFrom(local_gemini);

    const aiwrite::ai::ProviderCreation gemini_provider =
        aiwrite::ai::createProvider(local_gemini, gemini_config);
    check(gemini_provider.provider != nullptr, "Gemini：工厂按表建出实现");

    aiwrite::ai::GenerateParams gemini_params;
    gemini_params.prompt        = "请只回复 pong";
    gemini_params.system_prompt = "你是测试助手";
    gemini_params.max_tokens    = 64;
    gemini_params.seed          = 7;

    if (gemini_provider.provider != nullptr) {
        const aiwrite::ai::GenerateResult result = gemini_provider.provider->generate(gemini_params);
        check(result.ok && result.text == "pong" && result.http_status == 200 &&
                  result.protocol == "gemini",
              "Gemini 端到端：整链路成功（HTTP " + std::to_string(result.http_status) + " / " +
                  result.provider_name + " / " +
                  std::to_string(static_cast<long long>(result.elapsed_ms)) + " ms / 文本 " +
                  std::to_string(result.text.size()) + " 字节" +
                  (result.error.empty() ? "" : (" / 错误：" + result.error)) + "）");
        std::string target;
        std::string request_body;
        std::string auth_header;
        {
            std::lock_guard<std::mutex> lock(gemini_seen->mutex);
            target       = gemini_seen->target;
            request_body = gemini_seen->body;
            auth_header  = gemini_seen->headers["authorization"];
        }
        check(has(target, "/v1beta/models/gemini-selftest:generateContent") &&
                  has(target, "key=sk-selftest-gemini") && auth_header.empty(),
              "Gemini 上线校验：路径含 `{model}` 替换结果 + `?key=` 查询参数，"
              "且**未**发凭证头（auth_style=query）");
        check(has(request_body, "\"systemInstruction\"") &&
                  has(request_body, "\"maxOutputTokens\":64") &&
                  has(request_body, "\"seed\":7"),
              "Gemini 上线校验：systemInstruction / generationConfig（含 seed）都在正文里");
    }

    std::printf("  V-15 小计：%d 通过 / %d 失败\n", passed, failed);
    server.stop();
    server_thread.join();
    return failed == 0;
}


void printUsage()
{
    std::printf("api_probe - AIwrite M1 技术验证工具\n");
    std::printf("用法:\n");
    std::printf("  api_probe --selftest\n");
    std::printf("  api_probe --graph-selftest          # 图模型/注册表/撤销栈/序列化 自检（不需要网络）\n");
    std::printf("  api_probe --exec-selftest           # 拓扑排序 + 加载/运行前校验 + 执行器 自检（不需要网络）\n");
    std::printf("  api_probe --image-decode <图片路径>  # 图片诊断：内容嗅探 / MIME / 解码（stb|wic）/ 尺寸 / WIC 能力\n");
    std::printf("  api_probe --sha3 <text>\n");
    std::printf("  api_probe --http <url>\n");
    std::printf("  api_probe --chat \"<prompt>\" [--model <name>] [--api-base <url>] [--insecure]\n");
    std::printf("     --dry-run   只打印请求（URL / Authorization 脱敏 / body）并做结构断言，不发起网络请求\n");
    std::printf("  自检 --selftest 覆盖: V-05 SHA3 / V-04 HTTP / V-06 请求构造 / V-08 配置往返 / V-09 会话与可见性\n");
    std::printf("                        + V-11 无 Key 请求构造 / V-12 取消与超时（回环）/ V-13 L2 协议请求构造\n");
    std::printf("                        + V-14 工厂分派与能力门控 / V-15 Anthropic·Gemini 回环端到端\n");
    std::printf("                        + V-16 代理 / 自签证书 / 凭据库配置（回环证明）\n");
    std::printf("  api_probe --diag-dump               # 打印诊断报告（版本/系统/依赖/路径/配置/站点表；不含密钥）\n");
    std::printf("  api_probe --diag-assert             # 诊断报告离线断言（单行 PASS/FAIL；exit 0/1）\n");
    std::printf("  api_probe --dep-check               # 依赖自检：WebView2 运行时（缺失时打印安装指引；exit 0/1）\n");
    std::printf("  api_probe --settings-assert         # 设置面板校验纯函数断言（单行 PASS/FAIL；exit 0/1）\n");
    std::printf("  api_probe --site-assert             # 自建网页版站点闭环断言（会临时写入再删除\n"
                "                                      ~/.brain-ai/providers.d/aiwrite-selftest-site.json；exit 0/1）\n");
    std::printf("  api_probe --official-assert         # 自建官方 API 提供商闭环断言（自定义 AI 收口；\n"
                "                                      会临时写入再删除 ~/.brain-ai/providers.d/\n"
                "                                      aiwrite-selftest-official.json；exit 0/1）\n");
    std::printf("  api_probe --autosave-assert         # 自动保存 / 崩溃恢复断言（PC-01/PC-02；临时目录内构造\n"
                "                                      场景，不碰用户数据与快照；exit 0/1）\n");

}

bool test_sha3()
{
    struct Vector {
        const char* text;
        const char* expected;
    };
    const Vector vectors[] = {
        {"abc", kSha3Abc},
        {"", kSha3Empty},
    };

    bool all_ok = true;
    std::printf("[V-05] SHA3-256 (OpenSSL EVP)\n");
    for (const Vector& vector : vectors) {
        const std::string actual = aiwrite::crypto::sha3_256(vector.text);
        const bool ok            = (actual == vector.expected);
        all_ok                   = all_ok && ok;
        std::printf("   %-6s SHA3-256(\"%s\") = %s\n", ok ? "PASS" : "FAIL", vector.text,
                    actual.c_str());
        if (!ok) {
            std::printf("          期望: %s\n", vector.expected);
        }
    }
    return all_ok;
}

// V-06 请求构造自检（无需 API Key）：打印将发送的请求并做结构断言
bool test_chat_request_shape(const Options& options)
{
    std::printf("[V-06] 请求构造自检（不发起网络请求）\n");

    nlohmann::json body;
    body["model"]    = options.model;
    body["stream"]   = false;
    body["messages"] = nlohmann::json::array(
        {nlohmann::json{{"role", "user"}, {"content", options.chat_prompt}}});

    const char* key        = std::getenv("DEEPSEEK_API_KEY");
    const std::string auth = (key != nullptr && *key != '\0')
                                 ? "Bearer ****（已设置 DEEPSEEK_API_KEY，值已脱敏）"
                                 : "(未设置 DEEPSEEK_API_KEY，仅校验请求构造)";

    std::printf("       POST %s/chat/completions\n", options.api_base.c_str());
    std::printf("       Authorization: %s\n", auth.c_str());
    std::printf("       Content-Type: application/json\n");
    std::printf("       body: %s\n", body.dump(2).c_str());

    const bool ok =
        body.contains("model") && body["model"].is_string() &&
        !body["model"].get<std::string>().empty() && body.contains("stream") &&
        body["stream"].is_boolean() && !body["stream"].get<bool>() && body.contains("messages") &&
        body["messages"].is_array() && body["messages"].size() == 1 &&
        body["messages"][0].value("role", std::string()) == "user" &&
        body["messages"][0].value("content", std::string()) == options.chat_prompt;

    std::printf("   %-6s 请求体结构: model 非空 / stream=false / messages[1]{role=user, content=prompt}\n",
                ok ? "PASS" : "FAIL");
    return ok;
}

// M1-07 配置读写往返自检（在临时文件上做，不改动用户配置）
bool test_config_roundtrip()
{
    std::printf("[V-08] 配置加载 / 保存往返自检（临时文件）\n");

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "aiwrite_config_roundtrip.toml";
    std::error_code ec;
    std::filesystem::remove(file, ec);

    aiwrite::Config created;
    if (!aiwrite::loadConfig(file, created)) { // 文件不存在 → 生成默认配置并落盘
        std::printf("   FAIL  默认配置生成失败\n");
        return false;
    }
    const bool generated = std::filesystem::exists(file);

    aiwrite::Config modified         = created;
    modified.ui.show_grid            = !created.ui.show_grid;
    modified.output.ttl_days         = 7;
    modified.deepseek.model          = "deepseek-reasoner";
    // F3 / PD-04：窗口几何字段也要能往返
    modified.ui.window_width     = 1440;
    modified.ui.window_height    = 900;
    modified.ui.window_pos_x     = 120;
    modified.ui.window_pos_y     = 80;
    modified.ui.window_maximized = true;
    if (!aiwrite::saveConfig(file, modified)) {
        std::printf("   FAIL  保存配置失败\n");
        return false;
    }

    aiwrite::Config reloaded;
    if (!aiwrite::loadConfig(file, reloaded)) {
        std::printf("   FAIL  重新加载配置失败\n");
        return false;
    }

    const bool geometry_same = reloaded.ui.window_width == modified.ui.window_width &&
                               reloaded.ui.window_height == modified.ui.window_height &&
                               reloaded.ui.window_pos_x == modified.ui.window_pos_x &&
                               reloaded.ui.window_pos_y == modified.ui.window_pos_y &&
                               reloaded.ui.window_maximized == modified.ui.window_maximized;
    const bool same = generated && reloaded.config_version == modified.config_version &&
                      reloaded.ui.show_grid == modified.ui.show_grid &&
                      reloaded.output.ttl_days == modified.output.ttl_days &&
                      reloaded.deepseek.model == modified.deepseek.model && geometry_same;
    std::printf("   %-6s 默认生成=%s；字段往返一致=%s（show_grid / ttl_days / model / 窗口几何；"
                "`PQ-18②` 起 general.language 已移除、不再往返）\n",
                same ? "PASS" : "FAIL", generated ? "是" : "否", same ? "是" : "否");

    // PA-07：进程级配置访问器（app_config / set_app_config）
    const aiwrite::Config before = aiwrite::appConfig();
    aiwrite::setAppConfig(modified);
    const bool cache_ok = aiwrite::appConfig().deepseek.model == modified.deepseek.model &&
                          aiwrite::appConfig().output.ttl_days == modified.output.ttl_days &&
                          aiwrite::appConfig().ui.show_grid == modified.ui.show_grid;
    aiwrite::setAppConfig(before); // 还原，避免污染后续用例
    std::printf("   %-6s PA-07 配置访问器：set→get 一致=%s；默认连接超时=%d ms（未设置时返回默认值）\n",
                cache_ok ? "PASS" : "FAIL", cache_ok ? "是" : "否",
                aiwrite::Config{}.timeout.connect_ms);

    std::filesystem::remove(file, ec);

    // ---- P7a-12/13（UI A 档）：默认布局常显 + 老配置显式 false 尊重原值 ----
    {
        const std::filesystem::path defaults_file =
            std::filesystem::temp_directory_path() / "aiwrite_config_defaults.toml";
        std::error_code ec3;
        std::filesystem::remove(defaults_file, ec3);

        // ① 键**不存在** → 用新默认（节点库 / 参数面板**显示**，P7a-12）
        aiwrite::Config fresh;
        (void)aiwrite::loadConfig(defaults_file, fresh);
        const bool default_visible = fresh.ui.show_node_library && fresh.ui.show_property_panel;
        std::printf("   %-6s P7a-12 默认布局：节点库=%s / 参数面板=%s（新默认应为 true）\n",
                    default_visible ? "PASS" : "FAIL",
                    fresh.ui.show_node_library ? "true" : "false",
                    fresh.ui.show_property_panel ? "true" : "false");

        // ② 显式 false → **尊重原值**（P7a-13：按「键是否存在」判定）
        {
            std::ofstream out(defaults_file, std::ios::binary | std::ios::trunc);
            out << "config_version = 1\n\n[ui]\nshow_node_library = false\n"
                   "show_property_panel = false\n";
        }
        aiwrite::Config explicit_off;
        (void)aiwrite::loadConfig(defaults_file, explicit_off);
        const bool respected =
            !explicit_off.ui.show_node_library && !explicit_off.ui.show_property_panel;
        std::printf("   %-6s P7a-13 老配置显式 false：被尊重（未被新默认覆盖）=%s\n",
                    respected ? "PASS" : "FAIL", respected ? "是" : "否");

        // ③ 只写其中一个键 → 另一个走新默认（字段级独立）
        {
            std::ofstream out(defaults_file, std::ios::binary | std::ios::trunc);
            out << "config_version = 1\n\n[ui]\nshow_node_library = false\n";
        }
        aiwrite::Config partial;
        (void)aiwrite::loadConfig(defaults_file, partial);
        const bool field_level = !partial.ui.show_node_library && partial.ui.show_property_panel;
        std::printf("   %-6s P7a-13 字段级独立：显式 false 保持 / 缺失键走新默认=%s\n",
                    field_level ? "PASS" : "FAIL", field_level ? "是" : "否");

        std::filesystem::remove(defaults_file, ec3);
    }

    // ---- M_patchB L1（PB2-06）：多 provider 实例参数 + 旧单节迁移 + 备份 ----
    bool multi_ok = false;
    {
        const std::filesystem::path multi_file =
            std::filesystem::temp_directory_path() / "aiwrite_config_multi.toml";
        std::error_code ec2;
        std::filesystem::remove(multi_file, ec2);
        std::filesystem::remove(multi_file.string() + ".bak", ec2);

        // ① 旧格式（只有 [providers.deepseek]）→ 加载应自动迁移进映射（幂等）
        {
            std::ofstream out(multi_file, std::ios::binary | std::ios::trunc);
            out << "config_version = 1\n\n[providers.deepseek]\nprovider = \"deepseek\"\n"
                   "mode = \"web\"\napi_base = \"https://api.deepseek.com\"\n"
                   "model = \"deepseek-reasoner\"\napi_key_ref = \"brain-ai/deepseek\"\n";
        }
        aiwrite::Config legacy;
        const bool      loaded   = aiwrite::loadConfig(multi_file, legacy);
        const bool      migrated = loaded && legacy.providers.count("deepseek") == 1 &&
                              legacy.providers["deepseek"].mode == "web" &&
                              legacy.providers["deepseek"].model == "deepseek-reasoner" &&
                              legacy.deepseek.model == "deepseek-reasoner";

        // ② 增加第二个 provider → 保存 → 重新加载：两条都在；`.bak` 已生成
        legacy.providers["zhipu"].provider    = "zhipu";
        legacy.providers["zhipu"].api_base    = "https://open.bigmodel.cn/api/paas/v4";
        legacy.providers["zhipu"].model       = "glm-4-flash";
        legacy.providers["zhipu"].api_key_ref = "brain-ai/zhipu";
        legacy.deepseek.model                 = "deepseek-chat"; // 旧成员 = deepseek 条的权威
        const bool saved     = aiwrite::saveConfig(multi_file, legacy);
        const bool backed_up = std::filesystem::exists(multi_file.string() + ".bak", ec2);

        aiwrite::Config again;
        const bool      reloaded_ok = aiwrite::loadConfig(multi_file, again);
        const bool      multi       = reloaded_ok && again.providers.size() == 2 &&
                          again.providers["zhipu"].model == "glm-4-flash" &&
                          again.providers["zhipu"].api_base == "https://open.bigmodel.cn/api/paas/v4" &&
                          again.providers["deepseek"].model == "deepseek-chat" &&
                          again.deepseek.model == "deepseek-chat";
        multi_ok = migrated && saved && backed_up && multi;
        std::printf("   %-6s PB2-06 多 provider 实例参数：旧单节迁移=%s；新增条目往返=%s；"
                    "保存前备份 .bak=%s\n",
                    multi_ok ? "PASS" : "FAIL", migrated ? "OK" : "异常", multi ? "OK" : "异常",
                    backed_up ? "已生成" : "缺失");

        std::filesystem::remove(multi_file, ec2);
        std::filesystem::remove(multi_file.string() + ".bak", ec2);
    }

    // ---- F3 / PD-04：越屏矫正纯函数（utils::fit_window_to_workarea）----
    using aiwrite::utils::WindowRect;
    const WindowRect workarea{0, 0, 1920, 1040};             // 工作区（已扣除任务栏）
    const WindowRect inside{100, 80, 1280, 720};             // 完全在工作区内
    const WindowRect offscreen_right{3000, 200, 1280, 720};  // 完全在右侧屏外
    const WindowRect offscreen_topleft{-200, -100, 1280, 720};
    const WindowRect too_large{0, 0, 3000, 2000};            // 尺寸超出工作区
    const WindowRect too_small{10, 10, 100, 80};             // 尺寸小于最小值保护

    const aiwrite::utils::FitResult keep    = aiwrite::utils::fit_window_to_workarea(inside, workarea);
    const aiwrite::utils::FitResult pulled  =
        aiwrite::utils::fit_window_to_workarea(offscreen_right, workarea);
    const aiwrite::utils::FitResult clamped =
        aiwrite::utils::fit_window_to_workarea(offscreen_topleft, workarea);
    const aiwrite::utils::FitResult shrunk  = aiwrite::utils::fit_window_to_workarea(too_large, workarea);
    const aiwrite::utils::FitResult grown   = aiwrite::utils::fit_window_to_workarea(too_small, workarea);
    const aiwrite::utils::FitResult no_area =
        aiwrite::utils::fit_window_to_workarea(inside, WindowRect{});

    const bool fit_ok = !keep.changed && keep.rect.x == 100 && keep.rect.width == 1280 &&
                        pulled.changed && pulled.rect.x == 640 && pulled.rect.y == 200 &&
                        clamped.changed && clamped.rect.x == 0 && clamped.rect.y == 0 &&
                        shrunk.changed && shrunk.rect.width == 1920 && shrunk.rect.height == 1040 &&
                        grown.changed && grown.rect.width == 640 && grown.rect.height == 480 &&
                        !no_area.changed && no_area.rect.width == 1280 &&
                        aiwrite::utils::hasPosition(inside) &&
                        !aiwrite::utils::hasPosition(WindowRect{-1, -1, 0, 0});
    std::printf("   %-6s 窗口几何越屏矫正：区内不变=%s；越屏拉回=%s（x=%d）；超大收缩=%s（%dx%d）；"
                "最小保护=%s（%dx%d）；无工作区不变=%s\n",
                fit_ok ? "PASS" : "FAIL", !keep.changed ? "是" : "否",
                pulled.rect.x == 640 ? "是" : "否", pulled.rect.x,
                shrunk.rect.width == 1920 ? "是" : "否", shrunk.rect.width, shrunk.rect.height,
                grown.rect.width == 640 ? "是" : "否", grown.rect.width, grown.rect.height,
                !no_area.changed ? "是" : "否");

    // ---- PA-08：未接配置字段清单（"改了 config 却没效果"必须可查）----
    const std::vector<std::string> unwired = aiwrite::unwiredConfigFields();
    const auto                     has_unwired = [&unwired](const char* needle) {
        for (const std::string& field : unwired) {
            if (field.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    // 说明（`PQ-18②` · 2026-10-09）：general.* 已从配置表移除 ⇒ 断言改为「**不含** general.*」
    //  * 本行是**单行 PASS/FAIL**（非逐项计数）⇒ 不影响 `--selftest` / `--exec-selftest` 的断言数字
    // 口径变更（2026-10-10 · M6）：`output.auto_open_on_complete` / `advanced.log_ttl_days`
    //  **已接线** ⇒ 断言从「在清单里」改为「**不在**清单里」（接线点见 config.cpp 注释与 CHANGELOG）
    // 口径变更（2026-10-11 · M_patchAB 批 4）：`timeout.*` / `error.*` **已接线**（官方 API
    //  默认超时 + 可重试失败重试）⇒ 断言改为「**不在**清单里」（接线点见 config.cpp 注释）
    const bool unwired_ok = !has_unwired("general.") && !has_unwired("timeout.") &&
                            !has_unwired("error.") && has_unwired("ui.grid_size") &&
                            !has_unwired("console_height") && !has_unwired("running_animation") &&
                            !has_unwired("window_width") &&
                            !has_unwired("output.auto_open_on_complete") &&
                            !has_unwired("advanced.log_ttl_days");
    std::printf("   %-6s PA-08 未接配置字段清单：%zu 项；不含 general.*=%s；"
                "不含 console_height/running_animation/window_*=%s\n",
                unwired_ok ? "PASS" : "FAIL", unwired.size(),
                !has_unwired("general.") ? "是" : "否",
                (!has_unwired("console_height") && !has_unwired("running_animation") &&
                 !has_unwired("window_width"))
                    ? "是"
                    : "否");
    if (!unwired_ok) { // §3：断言失败必须打印逐例明细
        for (const std::string& field : unwired) {
            std::printf("         未接字段: %s\n", field.c_str());
        }
        std::printf("         auto_open_on_complete 在清单=%s / log_ttl_days 在清单=%s\n",
                    has_unwired("output.auto_open_on_complete") ? "是" : "否",
                    has_unwired("advanced.log_ttl_days") ? "是" : "否");
    }

    return same && cache_ok && fit_ok && unwired_ok && multi_ok;
}

// M2：网页版会话存储 + 参数条件可见性自检（不需要网络）
bool test_web_session_and_visibility()
{
    std::printf("[V-09] 会话存储 / 参数条件可见性自检\n");

    // ---- 会话存储：写入 → 快照 → 查找 → 注销 ----
    aiwrite::web::Session session;
    session.url = "https://chat.deepseek.com/";
    aiwrite::web::Cookie cookie;
    cookie.name  = "ds_session_id";
    cookie.value = "0123456789abcdef0123456789abcdef";
    session.cookies.push_back(cookie);
    aiwrite::web::SessionStore::instance().set(session);

    const aiwrite::web::Session snapshot = aiwrite::web::SessionStore::instance().snapshot();
    const bool store_ok = snapshot.loggedIn && snapshot.cookieCount() == 1 &&
                          snapshot.has("ds_session_id") &&
                          aiwrite::web::maskValue(cookie.value) == "0123****cdef(len=32)";
    aiwrite::web::SessionStore::instance().clear();
    const bool cleared_ok = !aiwrite::web::SessionStore::instance().loggedIn() &&
                            aiwrite::web::SessionStore::instance().cookieCount() == 0;

    // ---- 会话自动引导决策（2026-09-26 真 bug 的回归断言）----
    //  旧逻辑：只要“窗口已打开”就只等 → 用户手动开的窗口从不探测 → 25s 超时 →「未取得网页版凭证」
    using aiwrite::web::SessionBoot;
    const bool boot_ok =
        aiwrite::web::plan_session_boot(true, false) == SessionBoot::HaveToken &&
        aiwrite::web::plan_session_boot(true, true) == SessionBoot::HaveToken &&
        aiwrite::web::plan_session_boot(false, true) == SessionBoot::ReuseAndProbe &&
        aiwrite::web::plan_session_boot(false, false) == SessionBoot::StartAndProbe;
    // 手动「打开登录窗口」必须在页面加载后自动探测一次（否则运行时只能等到超时）
    // M_patchB L1 续（PB2-17）：无参重载 = 旧常量（逐字一致，钉住不变量 I2）；站点参数的按条目的
    // 断言在 --exec-selftest 的 VB2-17 块（改表即换站点）
    const aiwrite::web::LoginRequest interactive = aiwrite::web::interactive_login_request();
    const bool request_ok = interactive.probe_after_load && !interactive.offscreen &&
                            interactive.url == "https://chat.deepseek.com/" &&
                            interactive.window_title ==
                                "AIwrite · DeepSeek 网页版登录（登录后关闭本窗口）";

    // ---- 参数条件可见性：ProviderConfig 在 official 显示 api_key，在 web 隐藏（且不参与校验）----
    aiwrite::engine::registerAllNodes();
    aiwrite::engine::Graph graph;
    std::string            error;
    const std::string      id       = graph.addNode("ProviderConfig", 0.0f, 0.0f, &error);
    aiwrite::engine::Node* provider = graph.findNode(id);

    bool visibility_ok = false;
    if (provider != nullptr) {
        aiwrite::engine::Param* mode = provider->findParam("mode");
        std::vector<std::string> errors;
        const bool official_visible = aiwrite::engine::paramVisible(*provider, "api_key");
        const bool official_valid   = graph.validateParams(*provider, &errors);
        if (mode != nullptr) {
            mode->value = "web";
        }
        errors.clear();
        const bool web_hidden = !aiwrite::engine::paramVisible(*provider, "api_key") &&
                                !aiwrite::engine::paramVisible(*provider, "api_base");
        const bool web_valid = graph.validateParams(*provider, &errors);
        visibility_ok =
            (mode != nullptr) && official_visible && web_hidden && official_valid && web_valid;
    }

    const bool ok = store_ok && cleared_ok && visibility_ok && boot_ok && request_ok;
    std::printf("   %-6s 会话存储写读=%s%s；参数可见性=%s（official 显示 api_key / web 隐藏 "
                "api_key·api_base / 两模式校验均通过）\n",
                ok ? "PASS" : "FAIL", store_ok ? "OK" : "异常", cleared_ok ? "/注销清空 OK" : "/注销异常",
                visibility_ok ? "OK" : "异常");
    std::printf("         会话自动引导决策=%s（含「窗口已开→补探测 ReuseAndProbe」真值表 4 项）；"
                "手动登录窗口自动探测=%s\n",
                boot_ok ? "OK" : "异常", request_ok ? "OK" : "异常");
    return ok;
}

// 设置节点参数（节点/参数不存在时静默跳过，断言由调用方负责）
void set_param(engine::Graph& graph, const std::string& node_id, const std::string& param_id,
               const nlohmann::json& value)
{
    if (engine::Node* node = graph.findNode(node_id)) {
        if (engine::Param* param = node->findParam(param_id)) {
            param->value = value;
        }
    }
}

// M2-05：工作流文件读写 + 最近列表（不需要网络）
bool test_workflow_files()
{
    std::printf("[V-10] 工作流文件 / 最近列表自检\n");

    const std::filesystem::path work_file =
        std::filesystem::temp_directory_path() / "aiwrite_workflow_selftest.json";
    const std::filesystem::path recentFile = aiwrite::paths::recentFile();
    std::error_code             ec;
    std::filesystem::remove(work_file, ec);

    // 备份用户现有 recent.json（测试结束原样还原，避免污染最近列表）
    std::string recent_backup;
    const bool  had_recent = std::filesystem::exists(recentFile, ec);
    if (had_recent) {
        std::ifstream     in(recentFile, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        recent_backup = buffer.str();
    }

    bool ok = true;

    // ---------------------------------------------------- 1. 保存 → 加载 -----
    {
        aiwrite::engine::registerAllNodes();
        aiwrite::engine::Graph graph;
        std::string            error;
        const std::string      text_node = graph.addNode("TextInput", 0.0f, 0.0f, &error);
        const std::string      conf_node = graph.addNode("ProviderConfig", 0.0f, 0.0f, &error);
        set_param(graph, text_node, "text", "自检文本");
        set_param(graph, conf_node, "api_key", "sk-secret-file-test");
        graph.name = "自检工作流";

        const bool saved = aiwrite::engine::saveWorkflow(graph, work_file, &error);

        aiwrite::engine::Graph loaded;
        std::string            load_error;
        const bool             loaded_ok = aiwrite::engine::loadWorkflow(work_file, loaded, &load_error);
        const bool             same = loaded_ok && loaded.nodes.size() == graph.nodes.size() &&
                          loaded.edges.size() == graph.edges.size() &&
                          loaded.name == "自检工作流";
        aiwrite::engine::ValidationMessages issues;
        const bool valid = loaded_ok && aiwrite::engine::validateWorkflow(loaded, &issues);

        std::ifstream     in(work_file, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string text      = buffer.str();
        const bool        no_secret = text.find("sk-secret-file-test") == std::string::npos;

        const bool pass = saved && same && valid && no_secret;
        std::printf("   %-6s 保存=%s / 往返一致=%s / 加载校验=%s / 密钥未落盘=%s\n",
                    pass ? "PASS" : "FAIL", saved ? "OK" : "失败", same ? "OK" : "不一致",
                    valid ? "OK" : "失败", no_secret ? "OK" : "文件里出现明文 Key");
        ok = ok && pass;
    }

    // -------------------------------------------- 2. 最近列表（recent.json）--
    {
        aiwrite::engine::clearRecentFiles();
        const bool empty_ok = aiwrite::engine::loadRecentFiles().empty();

        aiwrite::engine::pushRecentFile("C:/tmp/a.json", "工作流 A");
        aiwrite::engine::pushRecentFile("C:/tmp/b.json", "工作流 B");
        aiwrite::engine::pushRecentFile("C:/tmp/a.json", "工作流 A"); // 重复 → 置顶不重复
        std::vector<aiwrite::engine::RecentEntry> entries = aiwrite::engine::loadRecentFiles();
        const bool order_ok = entries.size() == 2 && entries[0].name == "工作流 A" &&
                              entries[1].name == "工作流 B";

        for (int i = 0; i < 12; ++i) {
            aiwrite::engine::pushRecentFile("C:/tmp/w" + std::to_string(i) + ".json",
                                              "W" + std::to_string(i));
        }
        entries              = aiwrite::engine::loadRecentFiles();
        const bool cap_ok = entries.size() == aiwrite::engine::kMaxRecentFiles &&
                            entries.front().name == "W11";

        aiwrite::engine::clearRecentFiles();
        const bool cleared = aiwrite::engine::loadRecentFiles().empty();

        // M_patchAB §4（PC-03 / FEA-M2-02）：存在性校验 + 失效条目清理
        //  * 造一个**真实存在**的临时工作流 + 一个**不存在**的路径
        const std::filesystem::path real_recent_file =
            std::filesystem::temp_directory_path() / "aiwrite_recent_real.json";
        {
            std::ofstream out(real_recent_file, std::ios::binary | std::ios::trunc);
            out << "{\"version\":\"1.0\",\"nodes\":[]}\n";
        }
        aiwrite::engine::pushRecentFile(real_recent_file.string(), "真实工作流");
        aiwrite::engine::pushRecentFile("Z:/not-exists/aiwrite-recent-missing.json", "失效工作流");
        const std::vector<aiwrite::engine::RecentStatus> statuses =
            aiwrite::engine::recentWithStatus();
        bool real_exists    = false;
        bool missing_exists = true;
        for (const aiwrite::engine::RecentStatus& status : statuses) {
            if (status.entry.path == real_recent_file.string()) {
                real_exists = status.exists;
            }
            else {
                missing_exists = status.exists;
            }
        }
        const bool status_ok = statuses.size() == 2 && real_exists && !missing_exists &&
                               aiwrite::engine::countMissingRecent() == 1;
        // 清理前先复核：**只读查询不写盘**（列表仍 2 条）
        const bool query_readonly = aiwrite::engine::loadRecentFiles().size() == 2;
        const std::size_t removed = aiwrite::engine::pruneMissingRecent();
        const bool prune_ok = removed == 1 &&
                              aiwrite::engine::loadRecentFiles().size() == 1 &&
                              aiwrite::engine::countMissingRecent() == 0;
        std::filesystem::remove(real_recent_file, ec);
        aiwrite::engine::clearRecentFiles();

        const bool pass = empty_ok && order_ok && cap_ok && cleared && status_ok &&
                          query_readonly && prune_ok;
        std::printf("   %-6s 初始空列表=%s / 去重置顶=%s / 上限 10 条=%s / 清空=%s / "
                    "存在性校验=%s / 失效清理=%s\n",
                    pass ? "PASS" : "FAIL", empty_ok ? "OK" : "异常", order_ok ? "OK" : "异常",
                    cap_ok ? "OK" : "异常", cleared ? "OK" : "异常", status_ok ? "OK" : "异常",
                    prune_ok ? "OK" : "异常");
        ok = ok && pass;
    }

    // ------------------------------------- 3. 非法工作流文件被加载校验拒绝 ----
    {
        const std::filesystem::path bad_file =
            std::filesystem::temp_directory_path() / "aiwrite_workflow_bad.json";
        {
            std::ofstream out(bad_file, std::ios::binary | std::ios::trunc);
            out << "{\"version\":\"1.0\",\"name\":\"坏工作流\",\"nodes\":[{\"id\":\"n1\","
                   "\"type\":\"NotExists\",\"title\":\"未知\",\"position\":{\"x\":0,\"y\":0},"
                   "\"params\":{}}],\"edges\":[]}";
        }

        aiwrite::engine::Graph loaded;
        std::string            error;
        const bool             loaded_ok = aiwrite::engine::loadWorkflow(bad_file, loaded, &error);
        aiwrite::engine::ValidationMessages issues;
        const bool valid    = loaded_ok && aiwrite::engine::validateWorkflow(loaded, &issues);
        const bool rejected = !valid;
        std::printf("   %-6s 含未知节点类型的文件被拒绝（加载%s / 校验%s）\n",
                    rejected ? "PASS" : "FAIL", loaded_ok ? "成功" : "失败",
                    valid ? "通过（异常）" : "拒绝");
        ok = ok && rejected;
        std::filesystem::remove(bad_file, ec);
    }

    // -------------------------------------------------- 清理 / 还原现场 -----
    std::filesystem::remove(work_file, ec);
    if (had_recent) {
        std::ofstream out(recentFile, std::ios::binary | std::ios::trunc);
        out << recent_backup;
    }
    else {
        std::filesystem::remove(recentFile, ec);
    }
    return ok;
}

// V-04：HTTP GET
// 返回：0 = 通过（2xx~4xx，链路连通）；1 = 失败；2 = 跳过（网络/代理不可达，非代码问题）
int test_http(const std::string& url, bool insecure)
{
    std::printf("[V-04] HTTP GET %s\n", url.c_str());
    std::printf("       连接超时 15s / 读取超时 30s / 跟随重定向 / 证书校验: %s\n",
                insecure ? "关闭（--insecure）" : "开启");

    httplib::Client client(url);
    client.set_connection_timeout(15, 0);
    client.set_read_timeout(30, 0);
    client.enable_server_certificate_verification(!insecure);
    client.set_follow_location(true);

    const auto start = std::chrono::steady_clock::now();
    auto res         = client.Get("/");
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();

    if (!res) {
        const auto error = res.error();
        // 连接类错误视为"网络/代理不可达" → SKIP（不算代码失败）；TLS 类错误才算 FAIL
        const bool network_unreachable =
            error == httplib::Error::Connection || error == httplib::Error::ConnectionTimeout ||
            error == httplib::Error::Read || error == httplib::Error::Write ||
            error == httplib::Error::ProxyConnection;
        std::printf("   %-6s 请求失败: %s（TLS 校验结果: %ld）\n",
                    network_unreachable ? "SKIP" : "FAIL", httplib::to_string(error).c_str(),
                    insecure ? 0L : client.get_openssl_verify_result());
        if (network_unreachable) {
            std::printf("          判定为网络/代理不可达 → SKIP（不计入失败；如为证书问题可加 --insecure 复测）\n");
            return 2;
        }
        return 1;
    }

    // 未带 Key 访问 api.deepseek.com 会返回 401，同样说明链路连通
    const bool reachable = res->status >= 200 && res->status < 500;
    std::printf("   %-6s HTTP %d，耗时 %lld ms，响应 %zu 字节\n", reachable ? "PASS" : "FAIL",
                res->status, static_cast<long long>(elapsed), res->body.size());
    if (!reachable) {
        std::printf("         响应: %s\n", res->body.substr(0, 200).c_str());
    }
    return reachable ? 0 : 1;
}

// V-06：DeepSeek 官方 API 最小调用（M1-06）
int test_chat(const Options& options)
{
    if (options.dry_run) { // --dry-run：只校验请求构造，不需要 API Key
        return test_chat_request_shape(options) ? 0 : 1;
    }

    std::printf("[V-06] DeepSeek API 调用（model=%s, base=%s）\n", options.model.c_str(),
                options.api_base.c_str());

    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (key == nullptr || *key == '\0') {
        std::printf("   SKIP  未设置环境变量 DEEPSEEK_API_KEY，跳过（其余验证项不受影响）\n");
        return 2;
    }

    httplib::Client client(options.api_base);
    client.set_connection_timeout(15, 0);
    client.set_read_timeout(180, 0);
    client.enable_server_certificate_verification(!options.insecure);

    nlohmann::json body;
    body["model"]    = options.model;
    body["stream"]   = false;
    body["messages"] = nlohmann::json::array(
        {nlohmann::json{{"role", "user"}, {"content", options.chat_prompt}}});

    const httplib::Headers headers = {
        {"Authorization", std::string("Bearer ") + key},
        {"Content-Type", "application/json"},
    };

    const auto start = std::chrono::steady_clock::now();
    auto res         = client.Post("/chat/completions", headers, body.dump(), "application/json");
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();

    if (!res) {
        std::printf("   FAIL  请求失败: %s\n", httplib::to_string(res.error()).c_str());
        return 1;
    }
    if (res->status != 200) {
        std::printf("   FAIL  HTTP %d\n", res->status);
        std::printf("         响应: %s\n", res->body.substr(0, 400).c_str());
        return 1;
    }

    try {
        const nlohmann::json response = nlohmann::json::parse(res->body);
        std::string content;
        if (response.contains("choices") && !response["choices"].empty()) {
            content = response["choices"][0]["message"]["content"].get<std::string>();
        }

        std::printf("   PASS  HTTP 200，耗时 %lld ms\n", static_cast<long long>(elapsed));
        if (response.contains("usage")) {
            const auto& usage = response["usage"];
            std::printf("         tokens: prompt=%d completion=%d total=%d\n",
                        usage.value("prompt_tokens", 0), usage.value("completion_tokens", 0),
                        usage.value("total_tokens", 0));
        }
        std::printf("--------- 生成结果 ---------\n%s\n----------------------------\n",
                    content.c_str());
        return 0;
    }
    catch (const std::exception& ex) {
        std::printf("   FAIL  响应解析失败: %s\n", ex.what());
        std::printf("         响应: %s\n", res->body.substr(0, 400).c_str());
        return 1;
    }
}

