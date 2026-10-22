// =============================================================================
//  api_internal.h —— api_probe 跨 TU 共享声明（CLN-15 拆分）
//
//  拆分：api_probe.cpp（6935 行）按自检分节拆 5 个 TU ——
//    api_probe.cpp（入口 main）/ api_probe_selftest.cpp（协议/网络/CLI 自检）
//    api_graph_selftest.cpp（图模型）/ api_executor_selftest.cpp（执行器 P3-1）
//    api_execution_selftest.cpp（执行核心 P3-2）/ api_diag.cpp（诊断/断言）
//  原匿名块符号提升为全局，统一经本头声明（判据：行为输出逐字节不变）。
// =============================================================================
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "graph.h" // aiwrite::engine::Graph / Edge

// ---- 命令行选项（main 构造 → test_chat / test_http 读取）----
struct Options {
    bool        selftest = false;
    bool        graph_selftest = false;
    bool        exec_selftest  = false;
    bool        insecure = false;
    bool        dry_run  = false;   // --chat --dry-run：只打印/断言请求，不发网络请求
    std::string http_url;
    std::string sha3_text;
    std::string chat_prompt;
    std::string model    = "deepseek-chat";
    std::string api_base = "https://api.deepseek.com";
    std::string image_path;   // --image-decode：诊断某一图片文件（格式嗅探 / 解码 / 尺寸）
    // ---- M6（非技术用户可安装化）----
    bool diag_dump       = false; // --diag-dump：打印诊断报告（支持用；可直接重定向到文件）
    bool diag_assert     = false; // --diag-assert：诊断报告离线断言（单行 PASS/FAIL）
    bool dep_check       = false; // --dep-check：依赖自检（WebView2 运行时；缺依赖 ⇒ exit 1）
    bool settings_assert = false; // --settings-assert：设置面板校验纯函数断言（单行 PASS/FAIL）
    bool site_assert     = false; // --site-assert：自建站点闭环断言（模板纯函数 + 落盘/重载端到端）
    bool official_assert = false; // --official-assert：自建官方 API 提供商闭环断言（自定义 AI 收口）
    bool autosave_assert = false; // --autosave-assert：自动保存 / 崩溃恢复断言（PC-01 / PC-02）
};

// ---- 自检断言辅助（图模型 / 执行器 / 执行核心三 TU 共享）----
struct Check {
    int passed = 0;
    int failed = 0;
};

void expect(Check& check, bool ok, const std::string& name, const std::string& detail = {});
void expect_eq(Check& check, const std::string& actual, const std::string& wanted,
               const std::string& name);
std::string add_node(Check& check, aiwrite::engine::Graph& graph, const std::string& type,
                     const std::string& name);
bool contains_text(const std::vector<std::string>& messages, const std::string& needle);
aiwrite::engine::Edge make_edge(const std::string& id, const std::string& from_node,
                                const std::string& from_port, const std::string& to_node,
                                const std::string& to_port);
void set_param(aiwrite::engine::Graph& graph, const std::string& node_id,
               const std::string& param_id, const nlohmann::json& value);

// ---- 协议 / 网络 / CLI 单测（api_probe_selftest.cpp）----
void printUsage();
bool test_sha3();
bool test_chat_request_shape(const Options& options);
bool test_config_roundtrip();
bool test_web_session_and_visibility();
bool test_workflow_files();
int  test_http(const std::string& url, bool insecure);
int  test_chat(const Options& options);
bool test_nokey_request_shape();
bool test_cancel_and_timeout();
bool test_protocol_requests();
bool test_factory_and_capability();
bool test_network_and_credentials();
bool test_protocol_loopback();

// ---- 自检主入口（api_graph_selftest.cpp / api_executor_selftest.cpp /
//       api_execution_selftest.cpp）----
int graph_selftest();
int executor_selftest();
int execution_selftest();

// ---- 诊断 / 断言（api_diag.cpp）----
int  image_decode_report(const std::string& path);
bool test_diag_report();
bool test_dep_check();
bool test_settings_assert();
bool test_site_assert();
bool test_official_assert();
bool test_autosave_assert();
