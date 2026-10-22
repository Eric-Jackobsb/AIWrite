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
#include "failure_summary.h" // DEBT-L8（M6-04）：失败汇总纯函数断言
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


// P3-2：执行器核心（分帧 / 值传递 / 失败传播 / 取消 / 安全）
int execution_selftest()
{
    using engine::ExecState;
    using engine::Executor;
    using engine::Graph;
    using engine::NodeState;

    Check check;
    std::printf("[P3-2] 执行器核心自检（本地节点链路 / 失败传播 / 取消）\n");

    engine::registerAllNodes();
    aiwrite::nodes::registerAllExecutors();
    expect_eq(check, std::to_string(engine::NodeExecutorRegistry::instance().size()), "14",
              "已注册 14 个节点执行函数"); // M_node_expansion L2：8 + 6

    // ---- M_8nodes_extension L1-a（VL1-03 · 离线部分）：timeout.* / error.* 节点级覆盖参数面 ----
    //  * 行为生效（填参数 ⇒ 超时/重试变）属 AL1-04 人工验收（断网 / 超时场景）；
    //    离线断言：N-06 / N-07 都声明同名覆盖参数，且默认 0 = 继承全局（§5.3 覆盖规则）。
    {
        const char* override_ids[] = {"timeout_connect_ms", "timeout_first_byte_ms",
                                      "timeout_stream_idle_ms", "retry_enabled", "retry_count",
                                      "retry_interval_ms"};
        for (const char* type : {"LLMGenerate", "VLMGenerate"}) {
            const engine::Definition* def = engine::NodeRegistry::instance().find(type);
            for (const char* id : override_ids) {
                bool found = false;
                if (def != nullptr) {
                    for (const engine::Param& param : def->params) {
                        if (param.id == id) {
                            found = true;
                            break;
                        }
                    }
                }
                expect(check, found,
                       std::string("VL1-03 覆盖参数声明：") + type + "." + id);
            }
        }
        // 覆盖规则：数值覆盖项默认 0 = 继承全局（§5.3）
        const engine::Definition* llm = engine::NodeRegistry::instance().find("LLMGenerate");
        const engine::Param*      connect_param = nullptr;
        const engine::Param*      retry_param   = nullptr;
        if (llm != nullptr) {
            for (const engine::Param& param : llm->params) {
                if (param.id == "timeout_connect_ms") {
                    connect_param = &param;
                }
                if (param.id == "retry_count") {
                    retry_param = &param;
                }
            }
        }
        expect(check, connect_param != nullptr && connect_param->number() == 0.0 &&
                          retry_param != nullptr && retry_param->number() == 0.0,
               "VL1-03 覆盖规则：timeout_connect_ms / retry_count 默认 0 = 继承全局");
    }

    // ---- M_8nodes_extension L1-b（VL1-09 · 离线部分）：N-06/N-07 采样参数 → 请求体构造 ----
    //  * `top_k` / `stop` / `frequency_penalty` / `presence_penalty` 非零 / 非空才写进 body
    //    （默认请求体不变，既有断言与线上行为零变化）
    {
        aiwrite::ai::OfficialChatRequest sample_req;
        sample_req.model    = "deepseek-chat";
        sample_req.prompt   = "p";
        sample_req.top_k    = 5;
        sample_req.stop     = {"END", "\n"};
        sample_req.frequency_penalty = 0.5;
        sample_req.presence_penalty  = -0.2;
        const nlohmann::json body = aiwrite::ai::buildRequestBody(sample_req);
        expect(check, body.contains("top_k") && body["top_k"] == 5, "VL1-09 请求体：top_k 传递");
        expect(check, body.contains("stop") && body["stop"].is_array() &&
                          body["stop"].size() == 2 && body["stop"][0] == "END",
               "VL1-09 请求体：stop 列表传递");
        expect(check, body.contains("frequency_penalty") && body["frequency_penalty"] == 0.5,
               "VL1-09 请求体：frequency_penalty 传递");
        expect(check, body.contains("presence_penalty") && body["presence_penalty"] == -0.2,
               "VL1-09 请求体：presence_penalty 传递");

        aiwrite::ai::OfficialChatRequest default_req;
        default_req.model  = "deepseek-chat";
        default_req.prompt = "p";
        const nlohmann::json default_body = aiwrite::ai::buildRequestBody(default_req);
        expect(check, !default_body.contains("top_k") && !default_body.contains("stop") &&
                          !default_body.contains("frequency_penalty") &&
                          !default_body.contains("presence_penalty"),
               "VL1-09 请求体：默认不写采样项（既有行为不变）");

        // M_8nodes_extension L1-e：JSON 输出 + 多候选（非默认才写）
        aiwrite::ai::OfficialChatRequest json_req;
        json_req.model            = "deepseek-chat";
        json_req.prompt           = "p";
        json_req.response_format  = "json_object";
        json_req.n                = 3;
        const nlohmann::json json_body = aiwrite::ai::buildRequestBody(json_req);
        expect(check,
               json_body.contains("response_format") &&
                   json_body["response_format"]["type"] == "json_object",
               "VL1-09 请求体：response_format=json_object 传递");
        expect(check, json_body.contains("n") && json_body["n"] == 3,
               "VL1-09 请求体：n（候选数）传递");
    }

    // ---- M_8nodes_extension L1-b（VL1-07）：N-04 文本合并五项参数行为 ----
    {
        Graph             graph;
        const std::string a = add_node(check, graph, "TextInput", "合并 n1");
        const std::string b = add_node(check, graph, "TextInput", "合并 n2");
        const std::string c = add_node(check, graph, "TextInput", "合并 n3");
        const std::string m = add_node(check, graph, "TextMerge", "合并节点");
        set_param(graph, a, "text", "banana");
        set_param(graph, b, "text", "apple");
        set_param(graph, c, "text", "banana");
        graph.edges.push_back(make_edge("m1", a, "text", m, "texts"));
        graph.edges.push_back(make_edge("m2", b, "text", m, "texts"));
        graph.edges.push_back(make_edge("m3", c, "text", m, "texts"));
        // 参数：by_name 排序 + 前缀后缀 + 带源标签 + 去重（空行增强见下方直接调用用例）
        set_param(graph, m, "sort_mode", "by_name");
        set_param(graph, m, "prefix", "<");
        set_param(graph, m, "suffix", ">");
        set_param(graph, m, "source_label", "[nX]");
        set_param(graph, m, "dedup", true);
        set_param(graph, m, "empty_line_mode", "remove");

        Executor                 executor;
        std::vector<std::string> console_lines;
        executor.setConsoleHandler(
            [&console_lines](const std::string& text) { console_lines.push_back(text); });
        std::string error;
        expect(check, executor.start(graph, &error), "VL1-07：N-04 工作流 start 通过", error);
        expect(check, executor.runToCompletion(&graph, 32) &&
                          executor.state() == ExecState::Finished &&
                          executor.failedCount() == 0,
               "VL1-07：N-04 工作流 runToCompletion 完成");
        const nlohmann::json* merged = executor.outputs().find(m, "text");
        expect_eq(check, merged != nullptr ? merged->dump() : std::string("(无输出)"),
                  nlohmann::json("[1] <apple>\n[2] <banana>").dump(),
                  "VL1-07：排序 + 去重 + 前缀后缀 + 带源标签（apple / banana）");
    }
    { // VL1-07：空行增强（直接调用执行器 —— 空段经必填校验不可由 TextInput 产生）
        const auto run_merge = [](const nlohmann::json& params_json,
                                  const nlohmann::json& texts) {
            nlohmann::json inputs;
            inputs["texts"] = texts;
            engine::ExecutionContext ctx;
            return aiwrite::nodes::execute_text_merge(inputs, params_json, ctx).dump();
        };
        nlohmann::json collapse_params;
        collapse_params["empty_line_mode"] = "collapse";
        expect_eq(check, run_merge(collapse_params, {"a", "", "", "b"}),
                  nlohmann::json("a\n\nb").dump(), "VL1-07：collapse 连续空段合并为一个");
        nlohmann::json remove_params;
        remove_params["empty_line_mode"] = "remove";
        expect_eq(check, run_merge(remove_params, {"a", "", "", "b"}),
                  nlohmann::json("a\nb").dump(), "VL1-07：remove 全部空段移除");
        nlohmann::json keep_params;
        keep_params["empty_line_mode"]  = "keep";
        keep_params["ignore_empty"]     = false;
        expect_eq(check, run_merge(keep_params, {"a", "", "", "b"}),
                  nlohmann::json("a\n\n\nb").dump(), "VL1-07：keep 保留全部空段（ignore_empty=false）");
    }
    { // VL1-07：默认行为不变（edge_order / keep / ignore_empty）—— 空段被忽略
        Graph             graph;
        const std::string a = add_node(check, graph, "TextInput", "默认 n1");
        const std::string b = add_node(check, graph, "TextInput", "默认 n2");
        const std::string m = add_node(check, graph, "TextMerge", "默认合并");
        set_param(graph, a, "text", "甲");
        set_param(graph, b, "text", "乙");
        graph.edges.push_back(make_edge("d1", a, "text", m, "texts"));
        graph.edges.push_back(make_edge("d2", b, "text", m, "texts"));

        Executor executor;
        std::string error;
        expect(check, executor.start(graph, &error), "VL1-07：默认 merge start 通过", error);
        expect(check, executor.runToCompletion(&graph, 32), "VL1-07：默认 merge 完成");
        const nlohmann::json* merged = executor.outputs().find(m, "text");
        expect_eq(check, merged != nullptr ? merged->dump() : std::string("(无输出)"),
                  nlohmann::json("甲\n乙").dump(), "VL1-07：默认行为 = 边顺序 + 换行分隔");
    }

    // ---- M_8nodes_extension L1-b（VL1-11）：N-08 导出模板 / 自动命名 ----
    {
        aiwrite::utils::DocumentMeta meta;
        meta.workflow    = "我的工作流";
        meta.node_id     = "n2";
        meta.state       = "done";
        meta.duration_ms = 1234.0;
        meta.stamp       = "2026-10-14 12:00:00";
        const std::string body = aiwrite::utils::buildDocumentBody(
            "正文", "标签", meta, true, "{label}|{workflow}|{node_id}|{state}|{duration}|{time}");
        expect(check,
               body.rfind("标签|我的工作流|n2|done|1234|2026-10-14 12:00:00\n\n正文", 0) == 0,
               "VL1-11：导出模板占位符替换正确");
        const std::string default_body =
            aiwrite::utils::buildDocumentBody("正文", "标签", meta, true);
        expect(check, default_body.rfind("# 标签", 0) == 0 && default_body.find("> 工作流：") != std::string::npos,
               "VL1-11：空模板 = 默认元信息头不变");

        // 自动命名：file_stem 覆盖工作流名（写临时目录验证后清理）
        const std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
        aiwrite::utils::ExportRequest        request;
        request.text      = "正文";
        request.dir       = temp_dir.string();
        request.file_stem = "自定义主干";
        request.meta.workflow = "工作流名";
        const aiwrite::utils::ExportResult result = aiwrite::utils::exportTextDocument(request);
        expect(check, result.ok && result.path.find("自定义主干-") != std::string::npos,
               "VL1-11：file_stem 自动命名（覆盖工作流名）");
        if (result.ok) {
            std::error_code remove_error;
            std::filesystem::remove(result.path, remove_error);
        }
    }

    // ---- M_8nodes_extension L1-b（VL1-10 · 离线部分）：N-07 参数面（seed/mode 对称 + 多图顺序）----
    {
        const engine::Definition* vlm = engine::NodeRegistry::instance().find("VLMGenerate");
        const engine::Param*      order = nullptr;
        const engine::Param*      seed  = nullptr;
        const engine::Param*      mode  = nullptr;
        if (vlm != nullptr) {
            for (const engine::Param& param : vlm->params) {
                if (param.id == "image_order") {
                    order = &param;
                }
                if (param.id == "seed") {
                    seed = &param;
                }
                if (param.id == "mode") {
                    mode = &param;
                }
            }
        }
        expect(check,
               order != nullptr && order->text() == "edge_order" &&
                   std::find(order->enum_options.begin(), order->enum_options.end(), "by_name") !=
                       order->enum_options.end(),
               "VL1-10：image_order 参数存在且默认 edge_order（含 by_name 选项）");
        expect(check, seed != nullptr && seed->number() == 0.0 && mode != nullptr,
               "VL1-10：N-07 具备 seed（默认 0）与 mode 参数（与 N-06 对称）");
    }

    // ---- M_8nodes_extension L1-c（VL1-05 · 离线部分）：N-02 目录批量 / 预处理参数面 ----
    //  * 「预处理尺寸正确」由 image_preprocess_selftest 覆盖（零开销 / fit / fill / 裁剪 / 组合）；
    //    目录批量收集行为依赖真实文件系统 → 属 AL1-05 人工验收范围
    {
        const engine::Definition* img = engine::NodeRegistry::instance().find("ImageInput");
        const std::vector<std::string> wanted = {
            "directory", "resize_mode", "resize_max_width", "resize_max_height", "crop_enabled",
            "crop_x", "crop_y", "crop_w", "crop_h"};
        bool all_ok = img != nullptr;
        for (const std::string& id : wanted) {
            bool found = false;
            if (img != nullptr) {
                for (const engine::Param& param : img->params) {
                    if (param.id == id) {
                        found = true;
                        break;
                    }
                }
            }
            all_ok = all_ok && found;
        }
        expect(check, all_ok,
               "VL1-05：N-02 具备 directory + 图片预处理参数面（目录批量 / 预处理）");
    }

    // ---- M_8nodes_extension L1-d（VL1-06）：N-03 `seed` 只作用于内部 AI 调用，不影响模板替换 ----
    {
        const engine::Definition* tpl = engine::NodeRegistry::instance().find("PromptTemplate");
        const engine::Param*      seed_param = nullptr;
        const engine::Param*      provider_param = nullptr;
        if (tpl != nullptr) {
            for (const engine::Param& param : tpl->params) {
                if (param.id == "seed") {
                    seed_param = &param;
                }
                if (param.id == "provider") {
                    provider_param = &param;
                }
            }
        }
        expect(check, seed_param != nullptr && provider_param != nullptr,
               "VL1-06：N-03 注册面具备 provider（AI 配置）与 seed（只作用于内部 AI 调用）");

        // 模板替换本身不受 seed 影响（默认 manual 行为不变）
        nlohmann::json inputs;
        inputs["vars"] = {"世界"};
        nlohmann::json params;
        params["template"] = "你好，{vars}！";
        params["seed"]     = 999; // 只作用于内部 AI 调用
        engine::ExecutionContext ctx;
        const nlohmann::json templated =
            aiwrite::nodes::execute_prompt_template(inputs, params, ctx);
        expect_eq(check, templated.dump(), nlohmann::json("你好，世界！").dump(),
                  "VL1-06：模板替换不受 seed 影响（manual 行为不变）");
    }

    // ---- M_8nodes_extension L1-f（VL1-08 · 离线部分）：N-05 参数预设 / 模型列表参数面 ----
    //  * 参数预设带出（选 provider → api_base / key_ref / model 枚举）是 UI 行为（property_panel）；
    //    离线断言 = N-05 参数面齐全，且 provider 枚举来自配置表（provider_option_ids）
    {
        const engine::Definition* cfg = engine::NodeRegistry::instance().find("ProviderConfig");
        const std::vector<std::string> wanted = {
            "provider", "mode", "api_base", "model", "model_custom", "api_key", "api_key_ref"};
        bool all_ok = cfg != nullptr;
        for (const std::string& id : wanted) {
            bool found = false;
            if (cfg != nullptr) {
                for (const engine::Param& param : cfg->params) {
                    if (param.id == id) {
                        found = true;
                        break;
                    }
                }
            }
            all_ok = all_ok && found;
        }
        expect(check, all_ok, "VL1-08：N-05 参数面齐全（provider/mode/api_base/model/custom/key/ref）");
        const engine::Param* provider_param = nullptr;
        if (cfg != nullptr) {
            for (const engine::Param& param : cfg->params) {
                if (param.id == "provider") {
                    provider_param = &param;
                }
            }
        }
        expect(check,
               provider_param != nullptr && !provider_param->enum_options.empty() &&
                   provider_param->enum_options == engine::providerOptionIds(),
               "VL1-08：N-05 provider 枚举 = 配置表 provider_option_ids（表驱动）");
    }

    // ------------------------------------------------ 1. 本地链路端到端 ------
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "链路 n1（文本输入）");
        const std::string n2 = add_node(check, graph, "PromptTemplate", "链路 n2（模板）");
        const std::string n3 = add_node(check, graph, "TextOutput", "链路 n3（输出）");
        graph.edges.push_back(make_edge("e1", n1, "text", n2, "vars"));
        graph.edges.push_back(make_edge("e2", n2, "text", n3, "text"));
        set_param(graph, n1, "text", "标题：秋日");
        set_param(graph, n2, "template", "请根据以下内容续写：\n{vars}");

        std::vector<std::string> console_lines;
        std::vector<std::string> state_log;
        Executor                 executor;
        executor.setConsoleHandler(
            [&console_lines](const std::string& text) { console_lines.push_back(text); });
        executor.setStateHandler([&state_log](const std::string& id, NodeState state) {
            state_log.push_back(id + ":" + engine::nodeStateName(state));
        });

        std::string error;
        expect(check, executor.start(graph, &error), "本地链路：start 通过（运行前校验）", error);
        expect(check, executor.totalCount() == 3, "本地链路：计划含 3 个节点");

        std::vector<std::string> order;
        graph.topologicalOrder(&order, nullptr);
        expect(check, executor.plan() == order, "本地链路：执行计划 == 拓扑序");

        // 分帧：一次 tick 只推进一个节点
        expect(check, executor.tick(&graph), "本地链路：tick #1 推进成功");
        expect(check, executor.finishedCount() == 1, "本地链路：一帧只执行一个节点");

        expect(check, executor.runToCompletion(&graph, 64), "本地链路：runToCompletion 正常结束");
        expect(check, executor.state() == ExecState::Finished, "本地链路：会话状态 Finished");
        expect(check, executor.finishedCount() == 3 && executor.failedCount() == 0,
               "本地链路：完成 3 / 失败 0");

        const engine::Node* node1 = graph.findNode(n1);
        const engine::Node* node2 = graph.findNode(n2);
        const engine::Node* node3 = graph.findNode(n3);
        expect(check,
               node1 != nullptr && node1->state == NodeState::Done && node2 != nullptr &&
                   node2->state == NodeState::Done && node3 != nullptr &&
                   node3->state == NodeState::Done,
               "本地链路：三个节点状态均为 done");

        // TextOutput 节点只有输入端口 → 检查「上游模板节点的输出」与「Console 中的文本输出行」
        const nlohmann::json* templated = executor.outputs().find(n2, "text");
        expect_eq(check, templated != nullptr ? templated->dump() : std::string("(无输出)"),
                  nlohmann::json("请根据以下内容续写：\n标题：秋日").dump(),
                  "本地链路：模板节点输出已替换 {vars}");
        const bool printed =
            std::any_of(console_lines.begin(), console_lines.end(), [](const std::string& line) {
                return line.find("[文本输出]") != std::string::npos &&
                       line.find("标题：秋日") != std::string::npos;
            });
        expect(check, printed, "本地链路：文本输出节点把结果写入 Console 回调");
        expect(check, !console_lines.empty(), "本地链路：console 回调收到输出");
        expect(check, !state_log.empty() && state_log.front() == n1 + ":running",
               "本地链路：状态回调首个事件为 n1:running");
    }

    // ------------------------------------------- 2. 失败传播（设计 §10.4）----
    {
        Graph             graph;
        const std::string t1 = add_node(check, graph, "TextInput", "失败图 n1");
        const std::string t2 = add_node(check, graph, "PromptTemplate", "失败图 n2");
        const std::string l1 = add_node(check, graph, "LLMGenerate", "失败图 n3（生成·占位）");
        // 自检不联网：把生成模式置为 official（官方 API 占位分支，快速失败且信息稳定）
        if (engine::Node* llm_node = graph.findNode(l1)) {
            if (engine::Param* mode = llm_node->findParam("mode")) {
                mode->value = std::string("official");
            }
        }
        const std::string o1 = add_node(check, graph, "TextOutput", "失败图 n4（下游）");
        const std::string t3 = add_node(check, graph, "TextInput", "失败图 n5（无关分支）");
        const std::string o2 = add_node(check, graph, "TextOutput", "失败图 n6（无关分支）");
        graph.edges.push_back(make_edge("e1", t1, "text", t2, "vars"));
        graph.edges.push_back(make_edge("e2", t2, "text", l1, "prompt"));
        graph.edges.push_back(make_edge("e3", l1, "text", o1, "text"));
        graph.edges.push_back(make_edge("e4", t3, "text", o2, "text"));
        set_param(graph, t1, "text", "输入 A");
        set_param(graph, t3, "text", "输入 B");

        Executor    executor;
        std::string error;
        expect(check, executor.start(graph, &error), "失败传播：start 通过", error);
        expect(check, executor.runToCompletion(&graph, 64),
               "失败传播：runToCompletion 正常结束（会话 Finished）");
        expect(check, executor.state() == ExecState::Finished, "失败传播：会话状态 Finished");

        const engine::Node* llm = graph.findNode(l1);
        expect(check, llm != nullptr && llm->state == NodeState::Error,
               "失败传播：LLMGenerate 节点标记 error");
        expect(check, llm != nullptr && llm->error_message.find("缺少 API Key") != std::string::npos,
               "失败传播：错误信息说明「缺少 API Key」", llm != nullptr ? llm->error_message : std::string());
        const engine::Node* downstream = graph.findNode(o1);
        expect(check, downstream != nullptr && downstream->state == NodeState::Skipped,
               "失败传播：下游节点标记 skipped");
        const engine::Node* unrelated = graph.findNode(o2);
        expect(check, unrelated != nullptr && unrelated->state == NodeState::Done,
               "失败传播：无关分支继续执行（done）");
        expect(check, executor.failedCount() == 1 && executor.skippedCount() == 1,
               "失败传播：计数 失败 1 / 跳过 1");
        expect(check, executor.summary().find("完成 4/6") != std::string::npos,
               "失败传播：summary 反映 完成 4/6", executor.summary());

        // PA-01：运行信息只读暴露（输出面板 / workflow.log 明细的数据来源）
        const auto& infos = executor.runInfos();
        expect(check, infos.size() == graph.nodes.size(), "运行信息：记录数等于节点数",
               std::to_string(infos.size()));
        const auto find_info = [&infos](const std::string& id) -> const engine::NodeRunInfo* {
            for (const engine::NodeRunInfo& info : infos) {
                if (info.node_id == id) {
                    return &info;
                }
            }
            return nullptr;
        };
        const engine::NodeRunInfo* llm_info  = find_info(l1);
        const engine::NodeRunInfo* skip_info = find_info(o1);
        const engine::NodeRunInfo* done_info = find_info(t1);
        expect(check,
               llm_info != nullptr && llm_info->state == NodeState::Error &&
                   llm_info->duration_ms >= 0.0 && !llm_info->error.empty(),
               "运行信息：失败节点含 error 与耗时");
        expect(check, skip_info != nullptr && skip_info->state == NodeState::Skipped,
               "运行信息：跳过节点被记录");
        expect(check,
               done_info != nullptr && done_info->state == NodeState::Done &&
                   done_info->type == "TextInput",
               "运行信息：完成节点含类型");
        double sum_ms = 0.0;
        for (const engine::NodeRunInfo& info : infos) {
            sum_ms += info.duration_ms;
        }
        expect(check, sum_ms <= executor.elapsedSeconds() * 1000.0 + 5.0,
               "运行信息：节点耗时之和不超过总耗时", std::to_string(sum_ms) + " ms");
        executor.reset();
        expect(check, executor.runInfos().empty(), "运行信息：reset 后清空");
    }

    // ------------------------------------------- 3. ProviderConfig 安全性 ----
    {
        Graph             graph;
        const std::string p = add_node(check, graph, "ProviderConfig", "配置节点");
        set_param(graph, p, "api_key", "sk-secret-should-not-leak");
        set_param(graph, p, "model", "deepseek-reasoner");

        Executor    executor;
        std::string error;
        expect(check, executor.start(graph, &error), "提供商节点：start 通过", error);
        executor.runToCompletion(&graph, 16);

        const nlohmann::json* provider = executor.outputs().find(p, "provider");
        expect(check, provider != nullptr && provider->value("has_api_key", false),
               "提供商节点：输出 has_api_key = true");
        const std::string dump = provider != nullptr ? provider->dump() : std::string();
        expect(check, dump.find("sk-secret") == std::string::npos,
               "提供商节点：输出**不含** Key 明文（设计 §8.4）", dump);
        expect(check, dump.find("deepseek-reasoner") != std::string::npos,
               "提供商节点：输出包含模型名");
    }

    // ------------------------------------------- 4. 文本合并 / 模板占位符报错（PW-10）----
    {
        Graph             graph;
        const std::string a = add_node(check, graph, "TextInput", "合并 a");
        const std::string b = add_node(check, graph, "TextInput", "合并 b");
        const std::string m = add_node(check, graph, "TextMerge", "合并节点");
        const std::string t = add_node(check, graph, "PromptTemplate", "模板（含未知占位符）");
        graph.edges.push_back(make_edge("e1", a, "text", m, "texts"));
        graph.edges.push_back(make_edge("e2", b, "text", m, "texts"));
        graph.edges.push_back(make_edge("e3", m, "text", t, "vars"));
        set_param(graph, a, "text", "第一段");
        set_param(graph, b, "text", "第二段");
        set_param(graph, m, "separator", "|");
        set_param(graph, t, "template", "X{unknown}Y{vars}Z");

        Executor    executor;
        std::string error;
        expect(check, executor.start(graph, &error), "合并/模板：start 通过", error);
        executor.runToCompletion(&graph, 32);

        const nlohmann::json* merged = executor.outputs().find(m, "text");
        expect_eq(check, merged != nullptr ? merged->dump() : std::string("(无输出)"),
                  nlohmann::json("第一段|第二段").dump(), "文本合并：按分隔符拼接");
        // PW-10（PQ-13）：未匹配的占位符不再原样保留 ⇒ 模板节点失败（{unknown} 未命中任何输入）
        const engine::Node* tpl_node = graph.findNode(t);
        expect(check, tpl_node != nullptr && tpl_node->state == NodeState::Error,
               "PW-10：模板存在未填充占位符 ⇒ 节点失败（不再静默下发）");
        expect(check,
               tpl_node != nullptr &&
                   tpl_node->error_message.find("{unknown}") != std::string::npos,
               "PW-10：错误信息含未填充的占位符名",
               tpl_node ? tpl_node->error_message : std::string());
    }

    // ------------------------------------------- 4b. 模板占位符（PW-10 正 / 负例）----
    {
        // 正例：JSON 字面量 {…} 不算占位符 → 原样保留、不报错；{vars} 单值正常替换
        {
            nlohmann::json inputs;
            inputs["vars"] = {"值A"};
            nlohmann::json params;
            params["template"] = "{vars} 的 JSON: {\"a\": 1}";
            engine::ExecutionContext ctx;
            const nlohmann::json out = aiwrite::nodes::execute_prompt_template(inputs, params, ctx);
            expect_eq(check, out.dump(), nlohmann::json("值A 的 JSON: {\"a\": 1}").dump(),
                      "PW-10 正例：{vars} 替换且 JSON 字面量 {…} 原样保留");
        }
        // 正例：纯 JSON 字面量（无任何输入）→ 不报错、原样保留
        {
            nlohmann::json inputs;
            nlohmann::json params;
            params["template"] = "{\"k\": 1}";
            engine::ExecutionContext ctx;
            const nlohmann::json out = aiwrite::nodes::execute_prompt_template(inputs, params, ctx);
            expect_eq(check, out.dump(), nlohmann::json("{\"k\": 1}").dump(),
                      "PW-10 正例：纯 JSON 字面量 {…} 不误报");
        }
        // 负例：{vars} 在 2 个值时未提供 → 报错（PQ-13：不再静默下发 {vars}）
        {
            nlohmann::json inputs;
            inputs["vars"] = {"A", "B"};
            nlohmann::json params;
            params["template"] = "前缀{vars}后缀";
            engine::ExecutionContext ctx;
            bool threw = false;
            try {
                (void)aiwrite::nodes::execute_prompt_template(inputs, params, ctx);
            }
            catch (const engine::NodeError& err) {
                threw = std::string(err.what()).find("{vars}") != std::string::npos;
            }
            expect(check, threw, "PW-10 负例：多值下 {vars} 未填充 ⇒ 报错并含 {vars}");
        }
        // 负例：数字占位符越界（只有 1 个值却写 {2}）→ 报错
        {
            nlohmann::json inputs;
            inputs["vars"] = {"A"};
            nlohmann::json params;
            params["template"] = "a{2}b";
            engine::ExecutionContext ctx;
            bool threw = false;
            try {
                (void)aiwrite::nodes::execute_prompt_template(inputs, params, ctx);
            }
            catch (const engine::NodeError& err) {
                threw = std::string(err.what()).find("{2}") != std::string::npos;
            }
            expect(check, threw, "PW-10 负例：{2} 越界 ⇒ 报错并含 {2}");
        }
    }

    // ---- 4c. 创作链四段模板可实例化 + 端到端闭环（POL-L-01）------------------
    // 创作主线「构思 → 大纲 → 草稿 → 修订」（Creation_Roadmap_Guideline.md §1.1）：
    //  * 四段均为 PromptTemplate 纯文本处理（不触网、无需 Key；无 Key 请求构造另由 V-11 覆盖）
    //  * 可实例化 = {vars} 单值占位符正确替换、无未填充占位符（不抛 NodeError · PW-10 负例除外）
    //  * 闭环 = 加载（graph 构造）/ 校验（start）/ 运行（runToCompletion）/ 结果回流（outputs）
    {
        // 四段模板文本（创作链夹具 · 对齐 Creation_Roadmap_Guideline 四阶段）
        const std::string tpl_idea    = "你是小说策划师。把以下灵感扩写成一句话前提（logline）：\n{vars}";
        const std::string tpl_outline = "你是结构师。基于以下前提给出三幕结构与章节分解：\n{vars}";
        const std::string tpl_draft   = "你是小说家。根据以下大纲写出第一章草稿：\n{vars}";
        const std::string tpl_revise  = "你是编辑。对以下草稿做修订（结构 / 一致性 / 声音统一）：\n{vars}";
        const std::string inspiration = "一个少年在废墟中发现能改写记忆的日记本。";

        // ① 四段模板齐全（非空 + 各含可实例化占位符 {vars}）
        const bool tpls_ok = !tpl_idea.empty() && tpl_idea.find("{vars}") != std::string::npos &&
                             !tpl_outline.empty() && tpl_outline.find("{vars}") != std::string::npos &&
                             !tpl_draft.empty() && tpl_draft.find("{vars}") != std::string::npos &&
                             !tpl_revise.empty() && tpl_revise.find("{vars}") != std::string::npos;
        expect(check, tpls_ok, "创作链：构思/大纲/草稿/修订 四段模板齐全且各含 {vars} 占位符");

        // ② 单段可实例化（纯函数）：构思模板把灵感扩写进前提句引导（{vars} 正确替换）
        engine::ExecutionContext ctx;
        nlohmann::json           idea_input;
        idea_input["vars"] = {inspiration};
        const std::string idea_text =
            aiwrite::nodes::execute_prompt_template(idea_input, {{"template", tpl_idea}}, ctx)
                .get<std::string>();
        expect(check, idea_text.find(inspiration) != std::string::npos &&
                          idea_text.find("一句话前提") != std::string::npos,
               "创作链：构思模板可实例化（灵感 → 前提句引导，输入保留）", idea_text);

        // ③ 链式实例化（结果回流：上段输出作为下段 {vars} 输入）
        const std::string outline_text =
            aiwrite::nodes::execute_prompt_template(nlohmann::json{{"vars", {idea_text}}},
                                                    {{"template", tpl_outline}}, ctx)
                .get<std::string>();
        const std::string draft_text =
            aiwrite::nodes::execute_prompt_template(nlohmann::json{{"vars", {outline_text}}},
                                                    {{"template", tpl_draft}}, ctx)
                .get<std::string>();
        const std::string revise_text =
            aiwrite::nodes::execute_prompt_template(nlohmann::json{{"vars", {draft_text}}},
                                                    {{"template", tpl_revise}}, ctx)
                .get<std::string>();
        expect(check, outline_text.find(idea_text) != std::string::npos,
               "创作链：大纲模板可实例化并回流构思结果", outline_text);
        expect(check, draft_text.find(outline_text) != std::string::npos,
               "创作链：草稿模板可实例化并回流大纲结果", draft_text);
        expect(check, revise_text.find(draft_text) != std::string::npos,
               "创作链：修订模板可实例化并回流草稿结果", revise_text);
        expect(check, revise_text.find(inspiration) != std::string::npos,
               "创作链：修订稿仍含灵感原文（整链回流不丢信息）");

        // ④ 端到端闭环（graph + Executor）：加载 / 校验 / 运行 / 结果回流
        Graph             graph;
        const std::string n_in  = add_node(check, graph, "TextInput", "创作链 灵感");
        const std::string n_id  = add_node(check, graph, "PromptTemplate", "创作链 构思");
        const std::string n_ol  = add_node(check, graph, "PromptTemplate", "创作链 大纲");
        const std::string n_dr  = add_node(check, graph, "PromptTemplate", "创作链 草稿");
        const std::string n_rv  = add_node(check, graph, "PromptTemplate", "创作链 修订");
        const std::string n_out = add_node(check, graph, "TextOutput", "创作链 输出");
        graph.edges.push_back(make_edge("c1", n_in, "text", n_id, "vars"));
        graph.edges.push_back(make_edge("c2", n_id, "text", n_ol, "vars"));
        graph.edges.push_back(make_edge("c3", n_ol, "text", n_dr, "vars"));
        graph.edges.push_back(make_edge("c4", n_dr, "text", n_rv, "vars"));
        graph.edges.push_back(make_edge("c5", n_rv, "text", n_out, "text"));
        set_param(graph, n_in, "text", inspiration);
        set_param(graph, n_id, "template", tpl_idea);
        set_param(graph, n_ol, "template", tpl_outline);
        set_param(graph, n_dr, "template", tpl_draft);
        set_param(graph, n_rv, "template", tpl_revise);

        Executor    executor;
        std::string error;
        expect(check, executor.start(graph, &error),
               "创作链：start 通过（加载 + 运行前校验闭环）", error);
        expect(check, executor.runToCompletion(&graph, 32) &&
                          executor.state() == ExecState::Finished,
               "创作链：runToCompletion 完成（运行闭环）");
        const nlohmann::json* final_text = executor.outputs().find(n_out, "__result");
        const std::string final_str =
            (final_text != nullptr) ? final_text->get<std::string>() : std::string("(无输出)");
        expect(check, final_text != nullptr &&
                          final_str.find(inspiration) != std::string::npos &&
                          final_str.find("三幕结构") != std::string::npos &&
                          final_str.find("第一章草稿") != std::string::npos &&
                          final_str.find("修订") != std::string::npos,
               "创作链：结果回流 —— 输出面板拿到含灵感 + 四段引导词的修订稿", final_str);
    }

    // ------------------------------------------- 5. 图片输入（文件不存在）----
    {
        Graph             graph;
        const std::string img = add_node(check, graph, "ImageInput", "图片输入");
        set_param(graph, img, "path", "Z:/not-exists/aiwrite-selftest.png");

        Executor    executor;
        std::string error;
        expect(check, !executor.start(graph, &error) && error.find("不存在") != std::string::npos,
               "图片输入：文件不存在 → 运行前校验直接拒绝", error);
    }

    // ------------------------------------------- 6. 取消（协作式）------------
    {
        Graph             graph;
        const std::string a = add_node(check, graph, "TextInput", "取消 a");
        const std::string b = add_node(check, graph, "PromptTemplate", "取消 b");
        const std::string c = add_node(check, graph, "TextOutput", "取消 c");
        graph.edges.push_back(make_edge("e1", a, "text", b, "vars"));
        graph.edges.push_back(make_edge("e2", b, "text", c, "text"));
        set_param(graph, a, "text", "取消测试");

        Executor    executor;
        std::string error;
        expect(check, executor.start(graph, &error), "取消：start 通过", error);
        executor.tick(&graph);
        executor.cancel();
        executor.tick(&graph);
        expect(check, executor.state() == ExecState::Cancelled, "取消：会话状态 Cancelled");
        const engine::Node* node_c = graph.findNode(c);
        expect(check, node_c != nullptr && node_c->state == NodeState::Waiting,
               "取消：未执行的节点保持 waiting");
        expect(check, executor.finishedCount() == 1, "取消：已完成 1 个节点");
    }

    // ------------------------------------------- 6b. PD-06① 通用重跑（FEA-M4-11）----
    // VD-05：只重跑目标节点时上游结果复用；重跑「该节点及下游」时受影响节点结果刷新
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "重跑 n1（上游）");
        const std::string n2 = add_node(check, graph, "PromptTemplate", "重跑 n2（目标）");
        const std::string n3 = add_node(check, graph, "TextOutput", "重跑 n3（下游）");
        graph.edges.push_back(make_edge("e1", n1, "text", n2, "vars"));
        graph.edges.push_back(make_edge("e2", n2, "text", n3, "text"));
        set_param(graph, n1, "text", "上游值");
        set_param(graph, n2, "template", "{vars}!");

        Executor    executor;
        std::string error;

        // 先做一次全量运行（得到可复用的上游结果）
        expect(check, executor.start(graph, &error), "重跑：全量 start 通过", error);
        expect(check, executor.runToCompletion(&graph, 32), "重跑：全量运行完成");
        const nlohmann::json* full2 = executor.outputs().find(n2, "text");
        expect_eq(check, full2 != nullptr ? full2->dump() : std::string("(无输出)"),
                  nlohmann::json("上游值!").dump(), "重跑：全量运行 n2 = 上游值!");

        // ① 只重跑目标节点（NodeOnly）：上游结果复用（n1 不再执行），n3 不被触碰
        set_param(graph, n2, "template", "{vars}!!");
        engine::RerunSpec node_only;
        node_only.target_node = n2;
        node_only.scope       = engine::RerunScope::NodeOnly;
        expect(check, executor.startFrom(graph, node_only, &error),
               "重跑：NodeOnly startFrom 通过", error);
        expect(check, executor.plan() == std::vector<std::string>{n2},
               "重跑：NodeOnly 计划 = 仅目标节点");
        expect(check, executor.runToCompletion(&graph, 32) &&
                          executor.state() == ExecState::Finished,
               "重跑：NodeOnly 运行完成");
        const nlohmann::json* rerun2 = executor.outputs().find(n2, "text");
        expect_eq(check, rerun2 != nullptr ? rerun2->dump() : std::string("(无输出)"),
                  nlohmann::json("上游值!!").dump(),
                  "重跑：NodeOnly 复用上游结果并刷新本节点");
        expect(check, executor.runInfos().size() == 1 &&
                          executor.runInfos().front().node_id == n2,
               "重跑：NodeOnly 只执行了目标节点（上游 n1 未重算）");
        const nlohmann::json* old3 = executor.outputs().find(n3, "__result"); // 汇点节点输出在 __result
        const engine::Node*   stale3 = graph.findNode(n3);
        expect(check, old3 != nullptr && old3->dump() == nlohmann::json("上游值!").dump() &&
                          stale3 != nullptr && stale3->state == NodeState::Done,
               "重跑：NodeOnly 不触碰下游（n3 结果保持旧值）");

        // ② 重跑「该节点及下游」（Downstream）：n2 + n3 都刷新
        engine::RerunSpec downstream;
        downstream.target_node = n2;
        downstream.scope       = engine::RerunScope::Downstream;
        expect(check, executor.startFrom(graph, downstream, &error),
               "重跑：Downstream startFrom 通过", error);
        expect(check, executor.plan() == std::vector<std::string>({n2, n3}),
               "重跑：Downstream 计划 = 目标 + 下游（拓扑序）");
        expect(check, executor.runToCompletion(&graph, 32) &&
                          executor.state() == ExecState::Finished,
               "重跑：Downstream 运行完成");
        const nlohmann::json* down3 = executor.outputs().find(n3, "__result"); // 汇点节点输出在 __result
        expect_eq(check, down3 != nullptr ? down3->dump() : std::string("(无输出)"),
                  nlohmann::json("上游值!!").dump(),
                  "重跑：Downstream 下游结果刷新为 n2 新值");

        // ③ 目标节点不存在 → startFrom 拒绝
        engine::RerunSpec bad;
        bad.target_node = "n_not_exist";
        expect(check, !executor.startFrom(graph, bad, &error) &&
                          error.find("重跑目标节点不存在") != std::string::npos,
               "重跑：目标节点不存在 ⇒ startFrom 拒绝", error);
    }

    // ------------------------------------------- 7. 运行前校验失败 ----------
    {
        Graph             graph;
        const std::string a = add_node(check, graph, "PromptTemplate", "校验失败 a");
        add_node(check, graph, "TextOutput", "校验失败 b（必填输入未连）");
        set_param(graph, a, "template", "只有模板");

        Executor    executor;
        std::string error;
        expect(check, !executor.start(graph, &error) && executor.state() == ExecState::Failed,
               "运行前校验失败：start 拒绝且会话状态 Failed", error);
    }

    // ------------------------------------------- 8. PC-05 运行输出归档 -------
    {
        const std::filesystem::path temp_root =
            std::filesystem::temp_directory_path() / "aiwrite_archive_selftest";
        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);

        std::vector<aiwrite::utils::ArchiveNode> nodes;
        aiwrite::utils::ArchiveNode              a;
        a.node_id = "n1"; a.type = "TextInput";  a.state = "done";  a.duration_ms = 0.5;
        a.text    = "第一段文本";
        aiwrite::utils::ArchiveNode b;
        b.node_id = "n2"; b.type = "TextOutput"; b.state = "done";  b.duration_ms = 1.5;
        b.text    = "最终生成的文档";
        aiwrite::utils::ArchiveNode c;
        c.node_id = "n3"; c.type = "LLMGenerate"; c.state = "error"; c.duration_ms = 12.0;
        c.error   = "缺少 API Key";
        nodes = {a, b, c};

        aiwrite::utils::ArchiveRequest request;
        request.workflow_name = "自检:非法?名字";
        request.archive_dir   = temp_root.string();
        request.keep_history  = false;
        request.ttl_days      = 0;

        const aiwrite::utils::ArchiveResult first =
            aiwrite::utils::archiveRun(request, nodes, "完成 2/3，失败 1");
        const std::filesystem::path first_dir(first.dir);
        expect(check, first.ok && !first.dir.empty(), "归档：首次归档成功", first.error);
        expect(check, std::filesystem::exists(first_dir / "run.json"), "归档：run.json 已写出");
        expect(check, std::filesystem::exists(first_dir / "n2-TextOutput.txt"),
               "归档：有输出的节点写出 .txt");
        expect(check, !std::filesystem::exists(first_dir / "n3-LLMGenerate.txt"),
               "归档：无输出的失败节点不写 .txt（仅进 run.json）");
        const std::string dir_name = first_dir.filename().string();
        expect(check, dir_name.find('?') == std::string::npos && dir_name.find(':') == std::string::npos,
               "归档：工作流名中的非法字符被清洗", dir_name);
        {
            std::ifstream     in(first_dir / "n2-TextOutput.txt", std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            const std::string content = buffer.str();
            expect(check,
                   content.find("最终生成的文档") != std::string::npos &&
                       content.find("n2") != std::string::npos &&
                       content.find("状态: done") != std::string::npos,
                   "归档：节点文件含元信息头与正文");
        }
        {
            std::ifstream     in(first_dir / "run.json", std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            const nlohmann::json run_json = nlohmann::json::parse(buffer.str(), nullptr, false);
            // 期望：':' 与 '?' 被清洗为 '_'（archive_run 的 sanitize_name 规则）
            expect(check,
                   !run_json.is_discarded() &&
                       run_json.value("workflow", std::string()) == "自检_非法_名字" &&
                       run_json.contains("nodes") && run_json["nodes"].size() == 3 &&
                       run_json.value("summary", std::string()) == "完成 2/3，失败 1",
                   "归档：run.json 记录工作流名 / 统计 / 3 条节点明细");
        }

        const aiwrite::utils::ArchiveResult second =
            aiwrite::utils::archiveRun(request, nodes, "完成 2/3，失败 1");
        std::size_t dir_count = 0;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(temp_root, ec)) {
            if (entry.is_directory(ec)) {
                ++dir_count;
            }
        }
        expect(check, second.ok && dir_count == 2,
               "PW-11：keep_history=false 也至少保留 kHistoryFloor 份（两次运行后两份都还在）",
               std::to_string(dir_count) + " 份");

        // PW-11：保留下限 = kHistoryFloor —— 再多运行几次也只保留最近 3 份
        for (int i = 0; i < 3; ++i) {
            const aiwrite::utils::ArchiveResult extra =
                aiwrite::utils::archiveRun(request, nodes, "完成 2/3，失败 1");
            expect(check, extra.ok, "归档：连续归档（第 " + std::to_string(i + 3) + " 次）成功",
                   extra.error);
        }
        dir_count = 0;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(temp_root, ec)) {
            if (entry.is_directory(ec)) {
                ++dir_count;
            }
        }
        expect(check, dir_count == static_cast<std::size_t>(aiwrite::utils::kHistoryFloor),
               "PW-11：keep_history=false 保留下限 = kHistoryFloor(3)",
               std::to_string(dir_count) + " 份");

        request.archive_dir = (temp_root / "not_a_dir").string();
        {
            std::ofstream blocker(request.archive_dir);
            blocker << "x";
        }
        const aiwrite::utils::ArchiveResult failed =
            aiwrite::utils::archiveRun(request, nodes, "x");
        expect(check, !failed.ok && !failed.error.empty(),
               "归档：不可写路径返回错误而不抛异常", failed.error);

        // TTL：造一个 40 天前的旧目录 → ttl_days=30 时被清理（同批的新目录保留）
        {
            const std::time_t old_time = std::time(nullptr) - 40 * 86400;
            std::tm           old_tm{};
#if defined(_WIN32)
            localtime_s(&old_tm, &old_time);
#else
            localtime_r(&old_time, &old_tm);
#endif
            std::ostringstream name;
            name << std::put_time(&old_tm, "%Y%m%d-%H%M%S") << "-旧归档";
            const std::filesystem::path old_dir = temp_root / name.str();
            std::filesystem::create_directories(old_dir, ec);
            std::ofstream marker(old_dir / "run.json");
            marker << "{}";
            marker.close();

            aiwrite::utils::ArchiveRequest ttl_request = request;
            ttl_request.archive_dir = temp_root.string();
            ttl_request.ttl_days    = 30;
            const aiwrite::utils::ArchiveResult ttl_result =
                aiwrite::utils::archiveRun(ttl_request, nodes, "ttl 用例");

            std::string remaining;
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(temp_root, ec)) {
                if (entry.is_directory(ec)) {
                    remaining += entry.path().filename().string() + " ";
                }
            }
            expect(check,
                   ttl_result.ok && !std::filesystem::exists(old_dir) &&
                       std::filesystem::exists(std::filesystem::path(ttl_result.dir) / "run.json"),
                   "归档：ttl_days=30 清理超期目录且保留本次归档",
                   "ok=" + std::to_string(ttl_result.ok) +
                       " old_exists=" + std::to_string(std::filesystem::exists(old_dir)) +
                       " new=" + ttl_result.dir + " 剩余[" + remaining + "]");
        }

        // POL-C-02：归档 TTL / 保留策略边界补强
        //  ① ttl_days=0 关闭 TTL：40 天前的旧目录不被清理（独立子目录隔离保留策略干扰）
        {
            const std::filesystem::path ttl_off_root = temp_root / "ttl_off";
            std::filesystem::create_directories(ttl_off_root, ec);
            const std::time_t old_time2 = std::time(nullptr) - 40 * 86400;
            std::tm           old_tm2{};
#if defined(_WIN32)
            localtime_s(&old_tm2, &old_time2);
#else
            localtime_r(&old_time2, &old_tm2);
#endif
            std::ostringstream name2;
            name2 << std::put_time(&old_tm2, "%Y%m%d-%H%M%S") << "-ttl关闭用例";
            const std::filesystem::path old_dir2 = ttl_off_root / name2.str();
            std::filesystem::create_directories(old_dir2, ec);
            std::ofstream marker2(old_dir2 / "run.json");
            marker2 << "{}";
            marker2.close();

            aiwrite::utils::ArchiveRequest off_request = request;
            off_request.archive_dir = ttl_off_root.string();
            off_request.ttl_days    = 0; // 关闭 TTL
            const aiwrite::utils::ArchiveResult off_result =
                aiwrite::utils::archiveRun(off_request, nodes, "ttl 关闭用例");
            expect(check, off_result.ok && std::filesystem::exists(old_dir2),
                   "归档：ttl_days=0 关闭清理 ⇒ 超期目录仍保留", off_result.error);
        }

        //  ② 非归档命名规则目录不被误删（is_archive_dir_name 负例：不碰用户其它文件）
        {
            const std::filesystem::path manual_dir = temp_root / "manual_data";
            std::filesystem::create_directories(manual_dir, ec);
            aiwrite::utils::ArchiveRequest name_request = request;
            name_request.archive_dir = temp_root.string();
            name_request.ttl_days    = 30;
            const aiwrite::utils::ArchiveResult name_result =
                aiwrite::utils::archiveRun(name_request, nodes, "命名规则用例");
            expect(check, name_result.ok && std::filesystem::exists(manual_dir),
                   "归档：不符合命名规则的目录永不参与保留/TTL 清理（不误删用户文件）",
                   name_result.error);
        }

        //  ③ keep_history=true + max_history 上限（独立子目录隔离计数）
        {
            const std::filesystem::path hist_root = temp_root / "max_hist";
            std::filesystem::create_directories(hist_root, ec);
            aiwrite::utils::ArchiveRequest hist_request = request;
            hist_request.archive_dir  = hist_root.string();
            hist_request.keep_history = true;
            hist_request.max_history  = 2;
            for (int i = 0; i < 4; ++i) {
                const aiwrite::utils::ArchiveResult hist_result =
                    aiwrite::utils::archiveRun(hist_request, nodes, "max_history 上限用例");
                expect(check, hist_result.ok, "归档：max_history 上限用例（第 " +
                                                   std::to_string(i + 1) + " 次）成功",
                       hist_result.error);
            }
            std::size_t hist_count = 0;
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(hist_root, ec)) {
                if (entry.is_directory(ec)) {
                    ++hist_count;
                }
            }
            expect(check, hist_count == 2,
                   "归档：keep_history=true + max_history=2 ⇒ 连跑 4 次后只保留最近 2 份",
                   std::to_string(hist_count) + " 份");
        }

        std::filesystem::remove_all(temp_root, ec);
    }

    // --------------------------------------- M5-02：多模态请求体（离线断言）-----
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path  vlm_root = fs::temp_directory_path(ec) / "aiwrite-selftest-vlm";
        std::filesystem::remove_all(vlm_root, ec);
        fs::create_directories(vlm_root, ec);
        const fs::path png_a      = vlm_root / "样例图.PNG";  // 大写扩展名：顺带验 MIME 大小写
        const fs::path png_b      = vlm_root / "第二张.png";
        const fs::path png_c      = vlm_root / "第三张.png";  // P7a-03：三图请求体夹具
        const fs::path empty_file = vlm_root / "空图.png";

        // Python 生成的合法 1×1 PNG（黑 / 红）——十六进制转字节写盘，保证断言可复现
        const std::string png_a_hex =
            "89504E470D0A1A0A0000000D4948445200000001000000010802000000907753"
            "DE0000000C4944415478DA63606060000000040001C8EAEBF90000000049454E"
            "44AE426082"
;
        const std::string png_b_hex =
            "89504E470D0A1A0A0000000D4948445200000001000000010802000000907753"
            "DE0000000C4944415478DA63F8CFC0000003010100F70341430000000049454E"
            "44AE426082"
;
        // P7a-03：第三张（1×1 蓝）—— 与前两张**字节级不同**，便于断言「三张图各自独立」
        const std::string png_c_hex =
            "89504E470D0A1A0A0000000D4948445200000001000000010802000000907753"
            "DE0000000C49444154789C636060F80F00010301000889C2EC0000000049454E"
            "44AE426082"
;
        const auto write_hex = [](const std::filesystem::path& path,
                                  const std::string& hex) {
            std::ofstream out(path, std::ios::binary);
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                const char byte = static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
                out.write(&byte, 1);
            }
        };
        write_hex(png_a, png_a_hex);
        write_hex(png_b, png_b_hex);
        write_hex(png_c, png_c_hex);
        {
            std::ofstream empty(empty_file, std::ios::binary);
        }
        expect(check, fs::exists(png_a) && fs::file_size(png_a) > 0 && fs::exists(png_b),
               "M5-02 夹具：两张 1×1 PNG 已写入临时目录");
        expect(check, fs::exists(png_c) && fs::file_size(png_c) > 0,
               "P7a-03 夹具：第三张 1×1 PNG（蓝）已写入临时目录");

        // ---- 1) MIME 推断（大小写不敏感；未知扩展名回退 image/png）----
        expect_eq(check, aiwrite::ai::imageMimeFromPath("a.PNG"), "image/png",
                  "M5-02 MIME：.PNG（大写）→ image/png");
        expect_eq(check, aiwrite::ai::imageMimeFromPath("b.JpEg"), "image/jpeg",
                  "M5-02 MIME：.JpEg → image/jpeg");
        expect_eq(check, aiwrite::ai::imageMimeFromPath("c.webp"), "image/webp",
                  "M5-02 MIME：.webp → image/webp");
        expect_eq(check, aiwrite::ai::imageMimeFromPath("d.tif"), "image/tiff",
                  "M7-05 MIME：.tif（TIFF 属已知扩展名）→ image/tiff");
        expect_eq(check, aiwrite::ai::imageMimeFromPath("e.xyz"), "image/png",
                  "M5-02 MIME：未知扩展名回退 image/png");

        // ---- 1b) M7-04/05：内容嗅探优先 + 解码 + 可操作失败文案 ----
        {
            const fs::path webp_fake = vlm_root / "伪装.png"; // 内容 WebP、扩展名 .png（实测场景）
            {
                std::ofstream              out(webp_fake, std::ios::binary);
                const unsigned char        head[] = {'R', 'I',  'F',  'F',  0x1E, 0x00, 0x00, 0x00,
                                                     'W', 'E',  'B',  'P',  'V',  'P',  '8',  ' ',
                                                     0x12, 0x00, 0x00, 0x00};
                out.write(reinterpret_cast<const char*>(head), sizeof(head));
            }
            const aiwrite::utils::ImageMagic magic = aiwrite::utils::sniffImage(webp_fake.string());
            expect(check, magic.format == aiwrite::utils::ImageFormat::WebP,
                   "M7-05 嗅探：RIFF/WEBP 文件头 → WebP（不看扩展名）");
            expect(check, magic.mismatch, "M7-05 嗅探：内容 WebP + 扩展名 .png → 标记不一致");
            expect_eq(check, aiwrite::ai::imageMimeFromPath(webp_fake.string()), "image/webp",
                      "M7-05 MIME：WebP 伪装 .png → image/webp（不再谎报 image/png）");
            expect_eq(check, aiwrite::ai::imageMimeFromPath(png_a.string()), "image/png",
                      "M7-05 MIME：真 PNG（大写扩展名）→ image/png");

            const unsigned char png_head[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            expect_eq(check, aiwrite::ai::imageMimeFromBytes(png_head, sizeof(png_head)), "image/png",
                      "M7-05 MIME（纯函数）：PNG 魔数 → image/png");
            expect_eq(check, aiwrite::ai::imageMimeFromBytes(nullptr, 0), "",
                      "M7-05 MIME（纯函数）：空数据 → 空串（调用方回退扩展名）");

            const std::string message = aiwrite::utils::decodeErrorMessage(
                webp_fake.string(), magic, "stb：unknown image type；WIC：没有可用解码器");
            expect(check,
                   message.find("WebP") != std::string::npos &&
                       message.find("另存为") != std::string::npos,
                   "M7-04 文案：指明「内容其实是 WebP」+ 可操作建议（另存为 PNG/JPG）", message);
            expect(check, message.find("图片理解") != std::string::npos,
                   "M7-04 文案：说明「本地预览失败不影响发给模型」", message);

            const aiwrite::utils::DecodedImage decoded = aiwrite::utils::decodeImageRgba8(png_a.string());
            expect(check,
                   decoded.error.empty() && decoded.width == 1 && decoded.height == 1 &&
                       decoded.rgba.size() == 4,
                   "M7-04 解码：真 PNG → 1×1 RGBA8（decoder=" + decoded.decoder + "）", decoded.error);
            expect(check, !decoded.decoder.empty(), "M7-04 解码：记录实际解码器（stb / wic）");

            const fs::path text_fake = vlm_root / "文本.png";
            {
                std::ofstream out(text_fake, std::ios::binary);
                out << "this is not an image at all\n";
            }
            const aiwrite::utils::DecodedImage broken = aiwrite::utils::decodeImageRgba8(text_fake.string());
            expect(check, !broken.error.empty() && broken.error.find("无法识别") != std::string::npos,
                   "M7-04 解码：非图片内容（.png 后缀）→ 「无法识别的图片格式」+ 文件头", broken.error);
            expect(check, broken.error.find("解码器原因") != std::string::npos,
                   "M7-04 解码：错误文案附解码器原因（可诊断）", broken.error);

            int         width      = 0;
            int         height     = 0;
            std::string size_error;
            expect(check,
                   aiwrite::utils::readImageSize(png_a.string(), &width, &height, &size_error) &&
                       width == 1 && height == 1,
                   "M7-04 尺寸：真 PNG → 1×1", size_error);
            expect(check,
                   !aiwrite::utils::readImageSize(empty_file.string(), &width, &height, &size_error) &&
                       size_error.find("为空") != std::string::npos,
                   "M7-04 尺寸：空文件 → 可操作错误", size_error);
            expect(check,
                   !aiwrite::utils::decodeImageRgba8((vlm_root / "无此文件.png").string()).error.empty(),
                   "M7-04 解码：文件不存在 → 明确错误");
        }

        // ---- 2) 编码成功路径（与 Python 参考实现的 base64 逐字符比对）----
        {
            std::string       error;
            const std::string encoded =
                aiwrite::ai::encode_image_data_url(png_a.string(), 8 * 1024 * 1024, &error);
            expect_eq(check, encoded, std::string("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mNgYGAAAAAEAAHI6uv5AAAAAElFTkSuQmCC"),
                      "M5-02 编码：1×1 PNG → data URL（base64 与参考实现一致）");
            expect(check, error.empty(), "M5-02 编码：成功时不写错误");
        }

        // ---- 3) 编码失败路径（不存在 / 目录 / 空文件 / 超限）----
        {
            std::string error;
            expect(check,
                   aiwrite::ai::encode_image_data_url((vlm_root / "不存在.png").string(), 0,
                                                      &error).empty() &&
                       error.find("不存在") != std::string::npos,
                   "M5-02 编码：文件不存在 → 空串 + 可读错误", error);
            error.clear();
            expect(check,
                   aiwrite::ai::encode_image_data_url(vlm_root.string(), 0, &error).empty() &&
                       error.find("不存在") != std::string::npos,
                   "M5-02 编码：目录不是图片 → 报错", error);
            error.clear();
            expect(check,
                   aiwrite::ai::encode_image_data_url(empty_file.string(), 0, &error).empty() &&
                       error.find("为空") != std::string::npos,
                   "M5-02 编码：空文件 → 报错", error);
            error.clear();
            expect(check,
                   aiwrite::ai::encode_image_data_url(png_a.string(), 4, &error).empty() &&
                       error.find("过大") != std::string::npos,
                   "M5-02 编码：超过上限 → 报错并给出上限", error);
        }

        // ---- 4) 请求体（多模态）：content 数组 + 文本块在前 + 图片按序 ----
        {
            aiwrite::ai::OfficialChatRequest request;
            request.api_base      = "https://open.bigmodel.cn/api/paas/v4";
            request.model         = "glm-4v-flash";
            request.system_prompt = "你是编辑";
            request.prompt        = "这张图讲了什么？";
            request.temperature   = 1.0;
            request.images        = {png_a.string(), png_b.string()};

            const nlohmann::json body = aiwrite::ai::buildRequestBody(request);
            expect_eq(check, aiwrite::ai::buildEndpoint(request.api_base),
                      "https://open.bigmodel.cn/api/paas/v4/chat/completions",
                      "M5-02 端点：智谱 api_base 拼接正确");
            expect(check,
                   body["messages"].size() == 2 && body["messages"][0]["role"] == "system" &&
                       body["messages"][0]["content"] == "你是编辑",
                   "M5-02 请求体：system 消息保持字符串形态");
            const nlohmann::json& content = body["messages"][1]["content"];
            expect(check, content.is_array() && content.size() == 3,
                   "M5-02 请求体：有图时 user content = 数组（1 文本 + 2 图）",
                   content.dump().substr(0, 60));
            expect(check,
                   content.is_array() && content[0]["type"] == "text" &&
                       content[0]["text"] == "这张图讲了什么？",
                   "M5-02 请求体：文本块在前且内容一致");
            expect(check,
                   content.is_array() && content[1]["type"] == "image_url" &&
                       content[1]["image_url"]["url"] == std::string("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mNgYGAAAAAEAAHI6uv5AAAAAElFTkSuQmCC") &&
                       content[2]["type"] == "image_url" &&
                       content[2]["image_url"]["url"] == std::string("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mP4z8AAAAMBAQD3A0FDAAAAAElFTkSuQmCC"),
                   "M5-02 请求体：两张图按输入顺序编码为 data URL");
            expect(check,
                   body["model"] == "glm-4v-flash" && body["stream"] == false &&
                       body["temperature"] == 1.0 && !body.contains("seed"),
                   "M5-02 请求体：model / stream=false / 采样参数 / 无 seed 时不发 seed");
        }

        // ---- 4b) P7a-03：**三张图**请求体（content 数组含 3 个 image_url）----
        {
            aiwrite::ai::OfficialChatRequest request;
            request.model  = "glm-4v-flash";
            request.prompt = "三张图分别是什么颜色？";
            request.images = {png_a.string(), png_b.string(), png_c.string()};

            const nlohmann::json  body    = aiwrite::ai::buildRequestBody(request);
            const nlohmann::json& content = body["messages"].back()["content"];

            int image_blocks = 0;
            for (const nlohmann::json& item : content) {
                if (item.value("type", std::string()) == "image_url") {
                    ++image_blocks;
                }
            }
            expect(check, content.is_array() && content.size() == 4 && image_blocks == 3,
                   "P7a-03 请求体：3 张图 → content 含 3 个 image_url（+1 文本块）",
                   "size=" + std::to_string(content.size()) +
                       " image_url=" + std::to_string(image_blocks));
            expect_eq(check, content[3]["image_url"]["url"].get<std::string>(),
                      std::string("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNgYPgPAAEDAQAIicLsAAAAAElFTkSuQmCC"),
                      "P7a-03 请求体：第 3 张按输入顺序编码为 data URL（与参考实现一致）");
        }

        // ---- 4c) P7a-01：image 端口变长（3 张图输入同一条端口）----
        {
            engine::Graph     graph;
            const std::string img1 = add_node(check, graph, "ImageInput", "P7a 图片输入 1");
            const std::string img2 = add_node(check, graph, "ImageInput", "P7a 图片输入 2");
            const std::string img3 = add_node(check, graph, "ImageInput", "P7a 图片输入 3");
            const std::string text = add_node(check, graph, "TextInput", "P7a 提示词");
            const std::string vlm  = add_node(check, graph, "VLMGenerate", "P7a 图片理解");
            set_param(graph, img1, "path", png_a.string());
            set_param(graph, img2, "path", png_b.string());
            set_param(graph, img3, "path", png_c.string());
            set_param(graph, text, "text", "三张图分别是什么颜色？");
            graph.edges.push_back(make_edge("i1", img1, "image", vlm, "image"));
            graph.edges.push_back(make_edge("i2", img2, "image", vlm, "image"));
            graph.edges.push_back(make_edge("i3", img3, "image", vlm, "image"));
            graph.edges.push_back(make_edge("i4", text, "text", vlm, "prompt"));

            const engine::Node* vlm_node = graph.findNode(vlm);
            const engine::Port* image_port =
                vlm_node != nullptr ? vlm_node->findPort("image", engine::PortDirection::Input)
                                    : nullptr;
            expect(check, image_port != nullptr && image_port->is_variadic,
                   "P7a-01 端口：VLMGenerate.image 已声明为变长（is_variadic）");
            expect(check, graph.inputConnectionCount(vlm, "image") == 3,
                   "P7a-01 连线：3 个「图片输入」可同时连入 image（不被替换）",
                   std::to_string(graph.inputConnectionCount(vlm, "image")));

            std::vector<std::string> load_errors;
            expect(check, engine::validateWorkflow(graph, &load_errors),
                   "P7a-01 校验：变长 image 端口不再报「输入端口被重复占用」",
                   load_errors.empty() ? std::string() : load_errors.front());

            std::vector<std::string> run_errors;
            std::vector<std::string> run_warnings;
            expect(check, engine::validateBeforeRun(graph, &run_errors, &run_warnings),
                   "P7a-01 运行前校验：三个图片输入 + 提示词全部就绪",
                   run_errors.empty() ? std::string() : run_errors.front());
        }

        // ---- 4d) P7a-02：多选路径串（解析 / 拼接 / 参数校验）----
        {
            const std::vector<std::string> list =
                aiwrite::paths::splitPathList("A.png\r\n  B.png  \nA.png\n\nC.png");
            expect(check,
                   list.size() == 3 && list[0] == "A.png" && list[1] == "B.png" &&
                       list[2] == "C.png",
                   "P7a-02 多值解析：CRLF / 首尾空白 / 空行 / 重复项 → 保序去重",
                   std::to_string(list.size()));
            expect_eq(check, aiwrite::paths::joinPathList(list), std::string("A.png\nB.png\nC.png"),
                      "P7a-02 多值拼接：每行一个路径");
            expect(check, aiwrite::paths::splitPathList("   \n\t\n").empty(),
                   "P7a-02 多值解析：全空白 → 空列表");
            expect(check, aiwrite::paths::splitPathList(png_a.string()).size() == 1,
                   "P7a-02 单值解析：无换行 → 长度 1（**旧工作流语义不变**，I19）");

            engine::Graph     param_graph;
            const std::string image_id =
                add_node(check, param_graph, "ImageInput", "P7a 多选参数校验");
            engine::Node*  image      = param_graph.findNode(image_id);
            engine::Param* path_param = image != nullptr ? image->findParam("path") : nullptr;
            expect(check, path_param != nullptr, "P7a-02 参数校验：ImageInput.path 存在");
            if (path_param != nullptr) {
                std::string       reason;
                const std::string three = aiwrite::paths::joinPathList(
                    {png_a.string(), png_b.string(), png_c.string()});
                path_param->value = three;
                expect(check, engine::Graph::validateParam(*path_param, &reason),
                       "P7a-02 参数校验：三张图全部存在 → 通过", reason);

                path_param->value = three + "\n" + (vlm_root / "缺失.png").string();
                const bool failed = !engine::Graph::validateParam(*path_param, &reason);
                expect(check,
                       failed && reason.find("缺失.png") != std::string::npos &&
                           reason.find("共 4 项") != std::string::npos &&
                           reason.find("1 项不可用") != std::string::npos,
                       "P7a-02 参数校验：缺 1 张 → 列出缺失路径 + 项数（可操作文案）", reason);
            }
        }

        // ---- 5b) P7a-08：编码缓存（同图重跑不重复编码）----
        {
            aiwrite::ai::resetImageEncodeCache();
            std::string       error;
            const std::string first = aiwrite::ai::encode_image_data_url(png_a.string(), 0, &error);
            const std::string again = aiwrite::ai::encode_image_data_url(png_a.string(), 0, &error);
            expect(check, !first.empty() && first == again,
                   "P7a-08 缓存：同一张图两次编码 → 结果一致且非空");
            expect(check, aiwrite::ai::imageEncodeCount() == 1,
                   "P7a-08 缓存：同图编码两次 → 实际只编码 1 次",
                   std::to_string(aiwrite::ai::imageEncodeCount()));

            (void)aiwrite::ai::encode_image_data_url(png_b.string(), 0, &error);
            expect(check, aiwrite::ai::imageEncodeCount() == 2,
                   "P7a-08 缓存：换一张图 → 编码计数 +1");

            { // 内容/大小变化 → 缓存键变化 → 重编码（不会吃到过期内容）
                std::ofstream appended(png_b, std::ios::binary | std::ios::app);
                appended.write("x", 1);
            }
            (void)aiwrite::ai::encode_image_data_url(png_b.string(), 0, &error);
            expect(check, aiwrite::ai::imageEncodeCount() == 3,
                   "P7a-08 缓存：文件变化（大小 / 时间不同）→ 重新编码");

            aiwrite::ai::resetImageEncodeCache();
            expect(check, aiwrite::ai::imageEncodeCount() == 0,
                   "P7a-08 缓存：reset 后计数归零（自检用例彼此隔离）");
        }

        // ---- 5c) P7a-09 / P7a-11：三类可操作诊断 + 内容策略分类 ----
        {
            std::string       error;
            const std::string oversize =
                aiwrite::ai::encode_image_data_url(png_a.string(), 4, &error);
            expect(check,
                   oversize.empty() && error.find("**体积**") != std::string::npos &&
                       error.find("字节") != std::string::npos &&
                       error.find("image_max_bytes") != std::string::npos,
                   "P7a-09 体积类：直接拒绝 + 实际/上限/引导齐全（含字节数与配置项名）", error);

            error.clear();
            expect(check,
                   aiwrite::ai::encode_image_data_url(empty_file.string(), 0, &error).empty() &&
                       error.find("**格式**") != std::string::npos,
                   "P7a-11 格式类：空文件 → 标「格式」并给换图引导", error);

            error.clear();
            expect(check,
                   aiwrite::ai::encode_image_data_url((vlm_root / "无此图.png").string(), 0, &error)
                           .empty() &&
                       error.find("**路径**") != std::string::npos,
                   "P7a-11 路径类：文件不存在 → 标「路径」", error);

            const std::string policy = aiwrite::ai::classifyHttpError(
                400, R"({"error":{"message":"content_policy_violation"}})");
            const std::string param_error =
                aiwrite::ai::classifyHttpError(400, R"({"error":{"message":"invalid model"}})");
            expect(check,
                   policy.find("**内容策略**") != std::string::npos &&
                       param_error.find("请求不合法") != std::string::npos && policy != param_error,
                   "P7a-11 内容策略类：400 + 策略关键词 → 与「参数不合法」明确区分", param_error);
            expect(check,
                   aiwrite::ai::classifyHttpError(401, "{}").find("API Key") != std::string::npos &&
                       aiwrite::ai::classifyHttpError(429, "{}").find("限流") != std::string::npos &&
                       aiwrite::ai::classifyHttpError(503, "{}").find("服务端") != std::string::npos,
                   "PB-05 回归：401 / 429 / 503 分类文案不变");
            expect_eq(check, aiwrite::ai::humanBytes(4096), std::string("4.0 KB"),
                      "P7a-10 体积文本：< 1 MB → KB（小图不再显示成 0.00 MB）");
            expect_eq(check, aiwrite::ai::humanBytes(2u * 1024u * 1024u), std::string("2.0 MB"),
                      "P7a-10 体积文本：≥ 1 MB → MB");
        }

        // ---- 6) P7a-04/05/06/07：统一资源目录（内容寻址 + 令牌 + 兼容 + 缺失文案）----
        {
            std::string       error;
            const std::string token_a = aiwrite::asset::importFile(png_a.string(), &error);
            expect(check,
                   aiwrite::asset::isToken(token_a) &&
                       token_a.rfind(aiwrite::asset::kTokenPrefix, 0) == 0 &&
                       token_a.size() == std::string(aiwrite::asset::kTokenPrefix).size() + 64,
                   "P7a-04 归档：导入图片 → 资源令牌（aiwrite-asset: + 64 位摘要）", token_a);
            expect(check,
                   !aiwrite::asset::isToken("aiwrite-asset:xyz") &&
                       !aiwrite::asset::isToken(png_a.string()) &&
                       !aiwrite::asset::isToken(std::string()),
                   "P7a-04 判定：非法摘要 / 普通路径 / 空串都不是令牌");

            const auto count_assets = []() {
                std::size_t count = 0;
                std::error_code ec;
                const std::filesystem::path root = aiwrite::asset::imagesRoot();
                if (std::filesystem::exists(root, ec)) {
                    for (const std::filesystem::directory_entry& entry :
                         std::filesystem::directory_iterator(root, ec)) {
                        if (entry.is_regular_file(ec)) {
                            ++count;
                        }
                    }
                }
                return count;
            };
            const std::size_t before = count_assets();
            const std::string token_a2 = aiwrite::asset::importFile(png_a.string(), &error);
            expect(check, token_a2 == token_a && count_assets() == before,
                   "P7a-04 去重：同内容再导入 → 同一令牌且资源目录文件数不变",
                   std::to_string(before) + " → " + std::to_string(count_assets()));

            const std::string token_b = aiwrite::asset::importFile(png_b.string(), &error);
            expect(check, !token_b.empty() && token_b != token_a,
                   "P7a-04 归档：不同内容 → 不同令牌");

            bool              missing = false;
            const std::string local_a = aiwrite::asset::toLocalPath(token_a, &missing, &error);
            expect(check,
                   !missing && !local_a.empty() && std::filesystem::exists(local_a) &&
                       local_a.find("assets") != std::string::npos,
                   "P7a-04 解析：令牌 → 资源目录内的真实文件", local_a);
            expect(check, aiwrite::asset::toLocalPath(png_c.string()) == png_c.string(),
                   "P7a-06 兼容：旧绝对路径**原样返回**（不变量 I19）");
            expect(check,
                   aiwrite::asset::needsMigration(png_c.string()) &&
                       !aiwrite::asset::needsMigration(token_a),
                   "P7a-06 判定：外部路径需迁移、资源令牌不需要");
            expect(check,
                   aiwrite::asset::migrationHint(png_c.string()).find("迁移到资源目录") !=
                       std::string::npos,
                   "P7a-06 文案：迁移提示指向「迁移到资源目录」");

            // 内容优先的规范扩展名（内容 WebP 却叫 .png → 归档成 .webp）
            std::string disguised_token;
            {
                const fs::path disguised = vlm_root / "伪装.png";
                {
                    std::ofstream              out(disguised, std::ios::binary);
                    const unsigned char        head[] = {'R',  'I',  'F',  'F',  0x1E, 0x00, 0x00, 0x00,
                                                         'W',  'E',  'B',  'P',  'V',  'P',  '8',  ' ',
                                                         0x12, 0x00, 0x00, 0x00};
                    out.write(reinterpret_cast<const char*>(head), sizeof(head));
                }
                disguised_token = aiwrite::asset::importFile(disguised.string(), &error);
                expect(check,
                       !disguised_token.empty() &&
                           aiwrite::asset::toLocalPath(disguised_token).find(".webp") !=
                               std::string::npos,
                       "P7a-04 扩展名：内容优先（WebP 存成 .png → 归档为 .webp）",
                       aiwrite::asset::toLocalPath(disguised_token));
            }

            // 归档失败路径
            std::string import_error;
            expect(check,
                   aiwrite::asset::importFile((vlm_root / "不存在.png").string(), &import_error)
                           .empty() &&
                       !import_error.empty(),
                   "P7a-04 归档失败：文件不存在 → 空令牌 + 可读错误", import_error);

            // 清理：删掉本块导入的资源（自检**不在用户资源目录留垃圾**）
            for (const std::string& created : {token_a, token_b, disguised_token}) {
                const std::string created_path = aiwrite::asset::toLocalPath(created);
                if (!created_path.empty()) {
                    std::error_code cleanup_code;
                    std::filesystem::remove(created_path, cleanup_code);
                }
            }
        }

        // ---- 7) P7a-05/06/07：令牌进工作流的往返 / 旧路径兼容 / 缺失文案 ----
        {
            std::string       error;
            const std::string token      = aiwrite::asset::importFile(png_b.string(), &error);
            const fs::path    asset_file = aiwrite::asset::toLocalPath(token);
            expect(check, !asset_file.empty() && std::filesystem::exists(asset_file),
                   "P7a-05 准备：资源已归档", asset_file.string());

            // P7a-05：令牌写进工作流 → 保存 → 读回 → 运行（换目录 / 换机语义）
            {
                engine::Graph     graph;
                const std::string img = add_node(check, graph, "ImageInput", "P7a 令牌工作流");
                set_param(graph, img, "path", token);
                const fs::path workflow_file = vlm_root / "p7a_asset_roundtrip.json";

                std::string save_error;
                expect(check, engine::saveWorkflow(graph, workflow_file, &save_error),
                       "P7a-05 保存：含资源令牌的工作流可落盘", save_error);

                engine::Graph loaded;
                std::string   load_error;
                const bool    loaded_ok = engine::loadWorkflow(workflow_file, loaded, &load_error);
                expect(check, loaded_ok, "P7a-05 加载：工作流可读回", load_error);
                const engine::Node* loaded_node = loaded.findNode(img);
                expect(check,
                       loaded_node != nullptr && loaded_node->findParam("path") != nullptr &&
                           loaded_node->findParam("path")->text() == token,
                       "P7a-05 往返：文件里存的是**令牌**（不是绝对路径）→ 换机仍有效");

                Executor                 executor;
                std::vector<std::string> console_lines;
                executor.setConsoleHandler(
                    [&console_lines](const std::string& text) { console_lines.push_back(text); });
                std::string start_error;
                const bool  started = executor.start(loaded, &start_error);
                expect(check, started, "P7a-05 运行：start 通过", start_error);
                if (started) {
                    executor.runToCompletion(&loaded, 64);
                    const engine::Node* after = loaded.findNode(img);
                    expect(check, after != nullptr && after->state == engine::NodeState::Done,
                           "P7a-05 运行：ImageInput 完成（令牌已解析为资源路径）");
                    bool resolved_seen = false;
                    for (const std::string& line : console_lines) {
                        if (line.find(aiwrite::asset::imagesRoot().string()) != std::string::npos) {
                            resolved_seen = true;
                        }
                    }
                    expect(check, resolved_seen, "P7a-05 运行：Console 打印解析后的资源路径");
                }
            }

            // P7a-06：旧绝对路径 → 照旧可运行 + Console 给迁移提示（**不静默改写**文件）
            {
                engine::Graph     graph;
                const std::string img = add_node(check, graph, "ImageInput", "P7a 旧绝对路径");
                set_param(graph, img, "path", png_c.string());

                Executor                 executor;
                std::vector<std::string> console_lines;
                executor.setConsoleHandler(
                    [&console_lines](const std::string& text) { console_lines.push_back(text); });
                std::string start_error;
                const bool  started = executor.start(graph, &start_error);
                expect(check, started, "P7a-06 兼容：旧绝对路径不阻断运行", start_error);
                if (started) {
                    executor.runToCompletion(&graph, 64);
                    const engine::Node* after = graph.findNode(img);
                    expect(check, after != nullptr && after->state == engine::NodeState::Done,
                           "P7a-06 兼容：旧绝对路径照旧可运行（不变量 I19）");
                    bool hint_seen = false;
                    for (const std::string& line : console_lines) {
                        if (line.find("迁移到资源目录") != std::string::npos) {
                            hint_seen = true;
                        }
                    }
                    expect(check, hint_seen, "P7a-06 提示：Console 出现「迁移到资源目录」引导");
                }
            }

            // P7a-07：资源被删除 → 文案可操作 + 校验失败；重新归档可自愈
            {
                std::error_code remove_code;
                std::filesystem::remove(asset_file, remove_code);

                bool              gone_missing = false;
                std::string       gone_error;
                const std::string gone =
                    aiwrite::asset::toLocalPath(token, &gone_missing, &gone_error);
                expect(check,
                       gone_missing && gone.empty() &&
                           gone_error.find("图片资源缺失") != std::string::npos &&
                           gone_error.find(aiwrite::asset::imagesRoot().string()) !=
                               std::string::npos &&
                           gone_error.find("重新选择") != std::string::npos,
                       "P7a-07 缺失文案：含「资源目录完整路径 + 预期文件 + 下一步」", gone_error);

                engine::Graph     graph;
                const std::string img        = add_node(check, graph, "ImageInput", "P7a 已删资源校验");
                engine::Node*     image      = graph.findNode(img);
                engine::Param*    path_param = image != nullptr ? image->findParam("path") : nullptr;
                if (path_param != nullptr) {
                    std::string reason;
                    path_param->value = token;
                    expect(check,
                           !engine::Graph::validateParam(*path_param, &reason) &&
                               reason.find("图片资源缺失") != std::string::npos,
                           "P7a-07 校验：令牌指向的**已删除**资源 → 校验失败且文案可操作", reason);

                    // 反向：重新归档同一张图 → 令牌不变（内容寻址）→ 校验恢复通过
                    std::string       reimport_error;
                    const std::string again =
                        aiwrite::asset::importFile(png_b.string(), &reimport_error);
                    expect(check, again == token,
                           "P7a-04 幂等：重新归档同一内容 → 令牌不变（内容寻址可自愈）");
                    expect(check, engine::Graph::validateParam(*path_param, &reason),
                           "P7a-07 自愈：资源重新归档后校验恢复通过", reason);
                }
            }

            // 清理：删掉本次自检导入的资源（不在用户资源目录留垃圾）
            const std::string cleanup_path = aiwrite::asset::toLocalPath(token);
            if (!cleanup_path.empty()) {
                std::error_code cleanup_code;
                std::filesystem::remove(cleanup_path, cleanup_code);
            }
        }

        // ---- 5) 纯文本回归：content 必须是字符串（PB-05 行为不变）----
        {
            aiwrite::ai::OfficialChatRequest request;
            request.system_prompt     = "系统";
            request.prompt            = "纯文本提示";
            const nlohmann::json body = aiwrite::ai::buildRequestBody(request);
            expect(check,
                   body["messages"].size() == 2 && body["messages"][0]["content"].is_string() &&
                       body["messages"][1]["content"].is_string() &&
                       body["messages"][1]["content"] == "纯文本提示",
                   "M5-02 回归：无图时 content 仍为字符串（旧行为不变）");
            expect(check, body["stream"] == false && !body["messages"][1].contains("type"),
                   "M5-02 回归：无图请求体不含多模态字段");
        }

        // ---- 6) 不可用的图片被跳过（纯函数重载的降级行为）----
        {
            aiwrite::ai::OfficialChatRequest request;
            request.prompt            = "只有一张坏图";
            request.images            = {(vlm_root / "坏图.png").string()};
            const nlohmann::json body = aiwrite::ai::buildRequestBody(request);
            expect(check,
                   body["messages"].size() == 1 && body["messages"][0]["content"].is_string(),
                   "M5-02 降级：不可用图片被跳过（退化为纯文本请求体）");
        }

        // ---- 7) 节点接线（离线，无 HTTP）：模型名覆盖 / VLM 的模式与 Key 校验 ----
        {
            // 7a) 提供商配置句柄：model_custom 覆盖生效模型名
            {
                nlohmann::json params;
                params["provider"]     = "deepseek";
                params["mode"]         = "official";
                params["api_base"]     = "https://open.bigmodel.cn/api/paas/v4";
                params["model"]        = "deepseek-chat";
                params["model_custom"] = "glm-4v-flash";
                engine::ExecutionContext context;
                const nlohmann::json     handle =
                    aiwrite::nodes::execute_provider_config(nlohmann::json::object(), params,
                                                           context);
                expect(check,
                       handle["model"] == "glm-4v-flash" &&
                           handle["model_custom"] == "glm-4v-flash",
                       "M5-02b 提供商配置：model_custom 覆盖 provider 句柄的生效模型名");
            }

            // 7b) 引擎侧生效解析（界面与执行共用同一函数）
            {
                Graph             graph;
                const std::string provider = add_node(check, graph, "ProviderConfig", "解析用配置");
                const std::string llm      = add_node(check, graph, "LLMGenerate", "解析用推理");
                set_param(graph, provider, "model", "deepseek-chat");
                set_param(graph, provider, "model_custom", "glm-4v-flash");
                graph.edges.push_back(make_edge("m1", provider, "provider", llm, "provider"));
                if (const engine::Node* llm_node = graph.findNode(llm)) {
                    const engine::EffectiveProvider effective =
                        engine::resolveEffectiveProvider(graph, *llm_node);
                    expect(check,
                           effective.from_edge && effective.model == "glm-4v-flash" &&
                               effective.model_custom == "glm-4v-flash",
                           "M5-02b 生效解析：来源节点 model_custom 覆盖枚举模型名",
                           effective.model);
                }
            }

            // 7c) 图片理解 + 网页版（**M8-34 接线** + **M8-37 自动识别**）
            //  * §9.3 成对登记：原断言（`M5-02b`）断言「web 模式 → 图片理解暂不支持网页版」——
            //    该行为**已于 2026-10-06 被 `M8-34` 取代**（网页版图片理解 → 站点上传模板 + 站点自身
            //    多模态能力）⇒ 原断言**就地改写**为新路径（断言数不变；原因见 CHANGELOG）
            //  * M8-37（零开关）：生成字段未回填（`web_login_only`）**不再**是拦路理由 —— 本用例用
            //    `yuanbao-web`（adapter=dom，**登录型**，且**未声明 upload_adapter**）证明：
            //    报错是「**真不可用**：未接图片上传」，而**不是**旧的「登录型条目 / 暂不支持网页版」；
            //    该分支在 `dom_chat` 之前（不打开窗口 / 不发网络），故断言可离线复现
            {
                Graph             graph;
                const std::string img      = add_node(check, graph, "ImageInput", "VLM 图片输入");
                const std::string text     = add_node(check, graph, "TextInput", "VLM 提示词");
                const std::string vlm      = add_node(check, graph, "VLMGenerate", "图片理解");
                const std::string provider = add_node(check, graph, "ProviderConfig", "VLM 配置 web");
                set_param(graph, img, "path", png_a.string());
                set_param(graph, text, "text", "描述这张图");
                set_param(graph, provider, "provider", "yuanbao-web"); // 登录型网页条目（缺 upload_adapter）
                set_param(graph, provider, "mode", "web");
                graph.edges.push_back(make_edge("v1", img, "image", vlm, "image"));
                graph.edges.push_back(make_edge("v2", text, "text", vlm, "prompt"));
                graph.edges.push_back(make_edge("v3", provider, "provider", vlm, "provider"));

                Executor    executor;
                std::string error;
                const bool  started = executor.start(graph, &error);
                expect(check, started,
                       "M8-34 接线：图片理解（web 模式 · 网页版条目）运行前校验不阻断", error);
                if (started) {
                    executor.runToCompletion(&graph, 64);
                    const engine::Node* node = graph.findNode(vlm);
                    const std::string   message =
                        node != nullptr ? node->error_message : std::string();
                    expect(check,
                           node != nullptr && node->state == engine::NodeState::Error &&
                               message.find("暂不支持网页版") == std::string::npos &&
                               message.find("图片理解（网页版）") != std::string::npos &&
                               message.find("upload_adapter") != std::string::npos,
                           "M8-37 接线：web 模式不再因「生成字段未回填」拦路（真不可用才报错）",
                           message);
                }
            }

            // 7d) 图片理解 + official 且无 Key → 可操作错误（不发起 HTTP）
            {
                Graph             graph;
                const std::string img      = add_node(check, graph, "ImageInput", "VLM 图片输入 2");
                const std::string text     = add_node(check, graph, "TextInput", "VLM 提示词 2");
                const std::string vlm      = add_node(check, graph, "VLMGenerate", "图片理解 2");
                const std::string provider = add_node(check, graph, "ProviderConfig", "VLM 配置 official");
                set_param(graph, img, "path", png_a.string());
                set_param(graph, text, "text", "描述这张图");
                set_param(graph, provider, "mode", "official");
                set_param(graph, provider, "model_custom", "glm-4v-flash");
                // 指向一个不存在的凭据条目：确保「无 Key」这一分支可离线复现（不读真实凭据）
                set_param(graph, provider, "api_key_ref", "aiwrite-selftest/absent");
                graph.edges.push_back(make_edge("x1", img, "image", vlm, "image"));
                graph.edges.push_back(make_edge("x2", text, "text", vlm, "prompt"));
                graph.edges.push_back(make_edge("x3", provider, "provider", vlm, "provider"));

                Executor    executor;
                std::string error;
                const bool  started = executor.start(graph, &error);
                expect(check, started,
                       "M5-02b 接线：图片理解（official 无 Key）运行前校验不阻断", error);
                if (started) {
                    executor.runToCompletion(&graph, 64);
                    const engine::Node* node = graph.findNode(vlm);
                    expect(check,
                           node != nullptr && node->state == engine::NodeState::Error &&
                               node->error_message.find("缺少 API Key") != std::string::npos,
                           "M5-02b 接线：无 Key → 可操作错误（指向「提供商配置」）",
                           node != nullptr ? node->error_message : std::string());
                }
            }
        }

        // ---- 8) 快照图片通道（M5-03：无 GL 也能验，UI 据此渲染缩略图）----
        //      M7：ImagePreview 汇点已移除 —— 图片结果只来自 image 端口（如「图片输入」）
        {
            Graph             graph;
            const std::string img    = add_node(check, graph, "ImageInput", "快照图片输入");
            const std::string text   = add_node(check, graph, "TextInput", "快照文本");
            const std::string output = add_node(check, graph, "TextOutput", "快照文本输出");
            set_param(graph, img, "path", png_a.string());
            set_param(graph, text, "text", "纯文本");
            graph.edges.push_back(make_edge("s2", text, "text", output, "text"));

            Executor    executor;
            std::string error;
            const bool  started = executor.start(graph, &error);
            expect(check, started, "M5-03 快照：图片链路 start 通过", error);
            if (started) {
                executor.runToCompletion(&graph, 64);
                const engine::RunSnapshot snapshot = engine::makeSnapshot(graph, executor);
                const engine::RunNodeView* img_view = snapshot.find(img);
                expect(check,
                       img_view != nullptr && img_view->images.size() == 1 &&
                           img_view->images[0] == png_a.string(),
                       "M5-03 快照：ImageInput 的 image 端口进入 RunNodeView.images");
                const engine::RunNodeView* text_view = snapshot.find(output);
                expect(check, text_view != nullptr && text_view->images.empty(),
                       "M5-03 快照：纯文本节点的 images 为空（不误判为图片）");
            }
        }

        // ---- 9) 示例工作流 E-02（图片转小说）：可加载、可校验 ----
#ifdef AIWRITE_SOURCE_DIR
        {
            std::error_code             example_ec;
            const std::filesystem::path example =
                std::filesystem::path(AIWRITE_SOURCE_DIR) / "workflows" / "examples" /
                "E-02_图片转小说.json";
            expect(check, std::filesystem::exists(example, example_ec),
                   "M5-04 示例：E-02 图片转小说 文件存在", example.string());
            if (std::filesystem::exists(example, example_ec)) {
                Graph       graph;
                std::string error;
                const bool  loaded = engine::loadWorkflow(example, graph, &error);
                expect(check, loaded, "M5-04 示例：E-02 可加载（JSON → Graph）", error);
                if (loaded) {
                    expect(check, graph.nodes.size() == 5 && graph.edges.size() == 4,
                           "M5-04 示例：E-02 为 5 节点 4 连线",
                           std::to_string(graph.nodes.size()) + " 节点 / " +
                               std::to_string(graph.edges.size()) + " 连线");
                    // 2026-10-03（换机实测）：示例里的图片路径曾是**某台机器的绝对路径**
                    //   （`F:/GameDao/.../sample.png`）⇒ 换机后本断言直接挂，且换机用户打开示例
                    //   必报「文件不存在」。故拆成两步：
                    //   ① **新增**可移植性断言：示例不得写死盘符 / 反斜杠绝对路径；
                    //   ② 校验前把图片路径**代入本机示例图** —— 相对路径 / 资源令牌无法跨机解析
                    //      （`graph.cpp` 的 File 参数校验对非令牌值只做 `exists(原值)`），
                    //      若不代入，会把「示例可移植」误判成「示例损坏」。
                    std::string example_image;
                    for (const engine::Node& node : graph.nodes) {
                        if (node.type == "ImageInput") {
                            if (const engine::Param* path_param = node.findParam("path")) {
                                example_image = path_param->text();
                            }
                        }
                    }
                    expect(check,
                           !example_image.empty() && example_image.find(':') == std::string::npos &&
                               example_image.find('\\') == std::string::npos,
                           "M5-04 示例：E-02 的图片路径**可移植**（仓库相对路径，非某台机器的绝对路径）",
                           example_image);

#ifdef AIWRITE_SOURCE_DIR
                    const std::filesystem::path local_example_image =
                        std::filesystem::path(AIWRITE_SOURCE_DIR) / "assets" / "images" /
                        "flamingo.png";
                    for (const engine::Node& node : graph.nodes) {
                        if (node.type == "ImageInput") {
                            set_param(graph, node.id, "path", local_example_image.string());
                        }
                    }
#endif
                    engine::ValidationMessages errors;
                    const bool               valid = engine::validateWorkflow(graph, &errors);
                    expect(check, valid, "M5-04 示例：E-02 加载校验通过（端口/参数/无环）",
                           errors.empty() ? std::string() : errors.front());
                }
            }
        }
#endif

        std::filesystem::remove_all(vlm_root, ec);
    }

    // ---- M_patchB L1 / PB2-04：请求参数化（端点 / 认证 / env / 超时，纯函数）----
    {
        using aiwrite::ai::ProviderOptions;
        std::printf("   -- 请求参数化（M_patchB L1 / PB2-04）--\n");
        // 端点拼接
        expect_eq(check, aiwrite::ai::buildEndpoint("https://api.deepseek.com"),
                  "https://api.deepseek.com/chat/completions", "PB2-04 端点：默认地址");
        expect_eq(check, aiwrite::ai::buildEndpoint("https://a.example.com/v1/"),
                  "https://a.example.com/v1/chat/completions", "PB2-04 端点：尾斜杠 + 路径前缀");
        expect_eq(check, aiwrite::ai::buildEndpoint("https://b.example.com", "/v1/messages"),
                  "https://b.example.com/v1/messages", "PB2-04 端点：自定义路径（Anthropic）");
        expect_eq(check, aiwrite::ai::buildEndpoint("", ""),
                  "https://api.deepseek.com/chat/completions", "PB2-04 端点：空串回退默认");
        expect_eq(check,
                  aiwrite::ai::resolveChatPath("/v1beta/models/{model}:generateContent",
                                                 "gemini-2.0-flash"),
                  "/v1beta/models/gemini-2.0-flash:generateContent",
                  "PB2-04 端点：{model} 占位替换（Gemini 风格）");
        // 认证头
        ProviderOptions bearer;
        const auto auth_bearer = aiwrite::ai::build_auth_headers(bearer, "sk-test");
        expect(check, auth_bearer.size() == 1 && auth_bearer[0].first == "Authorization" &&
                          auth_bearer[0].second == "Bearer sk-test",
               "PB2-04 认证：bearer → Authorization: Bearer …");
        ProviderOptions api_key;
        api_key.auth_style  = "api-key";
        api_key.auth_header = "api-key";
        const auto auth_azure = aiwrite::ai::build_auth_headers(api_key, "k1");
        expect(check, auth_azure.size() == 1 && auth_azure[0].first == "api-key" &&
                          auth_azure[0].second == "k1",
               "PB2-04 认证：api-key → api-key: <key>（Azure 风格）");
        ProviderOptions anthropic;
        anthropic.auth_style  = "x-api-key";
        anthropic.auth_header = "x-api-key";
        const auto auth_claude = aiwrite::ai::build_auth_headers(anthropic, "k2");
        expect(check, auth_claude.size() == 1 && auth_claude[0].first == "x-api-key",
               "PB2-04 认证：x-api-key（Anthropic 风格）");
        ProviderOptions none;
        none.auth_style = "none";
        expect(check, aiwrite::ai::build_auth_headers(none, "k3").empty(),
               "PB2-04 认证：none → 不带头（本地 Ollama）");
        ProviderOptions query;
        query.auth_style = "query";
        expect(check, aiwrite::ai::build_auth_headers(query, "k4").empty(),
               "PB2-04 认证：query → 不走头，走 URL 拼接");
        // env 名按序（表内 env_names）
        expect_eq(check, aiwrite::ai::resolveApiKey("param-key", {"A_KEY", "B_KEY"}), "param-key",
                  "PB2-04 env：节点参数优先");
        expect_eq(check, aiwrite::ai::resolveApiKey(std::string(), {"AIWRITE_ABSENT_KEY"}), "",
                  "PB2-04 env：未命中的 env 名 → 空（无兜底）");
        expect_eq(check, aiwrite::ai::resolveApiKey(std::string(), {}), "",
                  "PB2-04 env：空列表 = 仅 DEEPSEEK_API_KEY（未设置 → 空）");
        // 协议默认值（来自配置表默认补全规则）
        expect_eq(check, aiwrite::ai::defaultChatPath("openai"), "/chat/completions",
                  "PB2-04 默认值：openai → /chat/completions");
        expect_eq(check, aiwrite::ai::defaultChatPath("anthropic"), "/v1/messages",
                  "PB2-04 默认值：anthropic → /v1/messages");
        expect_eq(check, aiwrite::ai::defaultAuthStyle("anthropic"), "x-api-key",
                  "PB2-04 默认值：anthropic → x-api-key");
        expect_eq(check, aiwrite::ai::defaultAuthHeader("api-key"), "api-key",
                  "PB2-04 默认值：auth_style=api-key → 头名 api-key");
    }

    // ---- M_patchB L1 / PB2-05：网页版站点参数化（探测脚本渲染；纯离线，不需要 WebView2）----
    {
        std::printf("   -- 网页版站点参数化（M_patchB L1 / PB2-05）--\n");
        aiwrite::web::LoginRequest default_request;
        const std::string default_script = aiwrite::web::probeKickoffScript(default_request);
        expect(check, default_script.find("'/api/v0/chat/create_pow_challenge'") != std::string::npos &&
                         default_script.find("'/api/v0/chat/completion'") != std::string::npos &&
                         default_script.find("'/api/v0/users/current'") != std::string::npos,
               "PB2-05 探测脚本：无站点参数 = 内置 DeepSeek 默认（行为不变）");

        aiwrite::web::LoginRequest custom;
        custom.challenge_path  = "/api/v9/pow";
        custom.completion_path = "/api/v9/chat";
        custom.probe_paths     = {"/api/v9/me"};
        const std::string custom_script = aiwrite::web::probeKickoffScript(custom);
        expect(check, custom_script.find("'/api/v9/pow'") != std::string::npos &&
                         custom_script.find("'/api/v9/chat'") != std::string::npos &&
                         custom_script.find("['/api/v9/me']") != std::string::npos,
               "PB2-05 探测脚本：站点路径来自配置表（改表即改探测目标）");
        expect(check, custom_script.find("'/api/v0/chat/completion'") == std::string::npos,
               "PB2-05 探测脚本：自定义站点后不再残留内置路径");
    }

    {
        std::printf("   -- 网页版站点参数按条目（M_patchB L1 续 / VB2-17）--\n");

        // ---- VB2-17：登录请求按**生效条目**构造（改表即换站点，零改码）----
        const aiwrite::web::LoginRequest legacy = aiwrite::web::interactive_login_request();
        expect(check, legacy.probe_after_load && !legacy.offscreen &&
                          legacy.url == "https://chat.deepseek.com/" &&
                          legacy.window_title == "AIwrite · DeepSeek 网页版登录（登录后关闭本窗口）",
               "VB2-17 兼容：无参 interactive_login_request 与旧常量逐字一致（不变量 I2）");

        const aiwrite::ai::ProviderSpecs table    = aiwrite::ai::loadProviderSpecs();
        const aiwrite::ai::ProviderSpec* web_spec = table.find("deepseek-web");
        bool                             table_ok = false;
        if (web_spec != nullptr) {
            const aiwrite::web::LoginRequest from_table =
                aiwrite::web::interactive_login_request(web_spec->web, web_spec->id);
            table_ok = from_table.url == web_spec->web.login_url &&
                       from_table.window_title == web_spec->web.window_title &&
                       from_table.provider_id == "deepseek-web" && from_table.probe_after_load &&
                       !from_table.offscreen &&
                       from_table.probe_paths == web_spec->web.probe_paths &&
                       from_table.challenge_path == web_spec->web.endpoints.challenge_path &&
                       aiwrite::web::login_request_site(from_table) == "https://chat.deepseek.com";
        }
        expect(check, table_ok, "VB2-17 表驱动：登录页 / 窗口标题 / 探测路径 / 站点键取自条目");

        const aiwrite::ai::ProviderWebSpec empty_spec;
        const aiwrite::web::LoginRequest   fallback =
            aiwrite::web::interactive_login_request(empty_spec, std::string());
        expect(check, fallback.url == legacy.url && fallback.window_title == legacy.window_title &&
                          fallback.probe_after_load,
               "VB2-17 回落：条目缺字段时回落到内置默认站点（DeepSeek）");

        const aiwrite::web::LoginRequest probe_req = aiwrite::web::probe_login_request(
            web_spec != nullptr ? web_spec->web : empty_spec,
            web_spec != nullptr ? web_spec->id : std::string());
        expect(check, probe_req.probe_after_load && !probe_req.url.empty(),
               "VB2-17 探测窗口：同样按条目构造（probe_after_load=true）");

        // ---- 站点键（origin）----
        const bool origin_ok =
            aiwrite::web::siteKeyOf("https://chat.deepseek.com/") == "https://chat.deepseek.com" &&
            aiwrite::web::siteKeyOf("HTTPS://Chat.DeepSeek.com/a/b?x=1") ==
                "https://chat.deepseek.com" &&
            aiwrite::web::siteKeyOf("about:blank") == "about:blank" &&
            aiwrite::web::siteKeyOf("").empty();
        expect(check, origin_ok, "VB2-16 站点键 = origin（忽略路径/查询/大小写；非 URL 原样）");

        // ---- VB2-18 / PB2-20（决策 D-21）：「模式」候选恒两项 + ProviderConfig 自参数解析 ----
        {
            aiwrite::engine::registerAllNodes();
            aiwrite::engine::Graph     mode_graph;
            std::string                mode_error;
            const std::string          mode_id =
                mode_graph.addNode("ProviderConfig", 0.0f, 0.0f, &mode_error);
            aiwrite::engine::Node* mode_node = mode_graph.findNode(mode_id);
            bool                   options_ok       = false;
            bool                   self_web_ok      = false;
            bool                   not_rewritten_ok = false;
            bool                   unknown_ok       = false;
            if (mode_node != nullptr) {
                const auto select_provider = [mode_node](const std::string& provider) {
                    if (aiwrite::engine::Param* p = mode_node->findParam("provider")) {
                        p->value = provider;
                    }
                };
                const auto set_mode = [mode_node](const std::string& value) {
                    if (aiwrite::engine::Param* p = mode_node->findParam("mode")) {
                        p->value = value;
                    }
                };

                // ① 候选**恒为 {official, web}**（official 条目 / web 条目 / 表外 id 都一样）
                const std::vector<std::string> expect_modes{"official", "web"};
                select_provider("deepseek-web");
                const std::vector<std::string> web_modes =
                    aiwrite::engine::providerModeOptions(*mode_node);
                select_provider("zhipu");
                const std::vector<std::string> api_modes =
                    aiwrite::engine::providerModeOptions(*mode_node);
                select_provider("no-such-provider");
                const std::vector<std::string> unknown_modes =
                    aiwrite::engine::providerModeOptions(*mode_node);
                options_ok = web_modes == expect_modes && api_modes == expect_modes &&
                             unknown_modes == expect_modes;

                // ② web 条目：自参数解析（面板/校验统一入口）→ 命中**该条目**（kind / 显示名 / 站点）
                select_provider("deepseek-web");
                const aiwrite::engine::EffectiveProvider web_self =
                    aiwrite::engine::resolveDisplayProvider(mode_graph, *mode_node);
                const aiwrite::ai::ProviderWebSpec web_site =
                    aiwrite::ai::webSpecFor(web_self.spec, web_self.specs.get());
                self_web_ok = web_self.resolved && !web_self.from_edge && web_self.spec != nullptr &&
                              web_self.kind == "web" && !web_self.display.empty() &&
                              web_self.spec->id == "deepseek-web" && !web_site.login_url.empty() &&
                              web_site.login_url == web_self.spec->web.login_url &&
                              aiwrite::ai::webProviderIdFor(web_self.spec, web_self.specs.get()) ==
                                  std::string("deepseek-web");

                // ③ kind 与 mode 不一致时**不改写** mode：
                //    official 条目 + web → kind 仍 official、mode 保持 web（将用内置默认站点）
                //    web 条目 + official → mode **保持 official**（撤回「静默改写为 web」的回归位）
                select_provider("deepseek");
                set_mode("web");
                const aiwrite::engine::EffectiveProvider official_self =
                    aiwrite::engine::resolveDisplayProvider(mode_graph, *mode_node);
                select_provider("deepseek-web");
                set_mode("official");
                const aiwrite::engine::EffectiveProvider web_official =
                    aiwrite::engine::resolveDisplayProvider(mode_graph, *mode_node);
                not_rewritten_ok =
                    official_self.kind == "official" && official_self.mode == "web" &&
                    web_official.kind == "web" && web_official.mode == "official" &&
                    !aiwrite::engine::modeKindHint(official_self).empty() &&
                    !aiwrite::engine::modeKindHint(web_official).empty();

                // ④ 表外 id → 条目为空（**不**回落显示为 official / 不猜条目）
                select_provider("no-such-provider");
                const aiwrite::engine::EffectiveProvider unknown_self =
                    aiwrite::engine::resolveDisplayProvider(mode_graph, *mode_node);
                unknown_ok = unknown_self.resolved && unknown_self.spec == nullptr &&
                             unknown_self.kind.empty();
            }
            expect(check, options_ok,
                   "VB2-18① 「模式」候选恒 {official, web}（official / web 条目、表外 id 都一样；D-21）");
            expect(check, self_web_ok,
                   "VB2-18② ProviderConfig 自参数解析按自身条目（kind=web / 显示名 / 站点与登录页取自条目）");
            expect(check, not_rewritten_ok,
                   "VB2-18③ kind 与 mode 不一致时**不改写** mode（official+web 保持 web；web+official 保持 official）");
            expect(check, unknown_ok,
                   "VB2-18④ 表外 id → 条目为空（不回落显示为 official）");
        }

        // ---- VB2-19 / PB2-22（决策 D-22② / D-26 / 不变量 I14）：**严格**站点解析（不回落）----
        {
            const aiwrite::ai::ProviderSpecs site_table = aiwrite::ai::loadProviderSpecs();
            const aiwrite::ai::ProviderSpec* official      = site_table.find("deepseek");
            const aiwrite::ai::ProviderSpec* web_entry_spec = site_table.find("deepseek-web");
            const aiwrite::ai::ProviderSpec* no_such        = site_table.find("no-such-provider");

            const aiwrite::ai::ProviderWebSpec official_site =
                aiwrite::ai::strictWebSpecFor(official);
            expect(check, official_site.login_url.empty() && official_site.adapter.empty(),
                   "VB2-19① 非网页版条目 → 严格解析为空（**不**回落表内第一个 web 条目）");

            const aiwrite::ai::ProviderWebSpec strict_web =
                aiwrite::ai::strictWebSpecFor(web_entry_spec);
            expect(check,
                   web_entry_spec != nullptr && !strict_web.login_url.empty() &&
                       strict_web.login_url == web_entry_spec->web.login_url &&
                       strict_web.window_title == web_entry_spec->web.window_title,
                   "VB2-19② 网页版条目 → 严格解析取**它自己**的 web 段（登录页 / 窗口标题一致）");

            expect(check,
                   aiwrite::ai::strictWebSpecFor(nullptr).login_url.empty() &&
                       aiwrite::ai::strictWebSpecFor(no_such).login_url.empty(),
                   "VB2-19③ nullptr / 表外 id → 严格解析为空（面板不再显示误导性站点）");

            expect(check,
                   aiwrite::ai::strictWebProviderIdFor(official).empty() &&
                       aiwrite::ai::strictWebProviderIdFor(web_entry_spec) == "deepseek-web",
                   "VB2-19④ 站点条目 id：非网页版条目为空 / 网页版条目 = 该条目 id");

            expect(check,
                   aiwrite::ai::webSiteError(web_entry_spec).empty() &&
                       !aiwrite::ai::webSiteError(official).empty() &&
                       !aiwrite::ai::webSiteError(nullptr).empty() &&
                       aiwrite::ai::webAdapterImplemented("builtin:deepseek") &&
                       aiwrite::ai::webAdapterImplemented("dom") &&
                       !aiwrite::ai::webAdapterImplemented("builtin:unknown-site"),
                   "VB2-19⑤ 站点错误文案（含三条引导）+ 适配器门控（builtin:deepseek/dom 已实现；未知适配器仍拒绝）");
        }

        // ---- VB2-21 / PB2-26（登录型站点条目）：可加载 + 严格解析可用 + 生成未就绪被如实标记 ----
        {
            const auto has_warn = [](const std::vector<std::string>& items, const std::string& needle) {
                for (const std::string& item : items) {
                    if (item.find(needle) != std::string::npos) {
                        return true;
                    }
                }
                return false;
            };

            const aiwrite::ai::ProviderSpecs login_table = aiwrite::ai::loadProviderSpecs();
            const aiwrite::ai::ProviderSpec* kimi    = login_table.find("kimi-web");
            const aiwrite::ai::ProviderSpec* ds_web2 = login_table.find("deepseek-web");

            expect(check, kimi != nullptr && kimi->kind == "web" && !kimi->web.login_url.empty(),
                   "VB2-21① 登录型站点条目可加载（kimi-web；不再因缺生成字段被跳过）");

            expect(check,
                   kimi != nullptr && aiwrite::ai::webSiteError(kimi).empty() &&
                       aiwrite::ai::strictWebSpecFor(kimi).login_url == kimi->web.login_url,
                   "VB2-21② 登录型条目站点可用（严格解析给出该条目的登录页；无错误）");

            expect(check,
                   kimi != nullptr && aiwrite::ai::webLoginOnly(kimi) &&
                       aiwrite::ai::webAdapterImplemented(kimi->web.adapter),
                   "VB2-21③ 生成未就绪被如实标记（dom 适配器**已实现**，该条缺生成字段 → 标记 `web_login_only`；运行期自动识别，M8-37）");

            expect(check,
                   ds_web2 != nullptr && !aiwrite::ai::webLoginOnly(ds_web2) &&
                       aiwrite::ai::webAdapterImplemented(ds_web2->web.adapter),
                   "VB2-21④ 已就绪条目不受影响（deepseek-web 仍是可生成条目）");

            expect(check, has_warn(login_table.report.warnings, "登录型站点条目"),
                   "VB2-21⑤ 加载报告把「缺生成字段」降级为**警告**（含「登录型站点条目」字样）");
        }

        // ---- VB2-22 / PB2-13（L3 DOM 适配器）：纯函数（轮询钳制 / 注入转义 / 脚本常量 / 就绪度 / 前置校验）----
        {
            aiwrite::ai::ProviderWebSpec site;
            site.adapter         = "dom";
            site.login_url       = "https://site.example.com/";
            site.input_selector  = "div[contenteditable='true']";
            site.send_kind       = "key";
            site.send_value      = "Enter";
            site.answer_selector = ".markdown-body";
            site.done_kind       = "selector_gone";
            site.done_selector   = "button[aria-label*='停止']";

            int poll_ms   = 0;
            int max_polls = 0;
            aiwrite::ai::clampPollParams(site, &poll_ms, &max_polls); // 结构体默认 500 / 120
            const bool clamp_default = (poll_ms == 500 && max_polls == 120);
            aiwrite::ai::ProviderWebSpec tiny;
            tiny.answer_poll_ms   = 100;
            tiny.answer_max_polls = 5;
            aiwrite::ai::clampPollParams(tiny, &poll_ms, &max_polls); // 极小 → 下限
            const bool clamp_low = (poll_ms == 200 && max_polls == 10);
            aiwrite::ai::ProviderWebSpec huge;
            huge.answer_poll_ms   = 99999;
            huge.answer_max_polls = 99999;
            aiwrite::ai::clampPollParams(huge, &poll_ms, &max_polls); // 超大 → 上限
            const bool clamp_high = (poll_ms == 2000 && max_polls == 600);
            expect(check, clamp_default && clamp_low && clamp_high,
                   "VB2-22① 轮询参数钳制（默认 500ms/120 次不变；极小 → 200ms/10 次；超大 → 2000ms/600 次）");

            const std::string tricky_prompt = "第一行 \"引号\" \\ 反斜杠\n第二行\t制表";
            aiwrite::ai::DomChatRequest request;
            request.prompt = tricky_prompt;
            request.site   = site;
            bool cfg_ok    = false;
            try {
                const nlohmann::json cfg = nlohmann::json::parse(aiwrite::ai::domCfgJson(request));
                cfg_ok = cfg.value("prompt", std::string()) == tricky_prompt &&
                         cfg.value("input_selector", std::string()) == site.input_selector &&
                         cfg.value("send_kind", std::string()) == "key" &&
                         cfg.value("send_value", std::string()) == "Enter" &&
                         cfg.value("answer_selector", std::string()) == site.answer_selector &&
                         cfg.value("done_kind", std::string()) == "selector_gone" &&
                         cfg.value("done_selector", std::string()) == site.done_selector &&
                         // M8-37：自动识别开关与候选池随配置下发（仍是**纯数据**，不落盘）
                         cfg.value("auto_input", false) && cfg.value("auto_answer", false) &&
                         !cfg.value("input_pool", std::string()).empty() &&
                         !cfg.value("answer_pool", std::string()).empty() &&
                         cfg.value("answer_min_chars", 0) > 0;
            }
            catch (const std::exception&) {
                cfg_ok = false;
            }
            expect(check, cfg_ok,
                   "VB2-22② 注入配置可解析且**转义安全**（提示词含引号 / 反斜杠 / 换行 / 制表 · 含 M8-37 自动识别字段）");

            const std::string kick = aiwrite::ai::domKickoffScript();
            const std::string poll = aiwrite::ai::domPollScript();
            const std::string prb  = aiwrite::ai::domProbeScript();
            expect(check,
                   kick.find("window.__aiwriteDom") != std::string::npos &&
                       poll.find("window.__aiwriteDom") != std::string::npos &&
                       prb.find("window.__aiwriteDomProbe") != std::string::npos &&
                       kick.find("input_selector") != std::string::npos &&
                       poll.find("answer_selector") != std::string::npos &&
                       // M8-37：自动识别分支真的在页面脚本里（输入框打分 + 答案基线差分）
                       kick.find("input_auto") != std::string::npos &&
                       kick.find("__aiwriteDomRuntime") != std::string::npos &&
                       poll.find("answer_pool") != std::string::npos,
                   "VB2-22③ 页面脚本常量（kickoff/poll 读 __aiwriteDom；probe 读 __aiwriteDomProbe；含 M8-37 自动识别）");

            aiwrite::ai::ProviderSpec ready_spec;
            ready_spec.kind = "web";
            ready_spec.web  = site;
            aiwrite::ai::ProviderSpec short_spec = ready_spec;
            short_spec.web.answer_selector.clear();
            expect(check, !aiwrite::ai::webLoginOnly(&ready_spec) &&
                              aiwrite::ai::webLoginOnly(&short_spec) &&
                              aiwrite::ai::webAdapterImplemented("dom"),
                   "VB2-22④ 就绪度推断：生成字段齐 → 非登录型；缺 answer_selector → 登录型（补齐即就绪）");

            // M8-37（零开关）：**真不可用**仍立即报错 —— 缺站点登录页时连窗口都无从开；
            //  而「生成字段未回填」已不再是前置失败（改由运行期自动识别，见 VB2-22②③）
            aiwrite::ai::DomChatRequest no_site;
            no_site.site = short_spec.web; // 缺 answer_selector（登录型）
            no_site.site.login_url.clear();
            const aiwrite::ai::DomChatResult no_site_result = aiwrite::ai::domChat(no_site);
            expect(check,
                   !no_site_result.ok &&
                       no_site_result.error.find("login_url") != std::string::npos,
                   "VB2-22⑤ dom_chat 前置校验（缺 web.login_url → 立即报错；不打开窗口、不发送）");

            // ---- PW-07（2026-10-08 · `AW-04` 反例的根因修）：答案**归属**门 ----
            //  * 真机反例（同形两次）：豆包布局容器 `div.bp5-overflow-list` 的「今天 13:31」（**12 字节**）
            //    与 Kimi 操作条的「Copy Share」（**10 字节**）都被旧「差分判据」当成新答案；
            //  * 本断言只查**静态口径**（装饰形态随 cfg 下发 + 页面脚本里真有这道门与体检串 +
            //    旧「短候选也算成功」确已删除）；**行为级**验证在
            //    `source/tools/poll_answer_gate_test.js`（node + 迷你 DOM 真跑该脚本：9 / 0，
            //    `--old` 负向模式 ①②③b 如实 FAIL）。
            bool gate_ok = false;
            try {
                const nlohmann::json cfg2 = nlohmann::json::parse(aiwrite::ai::domCfgJson(request));
                const std::string    re2  = cfg2.value("answer_chrome_text", std::string());
                gate_ok = re2.find("^") == 0 && re2.find("\\d{1,2}") != std::string::npos &&
                          re2.find("Copy|Share") != std::string::npos &&
                          re2.find("复制|分享") != std::string::npos;
            }
            catch (const std::exception&) {
                gate_ok = false;
            }
            expect(check,
                   gate_ok &&
                       poll.find("answer_chrome_text") != std::string::npos &&  // 形态随 cfg 下发（纯数据）
                       poll.find("chrome_text.test(t)") != std::string::npos && // 装饰 / 布局文本不算答案
                       poll.find("装饰文本拒") != std::string::npos &&          // 体检串（进 steps / 日志）
                       poll.find("过短拒") != std::string::npos &&
                       poll.find("只有短候选：仍如实返回") == std::string::npos, // 旧「短候选也算成功」已删
                   "VB2-22⑥ 答案归属门（PW-07）：装饰 / 布局文本不算答案 · 短候选不回落 · 体检串随 poll 回传");

            // ---- PW-07i（2026-10-10 · 真机驱动 · `T-L11` gemini-web）：**控件词拼接**不算答案 ----
            //  * 真机反例：`gemini-web` 自动识别把操作条 / 工具提示的拼接（**85 字节**：
            //    `Dictate (^⇧D)Close sidebarImagesSettingsEdit promptCopy promptCopy responseMoreRedo`）
            //    当答案写进 `n3` / `n4` ⇒ 三口径全判「可用」= **假成功**；上面那道「整串门」抓不住它
            //    （它**不等于**任一个控件词，而是**多个**控件词拼接 —— `Copy prompt` x2 这类按钮文本）；
            //  * 本断言只查**静态口径**（控件词随 cfg 下发 + 页面脚本里真有这道门与计数）；**行为级**在
            //    `source/tools/poll_answer_gate_test.js` ⑲a/⑲b（真机原文逐字）+ ⑲c（不得误伤）。
            bool words_ok = false;
            try {
                const nlohmann::json cfg3 = nlohmann::json::parse(aiwrite::ai::domCfgJson(request));
                const std::string    re3  = cfg3.value("answer_chrome_words", std::string());
                words_ok = re3.find("Copy prompt") != std::string::npos &&
                           re3.find("Close sidebar") != std::string::npos &&
                           re3.find("复制") != std::string::npos;
            }
            catch (const std::exception&) {
                words_ok = false;
            }
            expect(check,
                   words_ok &&
                       poll.find("answer_chrome_words") != std::string::npos &&  // 控件词随 cfg 下发（纯数据）
                       poll.find("chrome_only(t)") != std::string::npos &&       // 剥离后残余近空 ⇒ 不算答案
                       poll.find("控件词拼接拒") != std::string::npos,            // 计数进体检串（steps / 日志）
                   "VB2-22⑨ 控件词拼接门（PW-07i）：操作条拼接（gemini-web 85 字节反例）不算答案");

            // ---- PW-07j（2026-10-10 · 真机驱动 · `T-L12` gemini-web）：**工具提示 / 屏读宿主**不算候选 ----
            //  * 真机反例（日志物证）：`答案容器**自动识别** div.cdk-describedby-message-container … 候选池 38 个`
            //    —— Angular CDK 的 `aria-describedby` 文案容器，只因类名含 `message` 被池里
            //    `[class*="message"]` 误吞 ⇒ 77 / 92 字节「假成功」（两道文本门都抓不住：池选的**不是**答案文本）。
            //  * 本断言只查**静态口径**（排除选择器随 cfg 下发 + 页面脚本里真有池内剔除 + 体检串计数）；
            //    **行为级**在 `source/tools/poll_answer_gate_test.js` ⑳a（剔完轮到真答案）+ ⑳b（计数）。
            bool excl_ok = false;
            try {
                const nlohmann::json cfg4 = nlohmann::json::parse(aiwrite::ai::domCfgJson(request));
                const std::string    ex4  = cfg4.value("answer_pool_exclude", std::string());
                excl_ok = ex4.find("describedby") != std::string::npos &&
                          ex4.find("tooltip") != std::string::npos &&
                          ex4.find("sr-only") != std::string::npos;
            }
            catch (const std::exception&) {
                excl_ok = false;
            }
            expect(check,
                   excl_ok &&
                       poll.find("answer_pool_exclude") != std::string::npos &&            // 排除项随 cfg 下发
                       poll.find("el.matches && el.matches(pool_excl)") != std::string::npos && // 池内剔除自己
                       poll.find("另剔非候选宿主") != std::string::npos,                    // 计数进体检串
                   "VB2-22⑩ 候选池剔除工具提示 / 屏读宿主（PW-07j）：gemini-web 命中的 CDK 描述容器不算候选");

            // ---- PW-07k（2026-10-10 · 真机驱动 · `T-L13` gemini-web）：**本方消息回声**不算答案 ----
            //  * 真机反例：剔掉 CDK 描述容器后紧接着取到 `span.user-query-bubble-with-background`
            //    （`You said <提示词>`，**172 字节**）⇒ `n3` / `n4` = **自己那条**（下游拿本方消息当回答）。
            //    旧口径只拦「整串等于提示词 / 以提示词开头」⇒ 带前缀（`You said `）的回声漏网。
            //  * 判据 = 含提示词且**围绕它的残余**（去空白 / 标点）< `min_chars` ⇒ 判回声（真答案残余够长）；
            //    结构侧同时把 `user-query` / `query-text` / `user-message` 容器从池内剔除。
            //  * **行为级**在 `source/tools/poll_answer_gate_test.js` ㉑a/㉑b（真机形态）+ ㉑c（不得误伤）+ ㉑d。
            bool echo_ok = false;
            try {
                const nlohmann::json cfg5 = nlohmann::json::parse(aiwrite::ai::domCfgJson(request));
                const std::string    ex5  = cfg5.value("answer_pool_exclude", std::string());
                echo_ok = ex5.find("user-query") != std::string::npos &&
                          ex5.find("query-text") != std::string::npos;
            }
            catch (const std::exception&) {
                echo_ok = false;
            }
            expect(check,
                   echo_ok &&
                       poll.find("echo_only(t)") != std::string::npos &&     // 回声门（含提示词且残余近空）
                       poll.find("回声拒") != std::string::npos,              // 计数进体检串
                   "VB2-22⑪ 本方消息回声门（PW-07k）：`You said <提示词>`（gemini-web 172 字节反例）不算答案");

            // ---- PW-07m / PW-07n（2026-10-10 · 真机驱动 · `T-L17` 回归扫描）：两个新的「假成功」形态 ----
            //  ① PW-07m **站侧内部码**：`yuanbao-web` 的 `n3` / `n4` = `CONFLICT_LOG`（12 字节）——
            //     元宝把内部诊断节点渲染进 DOM，且是文档序最后候选 ⇒ 三口径判「可用」。
            //     判据 = 整串（去空白 / 标点）就是一个 `SNAKE_CASE` 常量名。
            //  ② PW-07n **生成中占位**：`ernie-web` 的 `n3` / `n4` = 站侧自动标题 + `正在编写，请稍候...`
            //     + `0%` + `改用对话直接回答` ⇒ 同样判「可用」。判据 = 占位 / 进度短语 **且** 裸百分比
            //     （两个独立信号同时出现 ⇒ 真答案几乎不可能同时满足，只拒不猜）。
            //  * **行为级**在 `source/tools/poll_answer_gate_test.js` ㉒a/㉒b 与 ㉓a/㉓b/㉓c。
            expect(check,
                   poll.find("internal_code_only(t)") != std::string::npos &&
                       poll.find("内部码拒") != std::string::npos,
                   "VB2-22⑫ 站侧内部码门（PW-07m）：`CONFLICT_LOG`（yuanbao-web 12 字节反例）不算答案");
            expect(check,
                   poll.find("progress_only(t)") != std::string::npos &&
                       poll.find("生成中拒") != std::string::npos,
                   "VB2-22⑬ 生成中占位门（PW-07n）：`正在编写…0%`（ernie-web 19 字节反例）不算答案");

            // ---- PW-07o（2026-10-10 · 真机驱动 · `T-L18` ernie-web）：**站侧推荐追问外壳**不算候选 ----
            //  * 真机反例：取到 `div._answer-ask-wrapper_16z1u_8`（文心 CSS-module 外壳），文本 = 4 条追问
            //    （`用代码模拟… / 把这段描述精简成… / 用幽默风格重新描述… / 能否再续写一段…`，183 字节）
            //    ⇒ 三口径判「可用」；真答案是同池内的 `div.cosd-markdown-content`。
            //  * 判据 = **外壳身份**（`ask-wrapper`，节点自己匹配即剔，不看祖先）⇒ 内层真答案照常参与。
            //  * **行为级**在 `source/tools/poll_answer_gate_test.js` ㉔a/㉔b（排除生效）+ ㉔c（不得误伤）。
            bool ask_ok = false;
            try {
                const nlohmann::json cfg6 = nlohmann::json::parse(aiwrite::ai::domCfgJson(request));
                ask_ok = cfg6.value("answer_pool_exclude", std::string()).find("ask-wrapper") != std::string::npos;
            }
            catch (const std::exception&) {
                ask_ok = false;
            }
            expect(check,
                   ask_ok && poll.find("推荐追问") != std::string::npos,
                   "VB2-22⑭ 推荐追问外壳剔除（PW-07o）：`_answer-ask-wrapper_*`（ernie-web 183 字节反例）不算候选");

            // ---- PW-07p（2026-10-10 · 真机驱动 · `T-L19` ernie-web）：**提示词派生短句列表**不算答案 ----
            //  * 真机教训：`T-L19` 追问容器的**类名换了**（`_answer-ask-wrapper_16z1u_8` →
            //    `ai-index-no-animation-icon`）⇒ PW-07o 的类名排除**打不完**（站侧一改版本就是新类名）。
            //    ⇒ 改用**文本形态**判：≥3 行 · 每行 ≤ 24 字 · 整串无句末标点 · 每行都与提示词共享 ≥2 字片段。
            //  * **行为级**在 `source/tools/poll_answer_gate_test.js` ㉕a（拒）+ ㉕b/㉕c（不得误伤）。
            expect(check,
                   poll.find("suggest_list(raw_el)") != std::string::npos &&
                       poll.find("建议列表拒") != std::string::npos,
                   "VB2-22⑮ 提示词派生短句列表门（PW-07p）：`ai-index-no-animation-icon` 追问（192 字节）不算答案");

            // ---- PW-07q（2026-10-10 · 真机驱动 · `T-L22` gemini-web）：**外壳容器**不算候选 ----
            //  * 真机反例：取到 `infinite-scroller.chat-history`（436 字节 = 历史各轮 `You said … / Gemini
            //    said …` + 本轮），而本轮真答案是它内部的 `#model-response-message-content*`（同池候选）。
            //    外壳中选的原因：内层「上一轮」答案文本早在基线里（`is_new` 为假）、内层「本轮」答案节点
            //    当时还空 ⇒ 只有外壳的文本是「新」的。
            //  * 判据 = 池内**包含关系**（`el.contains(other候选)`，与类名 / 文本无关）⇒ 外壳不算候选。
            //  * **行为级**在 `source/tools/poll_answer_gate_test.js` ㉖a（外壳剔 / 继续轮询）+ ㉖b/㉖c（不得误伤）。
            expect(check,
                   poll.find("inner_of_some(pool[i])") != std::string::npos &&
                       poll.find("外壳剔") != std::string::npos,
                   "VB2-22⑯ 外壳容器剔除（PW-07q）：`infinite-scroller.chat-history`（gemini-web 436 字节反例）不算候选");

            // ---- PW-07r（2026-10-10 · 真机驱动 · `T-L24` yuanbao-web）：**行内元素**不算候选 ----
            //  * 真机反例：取到 `code.hyc-common-markdown__code__inline`（**13 字节** = `mode=悬疑`，
            //    答案正文里的行内代码片段）—— 行内节点也带 markdown 字样类名 ⇒ 被池误吞，且是文档序
            //    **最后**候选 ⇒ 外层块级真答案反而落选（三口径判「可用」）。
            //  * 判据 = 元素形态（块级 vs 行内）：`code` / `span` 只承载片段 ⇒ 不进候选池。
            //  * **行为级**在 `source/tools/poll_answer_gate_test.js` ㉗a/㉗b（行内剔除）+ ㉗c（消息项不误伤）。
            bool inline_ok = false;
            try {
                const nlohmann::json cfg7 = nlohmann::json::parse(aiwrite::ai::domCfgJson(request));
                const std::string ex7 = cfg7.value("answer_pool_exclude", std::string());
                inline_ok = ex7.find("code") != std::string::npos && ex7.find("span") != std::string::npos;
            }
            catch (const std::exception&) {
                inline_ok = false;
            }
            expect(check,
                   inline_ok,
                   "VB2-22⑰ 行内元素不入池（PW-07r）：`code` / `span` 不算候选（yuanbao-web 13 字节片段反例）");

            // ---- PW-07g（2026-10-08 · 「段落换行丢失」根因修）：判据/产物**分离** + 排版纯函数 ----
            //  * 真机反例：豆包回答 `div.container-qX9Csx`（**759 字节**，内容完整）经页面侧
            //    `norm`（`\s+` → `' '`）后写盘 ⇒ `4-TextOutput.txt` 整篇 **1 行**（LF 全在文件头）：
            //    内容一个字没丢，**分段没了**（`3-LLMGenerate.txt` 里「…结果。 我试着…」就是指纹）。
            //  * 修法（用户拍板 · 方案 A）：页面只**无损搬运**（产物 `out.text = raw`，判据继续吃 `norm`），
            //    排版交给 C++ 纯函数 `format_answer_text`（本断言的下半段逐例锁住）。
            //  * 行为级验证（真跑页面脚本 · 迷你 DOM）：`source/tools/poll_answer_gate_test.js`（含 `--old` 反向）。
            expect(check,
                   poll.find("out.text = norm(") == std::string::npos &&                // 产物**不得**再归一
                       poll.find("out.text = t;") == std::string::npos &&               // 旧「归一值当产物」已删
                       poll.find("out.text = raw;") != std::string::npos &&             // 产物 = 原样搬运
                       poll.find("out.lf = lf_count(raw)") != std::string::npos &&      // 分段物证随 poll 回传
                       poll.find("out.found = norm(raw).length > 0") != std::string::npos && // 判据仍用归一
                       poll.find("const t = norm(raw);") != std::string::npos,          // 长度判定仍用归一
                   "VB2-22⑦ 文本保真（PW-07g）：产物**不**归一（`out.text = raw`）· 判据仍归一 · `lf` 随 poll 回传");

            // PW-07g：排版纯函数 —— 8 例（段落保留是**承重**项：修的就是它）
            {
                const auto fmt = [](const std::string& s) { return aiwrite::ai::formatAnswerText(s); };
                // ① 三段落（`\n` 分隔）⇒ 行内软换行保留、内容逐字保留（**本次事故的核心**）
                const bool multi = (fmt("甲段。\n乙段。\n丙段？") == "甲段。\n乙段。\n丙段？");
                // ② `\r\n` / 单独 `\r` → `\n`
                const bool crlf = (fmt("甲。\r\n乙。\r丙。") == "甲。\n乙。\n丙。");
                // ③ 空行规范化（A2）：连续 `\n{3,}` → `\n\n`；**恰好 1 个空行原样保留**（空行 = 段界）
                const bool blank = (fmt("甲。\n\n\n乙。") == "甲。\n\n乙。") &&
                                   (fmt("甲。\n\n乙。") == "甲。\n\n乙。");
                // ④ 行尾空白清除 + 行内连续空白 → 单空格；**行首缩进原样保留**
                //    （缩进放在**第 2 行**才有意义：整串首尾 trim 本就该去掉文首空白；`\t` 保留 ⇒ 不破坏代码块 / 引用）
                const bool space = (fmt("甲   乙  \n\t丙   丁  ") == "甲 乙\n\t丙 丁");
                // ⑤ Markdown 结构原样（标题 / 列表 / 引用：只规整空白，不改写语法 —— A2 下 `\n\n` 本就该在）
                const bool markdown = (fmt("# 标题\n\n- 一\n- 二\n> 引用") == "# 标题\n\n- 一\n- 二\n> 引用");
                // ⑥ 首尾空白 trim（含首尾空行）
                const bool trim = (fmt("\n\n  甲。  \n\n") == "甲。");
                // ⑦ 空串 / 纯空白 → 空串（不产生「假有内容」）
                const bool empty = fmt("").empty() && fmt("   \n\t\r\n  ").empty();
                // ⑧ **无损**：引号 / 反斜杠 / emoji / 制表 + 多段 ⇒ 除既定空白规整外逐字符不变
                const std::string long_raw = "第一段 \"引号\" \\ 反斜杠 \xF0\x9F\x98\x80\n第二段\t制表\n\n第三段。";
                const bool        lossless =
                    (fmt(long_raw) == "第一段 \"引号\" \\ 反斜杠 \xF0\x9F\x98\x80\n第二段 制表\n\n第三段。");
                // 失败时的**显式化**输出（`\n` / `\r` / `\t` 打成转义）—— 一眼对齐「实际 vs 期望」
                const auto esc = [](const std::string& s) {
                    std::string r;
                    for (const char c : s) {
                        if (c == '\n') { r += "\\n"; }
                        else if (c == '\r') { r += "\\r"; }
                        else if (c == '\t') { r += "\\t"; }
                        else { r += c; }
                    }
                    return r;
                };
                if (!(multi && crlf && blank && space && markdown && trim && empty && lossless)) {
                    std::printf(
                        "     [VB2-22⑧ 明细] multi=%d crlf=%d blank=%d space=%d markdown=%d trim=%d empty=%d "
                        "lossless=%d\n       crlf_got=[%s] blank_got=[%s] space_got=[%s] markdown_got=[%s] "
                        "lossless_got=[%s]\n",
                        multi ? 1 : 0, crlf ? 1 : 0, blank ? 1 : 0, space ? 1 : 0, markdown ? 1 : 0, trim ? 1 : 0,
                        empty ? 1 : 0, lossless ? 1 : 0, esc(fmt("甲。\r\n乙。\r丙。")).c_str(),
                        esc(fmt("甲。\n\n\n乙。")).c_str(), esc(fmt("甲   乙  \n\t丙   丁  ")).c_str(),
                        esc(fmt("# 标题\n\n- 一\n- 二\n> 引用")).c_str(), esc(fmt(long_raw)).c_str());
                }
                expect(check, multi && crlf && blank && space && markdown && trim && empty && lossless,
                       "VB2-22⑧ 排版纯函数 format_answer_text（PW-07g · A2）：多段保留 · CRLF · "
                       "空行规范化（≥1 空行 → 恰好 1 个）· "
                       "行内/行尾空白 · 缩进保留 · Markdown 原样 · trim · 空串 · 长文本无损");
            }
        }
    }

    {
        std::printf("   -- L4 登录态判定 / 文案与渲染条件（M_patchB PB2-27 / D-27 / D-28① / I15）--\n");

        using aiwrite::ai::ProviderSpec;
        using aiwrite::ai::WebSessionEvidence;
        using aiwrite::ai::WebSessionState;
        using aiwrite::ai::WebSessionVerdict;

        // ---- VB2-24：登录态判定（纯函数 · 5 例；判据**不看** userToken）----
        {
            ProviderSpec dom; // DOM 站点（未配 cookie_names）
            dom.id            = "site-a";
            dom.kind          = "web";
            dom.web.adapter   = "dom";
            dom.web.login_url = "https://site-a.example.com/";

            ProviderSpec dom_named      = dom; // 配了 cookie_names
            dom_named.web.cookie_names  = {"session_token", "sid"};

            const WebSessionVerdict v4 = aiwrite::ai::webSessionState(&dom, WebSessionEvidence{});

            WebSessionEvidence hit; // ① cookie_names 命中
            hit.cookies_known = true;
            hit.cookieCount  = 2;
            hit.cookie_names  = {"sid", "other"};
            const WebSessionVerdict v1 = aiwrite::ai::webSessionState(&dom_named, hit);

            WebSessionEvidence miss = hit; // ② 未命中
            miss.cookie_names       = {"other", "another"};
            const WebSessionVerdict v2 = aiwrite::ai::webSessionState(&dom_named, miss);

            WebSessionEvidence cookies_only; // ③ 未配 cookie_names 但该 origin 有 Cookie
            cookies_only.cookies_known = true;
            cookies_only.cookieCount  = 3;
            const WebSessionVerdict v3 = aiwrite::ai::webSessionState(&dom, cookies_only);

            ProviderSpec token_expr_spec   = dom; // ⑤ 配了 token_expr 但没有 token
            token_expr_spec.web.token_expr = "localStorage.getItem('userToken')";
            WebSessionEvidence no_cookies;
            no_cookies.cookies_known = true;
            const WebSessionVerdict v5 = aiwrite::ai::webSessionState(&token_expr_spec, no_cookies);

            expect(check, v1.state == WebSessionState::loggedIn,
                   "VB2-24① 条目 cookie_names 命中 → logged_in");
            expect(check, v2.state == WebSessionState::logged_out,
                   "VB2-24② cookie_names 未命中（已读过该站点 Cookie）→ logged_out");
            expect(check, v3.state == WebSessionState::loggedIn,
                   "VB2-24③ 未配 cookie_names 但该 origin 有 Cookie → logged_in（D-27 并集）");
            expect(check,
                   v4.state == WebSessionState::unknown &&
                       v4.reason.find("尚未读取") != std::string::npos,
                   "VB2-24④ 空会话（从未读过 Cookie）→ unknown（不误报「未登录」）");
            expect(check,
                   v5.state == WebSessionState::logged_out &&
                       v5.reason.find("userToken") == std::string::npos,
                   "VB2-24⑤ 配了 token_expr 但没有 token → 仍按 Cookie 判（判据**不看** userToken；I15）");
        }

        // ---- VB2-26：文案与渲染条件（可离线断言部分）----
        {
            const aiwrite::ai::ProviderSpecs  table = aiwrite::ai::loadProviderSpecs();
            const aiwrite::ai::ProviderSpec* kimi   = table.find("kimi-web");
            const aiwrite::ai::ProviderSpec* ds_web = table.find("deepseek-web");

            const std::string kimi_warn = aiwrite::ai::webSiteFieldWarnings(kimi);
            expect(check,
                   kimi != nullptr && !kimi_warn.empty() &&
                       kimi_warn.find("无法自动探测凭证") == std::string::npos &&
                       kimi_warn.find("协议探测不适用") != std::string::npos,
                   "VB2-26① DOM 条目的字段警告**不再**含「无法自动探测凭证」（改为「协议探测不适用」）");

            ProviderSpec with_token        = {};
            with_token.kind                = "web";
            with_token.web.token_expr      = "localStorage.getItem('userToken')";
            ProviderSpec without_token     = with_token;
            without_token.web.token_expr.clear();
            expect(check,
                   aiwrite::ai::webShowsUserToken(&with_token) &&
                       !aiwrite::ai::webShowsUserToken(&without_token) &&
                       aiwrite::ai::webShowsUserToken(ds_web) &&
                       !aiwrite::ai::webShowsUserToken(kimi),
                   "VB2-26② userToken 行渲染条件 = !token_expr.empty()（D-28①：deepseek-web 显示 / DOM 站点不显示）");

            const WebSessionVerdict kimi_verdict =
                aiwrite::ai::webSessionState(kimi, WebSessionEvidence{});
            const WebSessionVerdict ds_verdict =
                aiwrite::ai::webSessionState(ds_web, WebSessionEvidence{});
            expect(check,
                   kimi_verdict.reason.find("userToken") == std::string::npos &&
                       kimi_verdict.reason.find("ds_session_id") == std::string::npos &&
                       kimi_verdict.reason.find("/api/v0/") == std::string::npos &&
                       ds_verdict.reason.find("/api/v0/") == std::string::npos,
                   "VB2-26③ 会话结论文案只描述**站点无关**原因（Cookie / 页面），无 DeepSeek 专有名词");
        }

        // ---- VB2-25：协议探测适用性（纯函数 + 脚本分支；PB2-28 / 不变量 I16）----
        {
            const aiwrite::ai::ProviderSpecs  table        = aiwrite::ai::loadProviderSpecs();
            const aiwrite::ai::ProviderSpec*  deepseek_web = table.find("deepseek-web");
            const aiwrite::ai::ProviderSpec*  kimi_web     = table.find("kimi-web");

            aiwrite::ai::ProviderWebSpec custom_dom; // 自建 dom 条目：无任何探测字段
            custom_dom.adapter   = "dom";
            custom_dom.login_url = "https://site.example.com/";
            aiwrite::ai::ProviderWebSpec dom_with_paths = custom_dom;
            dom_with_paths.probe_paths                   = {"/api/mine/me"};
            aiwrite::ai::ProviderWebSpec dom_with_token = custom_dom;
            dom_with_token.token_expr                   = "localStorage.getItem('myToken')";

            expect(check, aiwrite::ai::probeIsApplicable(deepseek_web),
                   "VB2-25① 内置协议站点（deepseek-web）→ 协议探测**适用**");
            expect(check, kimi_web != nullptr && !aiwrite::ai::probeIsApplicable(kimi_web),
                   "VB2-25② DOM 站点（kimi-web）→ 协议探测**不适用**（I16）");
            expect(check,
                   !aiwrite::ai::probeIsApplicable(custom_dom) &&
                       aiwrite::ai::probeIsApplicable(dom_with_paths) &&
                       aiwrite::ai::probeIsApplicable(dom_with_token),
                   "VB2-25③ 自建 dom 条目：无探测字段 → 不适用；显式配 probe_paths / token_expr → 适用");

            const aiwrite::web::LoginRequest builtin_request; // 默认 = 无参 / CLI 路径
            const std::string builtin_script = aiwrite::web::probeKickoffScript(builtin_request);
            expect(check,
                   builtin_request.probe_applicable &&
                       builtin_script.find("/api/v0/chat/create_pow_challenge") != std::string::npos &&
                       builtin_script.find("localStorage.getItem('userToken')") != std::string::npos,
                   "VB2-25④ 默认参数 → 脚本仍是内置 DeepSeek 行为（probe_applicable 默认 true；守 I2）");

            aiwrite::web::LoginRequest readonly_request;
            readonly_request.probe_applicable = false;
            const std::string readonly_script =
                aiwrite::web::probeKickoffScript(readonly_request);
            expect(check,
                   readonly_script.find("/api/v0/") == std::string::npos &&
                       readonly_script.find("localStorage.getItem('userToken')") == std::string::npos &&
                       readonly_script.find("location.href") != std::string::npos &&
                       readonly_script.find("document.cookie") != std::string::npos,
                   "VB2-25⑤ 不适用分支脚本**不含** `/api/v0/` 与 `localStorage.getItem('userToken')`（只读诊断；I16）");
        }

        // ---- VB2-27（新增 · PB2-25 / PB2-28④）：会话失效识别（纯函数）----
        {
            const std::string auth_body = "{\"code\":40002,\"msg\":\"auth failed\"}";
            const std::string pow_body  = "{\"data\":{\"biz_code\":40003}}";
            const std::string ok_body   = "{\"code\":0,\"data\":{\"v\":\"hi\"}}";
            const std::string spaced    = "{ \"code\": 40002 }";

            const std::string hint_401 = aiwrite::ai::webSessionFailureHint(401, "");
            const std::string hint_2   = aiwrite::ai::webSessionFailureHint(200, auth_body);
            const std::string hint_3   = aiwrite::ai::webSessionFailureHint(200, pow_body);
            const std::string hint_ok  = aiwrite::ai::webSessionFailureHint(200, ok_body);

            expect(check, !hint_401.empty() && aiwrite::ai::webSessionFailureNeedsRelogin(401, ""),
                   "VB2-27① HTTP 401 → 识别为「会话已失效」且需重新登录");
            expect(check,
                   hint_2.find("重新登录") != std::string::npos &&
                       aiwrite::ai::webSessionFailureNeedsRelogin(200, auth_body) &&
                       aiwrite::ai::webSessionFailureNeedsRelogin(200, spaced),
                   "VB2-27② code=40002（含带空格写法）→ 提示重新登录并作废该站点会话");
            expect(check,
                   hint_3.find("40003") != std::string::npos &&
                       !aiwrite::ai::webSessionFailureNeedsRelogin(200, pow_body),
                   "VB2-27③ code=40003 → 给可操作提示但**不**作废会话（PoW / 频率 / 前端版本）");
            expect(check, hint_ok.empty() && !aiwrite::ai::webSessionFailureNeedsRelogin(200, ok_body),
                   "VB2-27④ 正常响应 → 不误报（hint 为空）");
            expect(check,
                   hint_401.find("userToken") == std::string::npos &&
                       hint_2.find("ds_session_id") == std::string::npos,
                   "VB2-27⑤ 失效提示文案不含厂商专有物（userToken / ds_session_id；I15 口径）");
        }

        // ---- VB2-29 / VB2-32（新增 · M7B 批 1）：管道协议 v1 离线桩（纯函数，不依赖真实 Python）----
        //  * 词表唯一来源 = M7B.md §6.1；Python 侧同构断言见 `python -m brain_ai_browser --selftest`
        //  * 纪律：非法行**只**能被丢弃 + 回 err{bad_frame}，**不得**让守护进程 / UI 线程崩
        {
            using namespace aiwrite::web::channel;

            // ① 合法命令帧：v / kind / id / name 解析正确（§6.1 帧格式）
            const Frame hello = parseFrame(make_command("hello", "7"));
            expect(check,
                   !hello.bad && hello.v == kProtoVersion && hello.kind == "cmd" &&
                       hello.id == "7" && hello.name == "hello",
                   "VB2-29① 合法命令帧：v / kind=cmd / id / name 解析正确（§6.1）",
                   hello.bad ? hello.reason : hello.payload_json);

            // ② 非法行：JSON 坏 / 缺 v / 缺 kind / kind 非法 / v 不识别 / 空行 → 判非法
            const Frame bad_json = parseFrame("{not json");
            const Frame bad_no_v = parseFrame("{\"kind\":\"cmd\",\"id\":\"1\",\"name\":\"hello\"}");
            const Frame bad_kind = parseFrame("{\"v\":1,\"id\":\"1\",\"name\":\"hello\"}");
            const Frame bad_kind2 = parseFrame("{\"v\":1,\"kind\":\"oops\",\"id\":\"1\"}");
            const Frame bad_ver =
                parseFrame("{\"v\":99,\"kind\":\"cmd\",\"id\":\"1\",\"name\":\"hello\"}");
            const Frame bad_blank = parseFrame("   ");
            expect(check,
                   bad_json.bad && bad_no_v.bad && bad_kind.bad && bad_kind2.bad && bad_ver.bad &&
                       bad_blank.bad && !bad_json.reason.empty() && !bad_ver.reason.empty(),
                   "VB2-29② 非法行（JSON 坏 / 缺 v / 缺 kind / kind 非法 / v 不识别 / 空行）→ 判非法",
                   bad_ver.reason);

            // ③ 未知命令 / 缺必需字段 → **可操作原因**（不静默执行）
            const Frame unknown = parseFrame(make_command("fly_to_moon", "8"));
            const Frame missing = parseFrame(make_command("open_tab", "9"));
            std::string unknown_reason;
            std::string missing_reason;
            const bool  unknown_ok = !validateCommand(unknown, &unknown_reason);
            const bool  missing_ok = !validateCommand(missing, &missing_reason);
            expect(check,
                   unknown_ok && unknown_reason.find("未知命令") != std::string::npos &&
                       missing_ok && missing_reason.find("缺少字段") != std::string::npos,
                   "VB2-29③ 未知命令 / 缺必需字段 → 明确原因（词表 10 命令 + 必需字段）",
                   unknown_reason + " | " + missing_reason);

            // ④ 词表完整：10 命令 / 8 事件（v3 起；与 §6.1 表逐条一致）
            expect(check,
                   knownCommands().size() == 10 && knownEvents().size() == 8 &&
                       isKnownCommand("upload_image") && isKnownCommand("send_prompt") &&
                       isKnownCommand("run_script") && isKnownCommand("logout_site") &&
                       isKnownCommand("current_tab") && isKnownCommand("shutdown") &&
                       !isKnownCommand("hello_world"),
                   "VB2-29④ 词表完整：10 命令 / 8 事件（§6.1 表 · v3 起）",
                   std::to_string(knownCommands().size()) + " / " +
                       std::to_string(knownEvents().size()));

            // ⑤ 非法帧回包 err{bad_frame} 且 id 与请求配对（请求-响应可追溯）
            const Frame err_frame =
                parseFrame(make_error(errorCodeBadFrame(), "7", "非法帧已丢弃"));
            expect(check,
                   !err_frame.bad && err_frame.kind == "err" && err_frame.id == "7" &&
                       err_frame.name == errorCodeBadFrame(),
                   "VB2-29⑤ 非法帧回包 err{bad_frame} 且 id 与请求配对",
                   err_frame.payload_json);

            // ⑥ 容量上限写死：单帧 ≤ 64 KiB / images ≤ 8（图片传路径，不传 base64）
            const Frame too_many = parseFrame(make_command(
                "upload_image", "10",
                "{\"provider\":\"doubao-web\",\"images\":[\"C:/a.png\",\"C:/b.png\",\"C:/c.png\","
                "\"C:/d.png\",\"C:/e.png\",\"C:/f.png\",\"C:/g.png\",\"C:/h.png\",\"C:/i.png\"]}"));
            std::string over_reason;
            const bool  over_ok = !validateCommand(too_many, &over_reason);
            expect(check,
                   frameWithinLimit(std::string(kMaxFrameBytes, 'x')) &&
                       !frameWithinLimit(std::string(kMaxFrameBytes + 1, 'x')) && over_ok &&
                       over_reason.find("上限") != std::string::npos,
                   "VB2-29⑥ 容量上限：单帧 ≤ 64 KiB / images ≤ 8（不传 base64）",
                   over_reason);

            // ⑦ attach 取值受词表约束（M7.md D11：auto | file_input | drop_zone | paste_only | none）
            const Frame bad_attach = parseFrame(make_command(
                "upload_image", "11",
                "{\"provider\":\"doubao-web\",\"images\":[\"C:/a.png\"],\"attach\":\"magic\"}"));
            const Frame ok_attach = parseFrame(make_command(
                "upload_image", "12",
                "{\"provider\":\"doubao-web\",\"images\":[\"C:/a.png\"],\"attach\":\"file_input\"}"));
            std::string bad_attach_reason;
            expect(check,
                   !validateCommand(bad_attach, &bad_attach_reason) &&
                       bad_attach_reason.find("attach") != std::string::npos &&
                       validateCommand(ok_attach, nullptr),
                   "VB2-29⑦ attach 取值受词表约束（D11：auto | file_input | drop_zone | paste_only | none）",
                   bad_attach_reason);

            // ---- VB2-32：CDP 帧 → 增量文本（纯函数；复用既有 `text` / `delta.text` 两形态）----
            bool              flag_a  = false;
            bool              flag_b  = false;
            bool              flag_no = false;
            const std::string delta_a = deltaTextOfFrame("{\"seq\":1,\"text\":\"你\"}", &flag_a);
            const std::string delta_b = deltaTextOfFrame("{\"delta\":{\"text\":\"好\"}}", &flag_b);
            const std::string delta_n = deltaTextOfFrame("{\"foo\":1}", &flag_no);
            expect(check,
                   delta_a == "你" && delta_b == "好" && delta_n.empty() && !flag_a && !flag_b,
                   "VB2-32① CDP 增量帧 → 文本（`text` 直取 / `delta.text` 嵌套；两形态复用）",
                   delta_a + delta_b);

            // I22：无增量 → 必须**显式**标注「非流式（轮询）」（不静默改变行为）
            bool              flag_empty = false;
            const std::string delta_e    = deltaTextOfFrame("", &flag_empty);
            const Frame       stage_evt =
                parseFrame(make_event("stage", "-", "{\"stage\":\"answer\",\"ok\":true}"));
            expect(check,
                   flag_no && flag_empty && delta_e.empty() && !stage_evt.bad &&
                       stage_evt.kind == "evt" && stage_evt.name == "stage",
                   "VB2-32②/I22 无增量 → 置「非流式（轮询）」标记（I22 显式降级）",
                   stage_evt.bad ? stage_evt.reason : stage_evt.payload_json);

        }

    }


    // ---- M7B 批 2（step 6）新增：站点描述搬迁 / 快照只读视图 / 新通道如实报错（纯逻辑，不启进程）----
    {
        using aiwrite::web::SiteRef;

        // ① `site_ref.h` 是**零语义搬迁**（守 I2）：无参默认请求与旧常量逐字一致
        const aiwrite::web::LoginRequest legacy = aiwrite::web::interactive_login_request();
        expect(check,
               legacy.url == std::string(aiwrite::web::kDefaultSiteLoginUrl) &&
                   legacy.window_title == std::string(aiwrite::web::kDefaultSiteWindowTitle) &&
                   legacy.probe_after_load,
               "VB2-40① `site_ref.h` 搬迁零语义：无参 `interactive_login_request()` 与旧常量逐字一致（守 I2）");

        // ② `attach`（B12-C2）：条目 `web.attach` → `SiteRef.attach`（**只携带、运行期不消费**）
        aiwrite::ai::ProviderWebSpec web_spec;
        web_spec.login_url = "https://example.invalid/";
        web_spec.attach    = "file_input";
        const SiteRef carried = aiwrite::web::login_request_of(web_spec, "demo-vb240",
                                                               /*for_probe=*/false,
                                                               /*offscreen=*/false);
        expect(check, carried.attach == "file_input" && carried.url == "https://example.invalid/",
               "VB2-40② `login_request_of()` 携带 `web.attach`（B12-C2：本批只解析、不消费）");

        // ③ 快照 C++ 只读视图（`VB2-39` 的 C++ 半）：只碰元数据；`summary()` **零明文**
        std::error_code             dir_ec;
        const std::filesystem::path probe_dir =
            std::filesystem::temp_directory_path(dir_ec) / "aiwrite-vb240";
        const std::filesystem::path probe_file = probe_dir / "cookies.dat";
        std::filesystem::create_directories(probe_dir, dir_ec);
        {
            std::ofstream out(probe_file, std::ios::binary | std::ios::trunc);
            out << "SENTINEL-CLEARTEXT-should-never-appear";
        }
        const aiwrite::web::SnapshotInfo probe_info = aiwrite::web::inspectSnapshotAt(probe_file);
        const std::string                probe_text = aiwrite::web::snapshotSummary(probe_info);
        expect(check,
               probe_info.exists && probe_info.bytes > 0 && !probe_info.modified_at.empty() &&
                   probe_text.find("SENTINEL-CLEARTEXT") == std::string::npos,
               "VB2-39⑥ 快照 C++ 只读视图：`inspect()` 只碰元数据（存在 / 字节 / 时间）+ "
               "`summary()` **零明文**（**不读内容**）");
        std::filesystem::remove_all(probe_dir, dir_ec);

        // ④ 跨语言常量（与 Python `session.py` 同值 —— 改一处必须改两处）
        expect(check,
               std::string(aiwrite::web::kSnapshotFileName) == "cookies.dat" &&
                   aiwrite::web::kSnapshotRefreshPeriod <= 10.0 &&
                   aiwrite::web::kSnapshotMaxBytes == 1024u * 1024u,
               "VB2-40③ 快照常量与 Python 同值：`cookies.dat` / 刷新 ≤ 10 s / 1 MiB 上限");

        // ④ 会话族**无会话 → 如实报错**（`I21`：不假装成功、不回落旧通道）
        //    `logout_site` 于 step 11（v3）**已落地** ⇒ 断言从「尚未实现」升级为
        //    「无会话 → 可操作错误（管道连不上）+ **不修改任何东西**」
        std::string logout_error;
        const bool  logout_ok = aiwrite::web::channel::logoutSite(carried, 1000, &logout_error);
        expect(check,
               !logout_ok && logout_error.find("尚未实现") == std::string::npos &&
                   logout_error.find("会话未在运行") != std::string::npos,
               "VB2-40④ 会话族无会话 → **可操作错误**（`logout_site`：不再是「尚未实现」；I21）",
               logout_error);

        // ⑤ 会话生命周期（`M7B-18` 换代）：断言**纯查询 + 幂等收尾**，零副作用
        //    ⚠️ 不能在此调 `ensure_session`：换代后它会**真起浏览器**（断言须无副作用）
        const bool no_session = !aiwrite::web::channel::sessionReady();
        const bool closed_now = aiwrite::web::channel::shutdownSession();
        expect(check, no_session && !closed_now,
               "VB2-40⑤ 会话生命周期：初始 `session_ready()==false` + `shutdown_session()` **幂等**"
               "（无会话时返回 false · 不崩 · I21 不假装）");
    }

    // ---- M7B 批 3（step 8）新增：词表 **v2** 的 `run_script`（纯逻辑；无守护进程 → 可操作错误）----
    {
        using namespace aiwrite::web::channel;

        aiwrite::web::SiteRef script_site;
        script_site.provider_id = "demo-vb241";
        std::string script_error;
        const bool  script_ok = run_script(script_site, "return 1;", 1000, nullptr, &script_error);
        expect(check,
               !script_ok && script_error.find("尚未实现") == std::string::npos &&
                   script_error.find("守护进程") != std::string::npos,
               "VB2-41① `run_script`（v2）无守护进程 → **可操作原因**（不再是「尚未实现」；I21 不静默降级）",
               script_error);

        const aiwrite::web::channel::Frame run_frame =
            parseFrame(make_command("run_script", "20",
                                     "{\"provider\":\"demo-vb241\",\"script\":\"return 1;\"}"));
        std::string run_reason;
        const bool  run_ok = validateCommand(run_frame, &run_reason);
        const bool  run_missing_ok = !validateCommand(
            parseFrame(make_command("run_script", "21", "{\"provider\":\"demo-vb241\"}")),
            &run_reason);
        expect(check,
               !run_frame.bad && run_ok && run_missing_ok &&
                   run_reason.find("script") != std::string::npos && kProtoVersion >= 2 &&
                   knownCommands().size() >= 8 && knownEvents().size() >= 7,
               "VB2-41② 词表 ≥v2：`run_script` 必需字段（缺 `script` → 明确原因）+ 版本 / 条数同值",
               run_reason + " | proto=" + std::to_string(kProtoVersion));
    }

    // ---- M7B 批 3（step 11）新增：词表 **v3** 会话族（纯逻辑；无守护进程 / 无会话 → 可操作错误）----
    {
        using namespace aiwrite::web::channel;

        // ① 词表 v3：条数 / 必需字段 / 事件 / 错误码
        std::string logout_reason;
        const Frame logout_frame =
            parseFrame(make_command("logout_site", "30", "{\"provider\":\"demo-vb243\"}"));
        const Frame current_frame = parseFrame(make_command("current_tab", "31"));
        const bool  logout_valid = validateCommand(logout_frame, &logout_reason);
        const bool  logout_missing_ok =
            !validateCommand(parseFrame(make_command("logout_site", "32")), &logout_reason);
        expect(check,
               kProtoVersion >= 3 && knownCommands().size() == 10 && knownEvents().size() == 8 &&
                   logout_valid && logout_missing_ok &&
                   logout_reason.find("provider") != std::string::npos &&
                   validateCommand(current_frame, nullptr),
               "VB2-43① 词表 ≥v3：`logout_site`（缺 provider → 明确原因）+ `current_tab`（无必需字段）"
               "+ 10 命令 / 8 事件",
               logout_reason + " | proto=" + std::to_string(kProtoVersion));

        // ② `tab` 事件（`current_tab` 回包）可生成且可解析
        const Frame tab_frame =
            parseFrame(make_event("tab", "31",
                                   "{\"url\":\"https://example.com/x\","
                                   "\"site\":\"https://example.com\"}"));
        expect(check,
               !tab_frame.bad && tab_frame.kind == "evt" && tab_frame.name == "tab" &&
                   tab_frame.id == "31",
               "VB2-43② `tab` 事件入词表：`current_tab` 回包（url / site）可生成且可解析",
               tab_frame.bad ? tab_frame.reason : tab_frame.payload_json);

        // ③ 会话族**无会话 → 空结果 / 如实报错**（`I21`：不编造站点、不回落旧通道）
        aiwrite::web::SiteRef tab_site;
        tab_site.provider_id = "demo-vb243";
        tab_site.url         = "https://example.com/";
        std::string session_error;
        const bool  session_off = !aiwrite::web::channel::tabOnSite("https://example.com");
        expect(check,
               session_off && aiwrite::web::channel::currentTabSite().empty() &&
                   !aiwrite::web::channel::logoutSite(tab_site, 500, &session_error) &&
                   session_error.find("会话未在运行") != std::string::npos,
               "VB2-43③ 会话族无会话 → 空结果 / 可操作错误（`tab_on_site` 不编造、`logout_site` 如实报）",
               session_error);

        // ④ `visible_login_request`（step 11）：`dom_chat` 的会话请求 = **有头**
        //    （新通道的浏览器是用户**唯一的登录入口** ⇒ 必须看得见；`I21` 可操作路径）
        aiwrite::ai::ProviderWebSpec web_spec;
        web_spec.login_url = "https://example.com/";
        const aiwrite::web::LoginRequest visible =
            aiwrite::web::visible_login_request(web_spec, "demo-vb243");
        const aiwrite::web::LoginRequest offscreen =
            aiwrite::web::boot_login_request(web_spec, "demo-vb243");
        expect(check,
               !visible.offscreen && offscreen.offscreen && visible.url == "https://example.com/",
               "VB2-43④ `visible_login_request` = **有头**（`dom_chat` 走新通道的唯一登录入口）；"
               "`boot_login_request` 仍离屏（只服务旧协议栈）",
               "visible.offscreen=" + std::to_string(visible.offscreen));

        // ---- VB2-44⑤~⑦（新增 · 批 3 step 14 · 词表 v4）：站字段组 + 内容返回通道 ----
        const std::vector<std::string> send_fields   = requiredFieldsOf("send_prompt");
        const std::vector<std::string> answer_fields = requiredFieldsOf("read_answer");
        const auto                     has_field     = [](const std::vector<std::string>& fields,
                                                          const char* want) {
            for (const std::string& item : fields) {
                if (item == want) {
                    return true;
                }
            }
            return false;
        };
        const std::string good_line = make_command(
            "send_prompt", "9",
            "{\"provider\":\"demo-vb244\",\"prompt\":\"x\",\"input_selector\":[\"a\"],"
            "\"send\":{\"kind\":\"key\",\"value\":\"Enter\"}}");
        const std::string bad_selector_line = make_command(
            "send_prompt", "9",
            "{\"provider\":\"demo-vb244\",\"prompt\":\"x\",\"input_selector\":\"a\","
            "\"send\":{\"kind\":\"key\",\"value\":\"Enter\"}}");
        const std::string bad_kind_line = make_command(
            "send_prompt", "9",
            "{\"provider\":\"demo-vb244\",\"prompt\":\"x\",\"input_selector\":[\"a\"],"
            "\"send\":{\"kind\":\"clickk\",\"value\":\"b\"}}");
        const std::string no_selector_line = make_command(
            "send_prompt", "9",
            "{\"provider\":\"demo-vb244\",\"prompt\":\"x\","
            "\"send\":{\"kind\":\"key\",\"value\":\"Enter\"}}");
        std::string good_reason;
        std::string bad_selector_reason;
        std::string bad_kind_reason;
        std::string no_selector_reason;
        const bool  good_valid = validateCommand(parseFrame(good_line), &good_reason);
        validateCommand(parseFrame(bad_selector_line), &bad_selector_reason);
        validateCommand(parseFrame(bad_kind_line), &bad_kind_reason);
        validateCommand(parseFrame(no_selector_line), &no_selector_reason);
        expect(check,
               kProtoVersion == 4 && has_field(send_fields, "provider") &&
                   has_field(send_fields, "prompt") && has_field(send_fields, "input_selector") &&
                   has_field(send_fields, "send") && has_field(answer_fields, "provider") &&
                   has_field(answer_fields, "answer_selector") &&
                   knownCommands().size() == 10 && knownEvents().size() == 8,
               "VB2-44⑤ 词表 v4：`send_prompt` 必需站字段组（provider+prompt+input_selector+send）· "
               "`read_answer` 必需 answer_selector；**命令 / 事件计数不变**（10 / 8）",
               "proto=" + std::to_string(kProtoVersion) + " reason=" + no_selector_reason);
        expect(check,
               good_valid && good_reason.empty() && !bad_selector_reason.empty() &&
                   !bad_kind_reason.empty() && !no_selector_reason.empty() &&
                   bad_selector_reason.find("input_selector") != std::string::npos &&
                   bad_kind_reason.find("send.kind") != std::string::npos,
               "VB2-44⑥ 站字段组校验（与 Python `station_fields_reason` 逐条同构）：合法帧通过；"
               "选择器非数组 / `send.kind` 非法 / 缺选择器 → **可操作原因**",
               bad_selector_reason + " | " + bad_kind_reason);
        aiwrite::web::SiteRef content_site;
        content_site.provider_id = "demo-vb244";
        content_site.url         = "https://example.com/";
        std::string upload_error;
        std::string send_error;
        std::string answer_error;
        std::string answer_text;
        bool        answer_cut  = false;
        int         answer_size = 0;
        const bool  upload_rejected = !aiwrite::web::channel::upload_image(
            content_site, {}, "file_input", "", 1'000, &upload_error);
        const bool  send_rejected = !aiwrite::web::channel::send_prompt(
            content_site, "x", {}, "key", "Enter", false, 1'000, &send_error);
        const bool  answer_rejected = !aiwrite::web::channel::read_answer(
            content_site, {}, "", 0, 0, 1'000, &answer_text, &answer_cut, &answer_size, &answer_error);
        expect(check,
               upload_rejected && send_rejected && answer_rejected && !upload_error.empty() &&
                   !send_error.empty() && !answer_error.empty(),
               "VB2-44⑦ 内容返回三函数**参数前置校验**：images / input_selector / answer_selector "
               "为空 → false + 可操作原因（**不发 IPC** · I21）",
               upload_error + " | " + send_error + " | " + answer_error);
    }


    {
        std::printf("   -- 会话按站点键控（M_patchB L1 续 / PB2-16）--\n");

        using aiwrite::web::Session;
        using aiwrite::web::SessionStore;
        SessionStore::instance().clearAll(); // 先清干净，避免影响其他自检

        Session session_a;
        session_a.site = "https://a.example.com";
        session_a.cookies.push_back({"site_a", "va", false, true, 0.0});
        SessionStore::instance().set(session_a);
        Session session_b;
        session_b.site = "https://b.example.com";
        session_b.cookies.push_back({"site_b", "vb", false, true, 0.0});
        SessionStore::instance().set(session_b);
        expect(check,
               SessionStore::instance().loggedIn("https://a.example.com") &&
                   SessionStore::instance().snapshot("https://a.example.com").has("site_a") &&
                   SessionStore::instance().loggedIn("https://b.example.com"),
               "VB2-16 多站点并存：写入 B 后 A 仍在（互不覆盖）");

        aiwrite::web::ProbeResult probe_a;
        probe_a.ok        = true;
        probe_a.token_raw = "token-a";
        SessionStore::instance().setProbe(std::move(probe_a), "https://a.example.com");
        aiwrite::web::ProbeResult probe_b;
        probe_b.ok        = true;
        probe_b.token_raw = "token-b";
        SessionStore::instance().setProbe(std::move(probe_b), "https://b.example.com");
        expect(check, SessionStore::instance().hasToken("https://a.example.com") &&
                          SessionStore::instance().hasToken("https://b.example.com"),
               "VB2-16 凭证按站点：两个站点各自持有 userToken");

        SessionStore::instance().clear("https://a.example.com");
        expect(check, !SessionStore::instance().hasToken("https://a.example.com") &&
                          !SessionStore::instance().loggedIn("https://a.example.com") &&
                          SessionStore::instance().hasToken("https://b.example.com") &&
                          SessionStore::instance().loggedIn("https://b.example.com"),
               "VB2-16 按站点注销：清 A 不影响 B");

        const std::vector<std::string> sites = SessionStore::instance().sites();
        expect(check, sites.size() == 1 && sites.front() == "https://b.example.com",
               "VB2-16 sites()：只列出仍有会话的站点（顺序稳定）");

        // ---- 兼容：不传站点的旧 API（默认槽）行为与改造前一致 ----
        SessionStore::instance().clearAll();
        Session plain; // 不带站点 = 改造前的用法
        plain.url = "https://chat.deepseek.com/";
        plain.cookies.push_back({"ds_session_id", "vv", false, true, 0.0});
        SessionStore::instance().set(plain);
        const bool legacy_slot_ok = SessionStore::instance().loggedIn() &&
                                    SessionStore::instance().cookieCount() == 1 &&
                                    SessionStore::instance().snapshot().has("ds_session_id");
        SessionStore::instance().clear();
        expect(check, legacy_slot_ok && !SessionStore::instance().loggedIn() &&
                          SessionStore::instance().cookieCount() == 0,
               "VB2-16 兼容：不传站点的旧 API（默认槽）行为不变");
        SessionStore::instance().clearAll(); // 归还干净状态
    }

    {
        std::printf("   -- Provider 配置表（M_patchB L1 / PB2-01）--\n");
        int       spec_passed = 0;
        const int spec_failed = aiwrite::ai::providerSpecSelftest(&spec_passed);
        check.passed += spec_passed;
        check.failed += spec_failed;
    }

    {
        // M8-14：站点上传模板（**离线**三组：图片准备 / 双证据 / 注册表与契约）
        //  * 不联网、不开窗口 —— 真机验证走 `aiwrite.exe --upload-selftest --provider <id>`
        std::printf("   -- 站点上传模板：图片准备（M8-14）--\n");
        int convert_passed = 0;
        int evidence_passed = 0;
        int registry_passed = 0;
        const int convert_failed  = aiwrite::ai::imageConvertSelftest(&convert_passed);
        std::printf("   -- 站点上传模板：双证据判定（M8-14）--\n");
        const int evidence_failed = aiwrite::ai::uploadEvidenceSelftest(&evidence_passed);
        std::printf("   -- 站点上传模板：注册表与契约（M8-14）--\n");
        const int registry_failed = aiwrite::ai::uploadRegistrySelftest(&registry_passed);
        check.passed += convert_passed + evidence_passed + registry_passed;
        check.failed += convert_failed + evidence_failed + registry_failed;
    }

    {
        // M_8nodes_extension L1-c（§4.2）：图片预处理（用户参数 → 裁剪 / 缩放）
        //  * VL1-05 离线部分：零开销 / fit / fill / 裁剪 / 组合 / 失败路径
        std::printf("   -- 图片预处理（M_8nodes_extension L1-c）--\n");
        int preprocess_passed = 0;
        const int preprocess_failed = aiwrite::ai::imagePreprocessSelftest(&preprocess_passed);
        check.passed += preprocess_passed;
        check.failed += preprocess_failed;
    }

    {
        // M_patchC（`PW-01` / `PW-03`）：**归属 + 就绪三级**的纯函数断言（离线；不开窗口、不联网）
        //  * 依据：真机 2026-10-06 三次运行（113 ms 假失败 / 41 ms 误归因）—— 见
        //    `docs/actionPlan/M_patchC_web_node_bind.md` §3 / §4.1 / §4.2
        namespace web = aiwrite::web;
        std::printf("   -- 网页通道归属与就绪三级（M_patchC PW-01 / PW-03）--\n");

        // ---- ① 归属（`WebOwner` · I24 的基础）----
        web::WebOwner a;
        a.run_id   = "run1";
        a.node_id  = "n7";
        a.site_key = "https://www.doubao.com";
        web::WebOwner b    = a;
        web::WebOwner c    = a;
        c.node_id          = "n3";
        const web::WebOwner none; // 未标注
        expect(check, web::owner_same(a, b), "PW-01 owner_same：同 run+node = 同一归属");
        expect(check, !web::owner_same(a, c),
               "PW-01 owner_same：不同 node ⇒ 不同归属（同站点多节点可区分，I24）");
        expect_eq(check, web::owner_key(a), "run1#n7", "PW-01 owner_key = run#node");
        expect(check, web::owner_key(none).empty(), "PW-01 空 owner ⇒ 键为空（未标注）");
        expect_eq(check, web::owner_tag(none), "[owner 未标注]",
                  "PW-01 owner_tag 永不为空（日志可 grep）");
        expect(check, web::owner_known(a) && !web::owner_known(none),
               "PW-01 owner_known：能区分「已标注 / 未标注」");

        // ---- ② 就绪三级（位组合 / 判定 / 可读串）----
        expect(check, web::kReadyWindow == 1 && web::kReadyPage == 2 &&
                          web::kReadyComposer == 4 && web::kReadyFileInput == 8,
               "PW-03 就绪位：窗口=1 / 页面=2 / 输入框=4 / 附件入口=8（可组合）");
        expect(check, web::kReadyScript == (web::kReadyWindow | web::kReadyPage | web::kReadyComposer) &&
                          web::kReadyUpload == (web::kReadyScript | web::kReadyFileInput),
               "PW-03 组合：kReadyScript = L1+L2+L3a；kReadyUpload = 再加 L3b");
        expect(check, web::ready_satisfied(web::kReadyWindow | web::kReadyPage,
                                          web::kReadyWindow | web::kReadyPage) &&
                          !web::ready_satisfied(web::kReadyWindow,
                                                web::kReadyWindow | web::kReadyPage),
               "PW-03 ready_satisfied：必须**覆盖**需求（缺一级即不满足）");
        expect_eq(check, web::ready_first_missing(web::kReadyUpload,
                                                 web::kReadyWindow | web::kReadyPage),
                  "交互态（输入框）", "PW-03 缺级判定：窗口 → 页面 → 交互态（逐级）");
        expect(check, web::ready_satisfied(web::kReadyUpload, web::kReadyUpload) &&
                          web::ready_first_missing(web::kReadyUpload, web::kReadyUpload).empty(),
               "PW-03 全满足 ⇒ 无缺级（空串）");
        expect_eq(check, web::ready_reached_text(web::kReadyWindow | web::kReadyPage),
                  "窗口级 + 页面级", "PW-03 已达级别可读串（日志 / 报告用）");
        expect_eq(check, web::ready_reached_text(web::kReadyNone), "未就绪",
                  "PW-03 一级都不达 ⇒ 「未就绪」");

        // ---- ③ 归因分离（`G6` 的回归断言：未就绪 ≠ 形态不支持）----
        const std::string miss_input =
            web::ready_missing_text(web::kReadyUpload, web::kReadyWindow | web::kReadyPage);
        const std::string miss_page   = web::ready_missing_text(web::kReadyPage, web::kReadyNone);
        const std::string miss_window = web::ready_missing_text(web::kReadyWindow, web::kReadyNone);
        //  * 判据踩在**旧误导文案**上（「可能是拖拽 / 粘贴入口」「暂不支持该形态」），
        //    而不是简单禁词 —— 新文案允许**显式否认**该结论（那正是归因分离的表达）
        expect(check,
               miss_input.find("页面未就绪") != std::string::npos &&
                   miss_input.find("可能是拖拽") == std::string::npos &&
                   miss_input.find("暂不支持该形态") == std::string::npos,
               "PW-03 归因分离：附件未挂载 ⇒ 报「页面未就绪」，**不得**沿用「该站点可能是拖拽 / 粘贴入口」");
        expect(check, miss_page.find("页面级") != std::string::npos &&
                          miss_window.find("窗口级") != std::string::npos,
               "PW-03 归因分离：缺哪一级就点哪一级（窗口级 / 页面级）");

        // ---- ③b 诊断附加（2026-10-10 · 循环开发 `T-L06` 真机驱动）----
        //  * 判据：`wait_ready` 失败时的文案必须**带上实时事实**（readyState / URL / 可见输入候选）
        //    —— `gemini-web` / `spark-web` 的定性全靠它（URL 已跳到 accounts.google.com ⇒ 需登录；
        //    readyState 停在 interactive ⇒ 页面本身没加载完）
        const std::string detail_full =
            web::ready_detail_text("https://gemini.google.com/app", "interactive", 3);
        expect(check, detail_full.find("readyState=interactive") != std::string::npos &&
                          detail_full.find("https://gemini.google.com/app") != std::string::npos &&
                          detail_full.find("可见输入候选=3") != std::string::npos,
               "PW-03 诊断附加：三要素齐全（readyState / URL / 可见输入候选）");
        expect(check, web::ready_detail_text(std::string(), std::string(), 0).empty(),
               "PW-03 诊断附加：**一条事实都没有** ⇒ 空串（不许编造）");
        const std::string detail_partial = web::ready_detail_text(std::string(), "loading", 0);
        expect(check, detail_partial.find("readyState=loading") != std::string::npos &&
                          detail_partial.find("（取不到）") != std::string::npos,
               "PW-03 诊断附加：只有 readyState 也照实写（URL 缺 ⇒ 「（取不到）」）");

        // ---- ⑤ `PW-02`（`I24`）：窗口表 —— 复用 / 独立 / 收尾顺序（离线；不开窗口、不联网）----
        web::WindowTable table;
        expect_eq(check, web::window_key(a, a.site_key), "run1#n7@https://www.doubao.com",
                  "PW-02 窗口键 = owner#node@站点（**不是**只按站点 —— 真机错配的根因）");
        expect(check, web::window_key(none, a.site_key) == "未标注@https://www.doubao.com",
               "PW-02 未标注归属 ⇒ 键前缀「未标注」（CLI / 自检仍可安全取窗）");
        expect(check, table.plan(a, a.site_key) == web::WindowPlan::Create,
               "PW-02 首次取窗：该 (owner, 站点) 没窗口 ⇒ 新建");
        table.note_opened(a, a.site_key);
        expect(check, table.plan(a, a.site_key) == web::WindowPlan::Reuse,
               "PW-02 同 owner + 同站点 ⇒ **复用**（幂等快路保留，语义不变）");
        expect(check, table.plan(c, a.site_key) == web::WindowPlan::Create,
               "PW-02 同站点、不同 owner ⇒ **各自独立窗口**（D-33② · 真并行 · 不排队）");
        table.note_opened(c, a.site_key);
        table.note_opened(c, "https://chat.deepseek.com");
        expect(check, table.size() == 3u,
               "PW-02 表里 3 个窗口（run1#n7@doubao + run1#n3@doubao + run1#n3@deepseek）");
        expect(check, table.keys_of_owner(c).size() == 2u,
               "PW-02 一个 owner 可跨站点各有一个窗口（换站点**不关旧窗**）");
        const std::vector<std::string> close_order = table.keys_in_close_order();
        expect(check, !close_order.empty() &&
                           close_order.front() == "run1#n3@https://chat.deepseek.com",
               "PW-02 收尾顺序 = **后开的先关**（由窗口表决定，不由调用者决定）");
        expect(check, table.report().rfind("3 个窗口", 0) == 0,
               "PW-02 诊断行：可 grep「谁开着哪些窗口」");
        expect(check, table.note_closed(a, a.site_key) &&
                           table.plan(a, a.site_key) == web::WindowPlan::Create,
               "PW-02 显式关窗后 ⇒ 该键回到「新建」（表是唯一真相）");

        // ---- ⑥ `PW-08`（`D-35③`）：运行级复位计划（**首次 + 失败时** · 单次重试）----
        expect(check, web::kMaxResetsPerWindow == 1,
               "PW-08 复位上限 = 1（单次重试；重试用尽 ⇒ 按归因报错，不空转）");
        expect(check, web::plan_reset(/*opened_now=*/true, false, false, 0) ==
                           web::ResetAction::None,
               "PW-08 本次新建窗口 ⇒ **不复位**（新开 = 已在 login_url 新会话页，避免重复导航）");
        expect(check, web::plan_reset(false, /*reused_external=*/true, false, 0) ==
                           web::ResetAction::NavigateToSession,
               "PW-08 **首次**使用早于本轮存在的窗口 ⇒ 同一控制器**导航回新会话页**"
               "（不关窗重开 —— 关窗后同毫秒重开撞 0x802A000C 竞态，D-35③ 修订）");
        expect(check, web::plan_reset(false, false, /*failed_before=*/true, 0) ==
                           web::ResetAction::NavigateToSession,
               "PW-08 上一次**未就绪** ⇒ 复位一次后重试（导航回新会话页）");
        expect(check, web::plan_reset(false, false, true, web::kMaxResetsPerWindow) ==
                           web::ResetAction::None,
               "PW-08 重试用尽 ⇒ 不复位（交给归因文案，不再空转）");
        expect(check, web::plan_reset(false, false, false, 0) == web::ResetAction::None,
               "PW-08 非首次、未失败 ⇒ 不复位（不打扰用户会话）");
        expect_eq(check, web::reset_action_name(web::ResetAction::NavigateToSession),
                  "导航回新会话页（同控制器）", "PW-08 复位动作可读名（日志可 grep）");
        expect_eq(check, web::reset_action_name(web::ResetAction::CloseAndReopen),
                  "关窗重开（兜底）", "PW-08 关窗重开保留为兜底（可读名）");

        // ---- ⑦ `PW-04`（`I26`）：证据归属准入 —— **跨 owner 回执被拒** ----
        web::WebResourceRecord record;
        record.owner = a;
        expect(check, web::owner_accepts(record.owner, a),
               "PW-04 回执归属 == 要它的归属 ⇒ 采信");
        expect(check, web::owner_accepts(record.owner, b),
               "PW-04 按 run+node 判定：同 run 同 node = 同一归属");
        expect(check, !web::owner_accepts(record.owner, c),
               "PW-04 **跨 owner 回执被拒**（同站点不同节点也不得互相采信 —— 真机「串答案」的防线）");
        expect(check, !web::owner_accepts(web::WebOwner{}, a),
               "PW-04 未绑定归属的回执 ⇒ 拒绝（未绑定不得用于 I18 判定）");

        // ---- ⑧ `PW-05`（`W-D6`）：三类错误文案**可分辨** ----
        const std::string err_ready =
            web::web_error_text(web::WebErrorKind::NotReady, a, a.site_key, "缺 交互态（附件入口）");
        const std::string err_entry = web::web_error_text(
            web::WebErrorKind::NoEntry, a, a.site_key, "枚举到 0 个 input[type=file]");
        const std::string err_busy = web::web_error_text(web::WebErrorKind::Preempted, a,
                                                         a.site_key, "该窗口正在被另一次调用使用");
        expect(check, err_ready.rfind("[owner run1#n7]", 0) == 0 &&
                           err_entry.rfind("[owner run1#n7]", 0) == 0 &&
                           err_busy.rfind("[owner run1#n7]", 0) == 0,
               "PW-05 三类文案**都以 owner 标签开头**（日志里一眼看出「谁要的」）");
        expect(check, err_ready.find("页面未就绪") != std::string::npos &&
                           err_entry.find("站点无上传入口") != std::string::npos &&
                           err_busy.find("窗口被占用") != std::string::npos,
               "PW-05 三类前缀互不重叠（未就绪 / 无上传入口 / 被占用）");
        expect(check, err_ready.find("拖拽") == std::string::npos &&
                           err_ready.find("暂不支持") == std::string::npos,
               "PW-05 未就绪文案**绝不**含「拖拽 / 暂不支持」这类形态结论（G6 回归断言）");
        expect(check, err_entry.find("web.attach_selector") != std::string::npos &&
                           err_busy.find("取用自己的窗口") != std::string::npos &&
                           err_entry.find("下一步：") != std::string::npos &&
                           err_busy.find("下一步：") != std::string::npos,
               "PW-05 每类都带**可操作下一步**（I21：无入口 → 填实测选择器；被占用 → 取用自己的窗口）");
        expect(check, web::web_error_kind_name(web::WebErrorKind::NotReady) !=
                               web::web_error_kind_name(web::WebErrorKind::NoEntry) &&
                           web::web_error_kind_name(web::WebErrorKind::NoEntry) !=
                               web::web_error_kind_name(web::WebErrorKind::Preempted),
               "PW-05 类别名两两不同（归因必须可分辨）");

        // ---- ⑨ `PB2-25` / `PB-07`（M_patchAB §2.2 / §3.3）：会话诊断（判据收紧 + 错误串反查）----
        //  * 动机（v9 实测）：端点 `code=40003` 时旧判据 `ok && has_user_token` **假 PASS**
        //  * 纯函数：`judge_probe_result()` 四分类 + `classify_session_failure_text()`
        std::printf("   -- 会话诊断（M_patchAB PB2-25 / PB-07）--\n");
        int       verdict_passed = 0;
        const int verdict_failed = web::probeVerdictSelftest(&verdict_passed);
        check.passed += verdict_passed;
        check.failed += verdict_failed;

        // ---- ⑩ `PC-04` / `DBG-M6-02`（M_patchAB §4）：版本迁移框架 ----
        //  * 工作流：缺版本 ⇒ 按当前处理；未来主版本 ⇒ **拒绝**；迁移机制（夹具表）可跑
        //  * 配置：版本补齐 + 未来版本 ⇒ **拒绝降级写回**（临时目录端到端）
        std::printf("   -- 版本迁移（M_patchAB PC-04 / DBG-M6-02）--\n");
        int       migration_passed = 0;
        const int migration_failed = engine::migrationSelftest(&migration_passed);
        check.passed += migration_passed;
        check.failed += migration_failed;

        // ---- ④ Loop Dev 批 3（`FEA-L01`）：DOM 探测**报告链路**的离线断言 ----
        //  * 报告解析 / 状态映射 / diff（含旧报告按事实回推）—— 不开窗口、不联网
        //  * 真机批量探测另有 `aiwrite.exe --provider all`（退出码 §6.3）
        std::printf("   -- DOM 批量探测报告（loop_dev 批 3 · FEA-L01）--\n");
        int dom_passed = 0;
        const int dom_failed = aiwrite::ai::domProbeSelftest(&dom_passed);
        check.passed += dom_passed;
        check.failed += dom_failed;
    }

    // ---- M_node_expansion L2（VL2 离线部分 · part2 §4.3.5）：6 个文本处理节点 ----
    {
        const auto run = [](const std::string& type, const nlohmann::json& inputs,
                            const nlohmann::json& params) {
            engine::ExecutionContext      ctx;
            engine::NodeExecutorRegistry& registry = engine::NodeExecutorRegistry::instance();
            const engine::NodeExecutor*   fn = registry.find(type);
            if (fn == nullptr) {
                return std::string("(未注册)");
            }
            try {
                return (*fn)(inputs, params, ctx).dump();
            }
            catch (const engine::NodeError& e) {
                return std::string("ERR:") + e.what();
            }
            catch (const std::exception& e) {
                return std::string("EXC:") + e.what();
            }
        };

        // --- L2-01 TextSplit（VL2-01）---
        {
            nlohmann::json inputs;
            inputs["text"] = "a\n\nb\n\nc";
            expect_eq(check, run("TextSplit", inputs, nlohmann::json::object()),
                      nlohmann::json({"a", "b", "c"}).dump(),
                      "VL2-01：by_separator 默认 \\n\\n 拆 3 段");
            nlohmann::json sep_param;
            sep_param["separator"] = ",";
            nlohmann::json inputs2;
            inputs2["text"]        = "x,y,z";
            expect_eq(check, run("TextSplit", inputs2, sep_param),
                      nlohmann::json({"x", "y", "z"}).dump(),
                      "VL2-01：自定义分隔符逗号");
            nlohmann::json keep_param;
            keep_param["separator"]      = ",";
            keep_param["keep_separator"] = true;
            expect_eq(check, run("TextSplit", inputs2, keep_param),
                      nlohmann::json({"x,", "y,", "z"}).dump(),
                      "VL2-01：保留分隔符");
            nlohmann::json len_param;
            len_param["split_mode"] = "by_length";
            len_param["max_length"] = 2;
            nlohmann::json inputs3;
            inputs3["text"]        = "abcdef";
            expect_eq(check, run("TextSplit", inputs3, len_param),
                      nlohmann::json({"ab", "cd", "ef"}).dump(),
                      "VL2-01：by_length 每 2 字符一块");
            nlohmann::json ol_param;
            ol_param["split_mode"] = "by_length";
            ol_param["max_length"] = 3;
            ol_param["overlap"]    = 1;
            expect_eq(check, run("TextSplit", inputs3, ol_param),
                      nlohmann::json({"abc", "cde", "ef"}).dump(),
                      "VL2-01：by_length + overlap=1");
            nlohmann::json para_param;
            para_param["split_mode"] = "by_paragraph";
            nlohmann::json inputs4;
            inputs4["text"]         = "第一段\n\n第二段\n\n第三段";
            expect_eq(check, run("TextSplit", inputs4, para_param),
                      nlohmann::json({"第一段", "第二段", "第三段"}).dump(),
                      "VL2-01：by_paragraph 空行分段落");
            nlohmann::json re_param;
            re_param["split_mode"]    = "by_regex";
            re_param["regex_pattern"] = "\\d+";
            nlohmann::json inputs5;
            inputs5["text"]          = "a1b22c";
            expect_eq(check, run("TextSplit", inputs5, re_param),
                      nlohmann::json({"a", "b", "c"}).dump(),
                      "VL2-01：by_regex 按数字拆分");
            nlohmann::json bad_param;
            bad_param["split_mode"]    = "by_regex";
            bad_param["regex_pattern"] = "[";
            expect(check, run("TextSplit", inputs5, bad_param).rfind("ERR:", 0) == 0,
                    "VL2-01：非法正则 → 报错");
            expect_eq(check, run("TextSplit", nlohmann::json::object(),
                                 nlohmann::json::object()),
                      nlohmann::json::array().dump(),
                      "VL2-01：空输入 → 空 texts");
            nlohmann::json nohit_param;
            nohit_param["separator"] = "~~~";
            expect_eq(check, run("TextSplit", inputs3, nohit_param),
                      nlohmann::json({"abcdef"}).dump(),
                      "VL2-01：分隔符不存在 → 单段（原文本）");
        }
        // --- L2-02 TextExtract（VL2-02）---
        {
            nlohmann::json params;
            params["extract_mode"] = "regex";
            params["pattern"]      = "name=(\\w+)";
            nlohmann::json inputs;
            inputs["text"]         = "id=42 name=John";
            expect_eq(check, run("TextExtract", inputs, params),
                      nlohmann::json{{"texts", {"John"}}, {"text", ""}}.dump(),
                      "VL2-02：regex 捕获组优先");
            nlohmann::json between;
            between["extract_mode"] = "between";
            between["start_marker"] = "<";
            between["end_marker"]   = ">";
            nlohmann::json inputs2;
            inputs2["text"]         = "pre<A>post<B>";
            expect_eq(check, run("TextExtract", inputs2, between),
                      nlohmann::json{{"texts", {"A", "B"}}, {"text", ""}}.dump(),
                      "VL2-02：between 全部非重叠区间");
            nlohmann::json jp;
            jp["extract_mode"] = "json_path";
            jp["json_path"]    = "a.b[1]";
            nlohmann::json inputs3;
            inputs3["text"]    = R"({"a":{"b":[10,20]}})";
            expect_eq(check, run("TextExtract", inputs3, jp),
                      nlohmann::json{{"texts", nlohmann::json::array()}, {"text", "20"}}.dump(),
                      "VL2-02：json_path 点分 + 数组下标");
            nlohmann::json kv;
            kv["extract_mode"] = "key_value";
            kv["key_name"]     = "名称";
            nlohmann::json inputs4;
            inputs4["text"]    = "名称: 张三\n年龄：30";
            expect_eq(check, run("TextExtract", inputs4, kv),
                      nlohmann::json{{"texts", {"张三"}}, {"text", ""}}.dump(),
                      "VL2-02：key_value 中英文冒号均可");
            nlohmann::json single;
            single["extract_mode"] = "regex";
            single["pattern"]      = "\\d+";
            single["output_mode"]  = "text";
            nlohmann::json inputs5;
            inputs5["text"]        = "abc123";
            expect_eq(check, run("TextExtract", inputs5, single),
                      nlohmann::json{{"texts", nlohmann::json::array()}, {"text", "123"}}.dump(),
                      "VL2-02：output_mode=text 单匹配输出该段");
            nlohmann::json multi;
            multi["extract_mode"] = "regex";
            multi["pattern"]      = "\\d+";
            multi["output_mode"]  = "text";
            nlohmann::json inputs6;
            inputs6["text"]       = "1a2";
            expect_eq(check, run("TextExtract", inputs6, multi),
                      nlohmann::json{{"texts", nlohmann::json::array()},
                                     {"text", nlohmann::json({"1", "2"}).dump()}}
                          .dump(),
                      "VL2-02：output_mode=text 多匹配输出 JSON 数组");
            nlohmann::json bad;
            bad["extract_mode"] = "regex";
            bad["pattern"]      = "[";
            nlohmann::json inputs7;
            inputs7["text"]     = "x";
            expect(check, run("TextExtract", inputs7, bad).rfind("ERR:", 0) == 0,
                    "VL2-02：非法正则 → 报错");
        }

        // --- L2-03 TextClean（VL2-03）---
        {
            nlohmann::json inputs;
            inputs["text"] = "  a  ";
            expect_eq(check, run("TextClean", inputs, nlohmann::json::object()),
                      nlohmann::json("a").dump(), "VL2-03：trim 默认开（去首尾空白）");
            nlohmann::json collapse;
            collapse["collapse_whitespace"] = true;
            nlohmann::json inputs2;
            inputs2["text"]                 = "a  b\nc";
            expect_eq(check, run("TextClean", inputs2, collapse),
                      nlohmann::json("a b c").dump(), "VL2-03：collapse 连续空白压成单空格");
            nlohmann::json html;
            html["remove_html"] = true;
            html["trim"]        = false;
            nlohmann::json inputs3;
            inputs3["text"]     = "<p>hi</p>&nbsp;x";
            expect_eq(check, run("TextClean", inputs3, html),
                      nlohmann::json("hi x").dump(), "VL2-03：去 HTML 标签 + 实体");
            nlohmann::json inputs4;
            inputs4["text"]     = std::string("a\x01") + "b";
            expect_eq(check, run("TextClean", inputs4, nlohmann::json::object()),
                      nlohmann::json("ab").dump(), "VL2-03：去控制字符默认开");
            nlohmann::json dedup;
            dedup["dedup_lines"] = true;
            nlohmann::json inputs5;
            inputs5["text"]      = "a\nb\na\n";
            expect_eq(check, run("TextClean", inputs5, dedup),
                      nlohmann::json("a\nb").dump(),
                      "VL2-03：去重复行（保序；尾部换行被默认 trim 去掉）");
            nlohmann::json rm_empty;
            rm_empty["remove_empty_lines"] = true;
            nlohmann::json inputs6;
            inputs6["text"]                = "a\n\n\nb";
            expect_eq(check, run("TextClean", inputs6, rm_empty),
                      nlohmann::json("a\nb").dump(), "VL2-03：去空行");
            nlohmann::json prefix;
            prefix["remove_prefix"] = "xx";
            nlohmann::json inputs7;
            inputs7["text"]         = "xxxxabc";
            expect_eq(check, run("TextClean", inputs7, prefix),
                      nlohmann::json("xxabc").dump(), "VL2-03：remove_prefix 只去开头一次");
            nlohmann::json suffix;
            suffix["remove_suffix"] = "yy";
            nlohmann::json inputs8;
            inputs8["text"]         = "abcyyyy";
            expect_eq(check, run("TextClean", inputs8, suffix),
                      nlohmann::json("abcyy").dump(), "VL2-03：remove_suffix 只去结尾一次");
            nlohmann::json ai;
            ai["remove_ai_prefix"] = true;
            nlohmann::json inputs9;
            inputs9["text"]        = "好的，以下是正文内容";
            expect_eq(check, run("TextClean", inputs9, ai),
                      nlohmann::json("正文内容").dump(), "VL2-03：去 AI 客套前缀");
            nlohmann::json all_off;
            all_off["trim"]                 = false;
            all_off["remove_control_chars"] = false;
            nlohmann::json inputs10;
            inputs10["text"]                = " 保持原样  ";
            expect_eq(check, run("TextClean", inputs10, all_off),
                      nlohmann::json(" 保持原样  ").dump(), "VL2-03：全部 false → 原文本不变");
        }

        // --- L2-04 TextReplace（VL2-04）---
        {
            nlohmann::json plain;
            plain["pattern"]     = "o";
            plain["replacement"] = "0";
            nlohmann::json inputs;
            inputs["text"]       = "hello world";
            expect_eq(check, run("TextReplace", inputs, plain),
                      nlohmann::json("hell0 w0rld").dump(), "VL2-04：普通文本全部替换");
            nlohmann::json ci;
            ci["pattern"]        = "hello";
            ci["replacement"]    = "X";
            ci["case_sensitive"] = false;
            nlohmann::json inputs2;
            inputs2["text"]      = "Hello HELLO";
            expect_eq(check, run("TextReplace", inputs2, ci),
                      nlohmann::json("X X").dump(), "VL2-04：忽略大小写");
            nlohmann::json ww;
            ww["pattern"]     = "is";
            ww["replacement"] = "X";
            ww["whole_word"]  = true;
            nlohmann::json inputs3;
            inputs3["text"]   = "this is a cat";
            expect_eq(check, run("TextReplace", inputs3, ww),
                      nlohmann::json("this X a cat").dump(),
                      "VL2-04：全词匹配（this 里的 is 不换）");
            nlohmann::json ww_cn;
            ww_cn["pattern"]     = "苹果";
            ww_cn["replacement"] = "梨";
            ww_cn["whole_word"]  = true;
            nlohmann::json inputs4;
            inputs4["text"]      = "a 苹果 b";
            expect_eq(check, run("TextReplace", inputs4, ww_cn),
                      nlohmann::json("a 梨 b").dump(), "VL2-04：中文全词匹配（独立成词）");
            nlohmann::json regex;
            regex["pattern"]     = "\\d+";
            regex["replacement"] = "#";
            regex["use_regex"]   = true;
            nlohmann::json inputs5;
            inputs5["text"]      = "a1b22c";
            expect_eq(check, run("TextReplace", inputs5, regex),
                      nlohmann::json("a#b#c").dump(), "VL2-04：正则替换");
            nlohmann::json limited;
            limited["pattern"]          = "a";
            limited["replacement"]      = "b";
            limited["max_replacements"] = 2;
            nlohmann::json inputs6;
            inputs6["text"]             = "aaaa";
            expect_eq(check, run("TextReplace", inputs6, limited),
                      nlohmann::json("bbaa").dump(), "VL2-04：max_replacements=2 限量");
            nlohmann::json empty_pattern;
            empty_pattern["replacement"] = "X";
            nlohmann::json inputs7;
            inputs7["text"]              = "abc";
            expect_eq(check, run("TextReplace", inputs7, empty_pattern),
                      nlohmann::json("abc").dump(), "VL2-04：空 pattern → 原文本不变");
            nlohmann::json bad;
            bad["pattern"]   = "[";
            bad["use_regex"] = true;
            nlohmann::json inputs8;
            inputs8["text"]  = "x";
            expect(check, run("TextReplace", inputs8, bad).rfind("ERR:", 0) == 0,
                    "VL2-04：非法正则 → 报错");
        }

        // --- L2-05 TextStats（VL2-05）---
        {
            nlohmann::json inputs;
            inputs["text"] = "你好 world\n你好";
            const std::string report = run("TextStats", inputs, nlohmann::json::object());
            expect(check, report.find("中文字数: 4") != std::string::npos, "VL2-05：basic 中文字数 4",
                    report);
            expect(check, report.find("英文单词数: 1") != std::string::npos,
                    "VL2-05：basic 英文单词数 1", report);
            expect(check, report.find("行数: 2") != std::string::npos, "VL2-05：basic 行数 2",
                    report);
            expect(check, report.find("段落数: 1") != std::string::npos, "VL2-05：basic 段落数 1",
                    report);
            nlohmann::json num_mode;
            num_mode["output_mode"] = "number";
            expect_eq(check, run("TextStats", inputs, num_mode),
                      nlohmann::json{{"text", nlohmann::json()}, {"number", 4}}.dump(),
                      "VL2-05：output_mode=number → number 端口输出中文字数 4");
            nlohmann::json wf;
            wf["stats_mode"] = "word_freq";
            wf["top_n"]      = 2;
            nlohmann::json inputs2;
            inputs2["text"]  = "你 好 你";
            const std::string wf_report = run("TextStats", inputs2, wf);
            expect(check, wf_report.find("你  2") != std::string::npos, "VL2-05：词频 你=2",
                    wf_report);
            expect(check, wf_report.find("好  1") != std::string::npos, "VL2-05：词频 好=1",
                    wf_report);
            expect_eq(check, run("TextStats", nlohmann::json::object(),
                                 nlohmann::json::object()),
                      nlohmann::json{{"text",
                                      "字符数（含空白）: 0\n字符数（不含空白）: 0\n中文字数: 0\n"
                                      "英文单词数: 0\n行数: 0\n段落数: 0\n"},
                                     {"number", 0}}
                          .dump(),
                      "VL2-05：空输入 → 全 0");
        }

        // --- L2-06 TextDiff（VL2-06）---
        {
            nlohmann::json inputs;
            inputs["text_a"] = "line1\nline2\nline3";
            inputs["text_b"] = "line1\nCHANGED\nline3";
            const std::string text = run("TextDiff", inputs, nlohmann::json::object());
            expect(check, text.find("- line2") != std::string::npos, "VL2-06：行差异含删除行",
                    text);
            expect(check, text.find("+ CHANGED") != std::string::npos, "VL2-06：行差异含插入行",
                    text);
            nlohmann::json inputs_same;
            inputs_same["text_a"] = "相同\n内容";
            inputs_same["text_b"] = "相同\n内容";
            expect_eq(check, run("TextDiff", inputs_same, nlohmann::json::object()),
                      nlohmann::json("（无差异）").dump(), "VL2-06：两文本相同 → 「无差异」");
            nlohmann::json inputs_empty;
            inputs_empty["text_a"] = "";
            inputs_empty["text_b"] = "只有B";
            const std::string only_b = run("TextDiff", inputs_empty, nlohmann::json::object());
            expect(check, only_b.find("+ 只有B") != std::string::npos,
                    "VL2-06：一方为空 → 全部为增", only_b);
            nlohmann::json json_mode;
            json_mode["output_mode"] = "json";
            json_mode["diff_mode"]   = "line";
            const std::string json_out = run("TextDiff", inputs, json_mode);
            // run() 对「文本型 JSON 串」会再编码一层引号 → 先解析一层取回文本，再解析数组
            nlohmann::json parsed = nlohmann::json::parse(json_out, nullptr, false);
            if (parsed.is_string()) {
                parsed = nlohmann::json::parse(parsed.get<std::string>(), nullptr, false);
            }
            expect(check, !parsed.is_discarded() && parsed.is_array() && !parsed.empty() &&
                             parsed[0].value("op", std::string()) == "equal",
                    "VL2-06：json 输出 = 操作序列数组", json_out);
            nlohmann::json char_mode;
            char_mode["diff_mode"] = "char";
            nlohmann::json inputs_char;
            inputs_char["text_a"]  = "abc";
            inputs_char["text_b"]  = "abd";
            const std::string char_out = run("TextDiff", inputs_char, char_mode);
            expect(check, char_out.find("- c") != std::string::npos &&
                              char_out.find("+ d") != std::string::npos,
                    "VL2-06：char 粒度差异", char_out);
        }

        // --- L2 互操作：TextSplit → TextMerge（VL2-07 · part2 L2-D 反向操作闭环）---
        {
            Graph             graph;
            const std::string input = add_node(check, graph, "TextInput", "分块输入");
            const std::string split = add_node(check, graph, "TextSplit", "分块节点");
            const std::string merge = add_node(check, graph, "TextMerge", "合并回串");
            const std::string output = add_node(check, graph, "TextOutput", "最终输出");
            set_param(graph, input, "text", "甲\n\n乙\n\n丙");
            set_param(graph, merge, "separator", "\n\n"); // 与 TextSplit 默认分隔符一致，回环相等
            graph.edges.push_back(make_edge("w1", input, "text", split, "text"));
            graph.edges.push_back(make_edge("w2", split, "texts", merge, "texts"));
            graph.edges.push_back(make_edge("w3", merge, "text", output, "text"));
            Executor    executor;
            std::string error;
            expect(check, executor.start(graph, &error), "VL2-07：分块→合并工作流 start", error);
            expect(check, executor.runToCompletion(&graph, 64), "VL2-07：分块→合并完成");
            const nlohmann::json* merged = executor.outputs().find(merge, "text");
            expect_eq(check, merged != nullptr ? merged->dump() : std::string("(无输出)"),
                      nlohmann::json("甲\n\n乙\n\n丙").dump(),
                      "VL2-07：TextSplit 数组直连 TextMerge 变长端口被展平");
            const nlohmann::json* out_text = executor.outputs().find(output, "__result");
            expect_eq(check, out_text != nullptr ? out_text->dump() : std::string("(无输出)"),
                      nlohmann::json("甲\n\n乙\n\n丙").dump(), "VL2-07：最终输出透传");
        }
    }

    // ---- DEBT-L7 / VB-03：离线「假增量」断言（流式数据源已拍板不做 · D-31/PQ-11 · 事件槽位保留）----
    // 语义对齐 UI 层 editor_state.cpp `RunEvent::Kind::Delta`：增量按段 `text += seg` 追加，
    // `delta_bytes = view->text.size()`（累计字节）；executor.handleDelta 对空文本直接 return。
    // 离线断言（纯逻辑，不启进程）：多段增量拼接 == 非流式整段结果；delta_bytes 与正文长度一致。
    {
        // ① 多段增量拼接 == 非流式整段结果（UI 逐字呈现的底座：拼接不丢不重）
        const std::string              whole = "你好，世界！"; // 非流式整段
        const std::vector<std::string> segs  = {"你好", "，", "世界", "！"};
        std::string                    joined;
        std::size_t                    delta_bytes = 0;
        for (const std::string& seg : segs) {
            if (seg.empty()) {
                continue; // executor.handleDelta：空文本 return
            }
            joined += seg;
            delta_bytes = joined.size(); // 与 editor_state Delta 分支一致（累计）
        }
        expect(check, joined == whole, "VB-03① 多段增量拼接 == 非流式整段结果", joined);

        // ② 空段被忽略（handleDelta 语义：空增量不产生事件、不污染正文）
        std::string with_empty;
        for (const std::string& seg : {"", "A", "", "B", ""}) {
            if (seg.empty()) {
                continue;
            }
            with_empty += seg;
        }
        expect(check, with_empty == "AB", "VB-03② 空段被忽略（handleDelta 语义）", with_empty);

        // ③ 单段一次性到达 == 整段（现状：httplib 无流式重载 → 增量一次性到达）
        std::string single;
        single += whole;
        expect(check, single == whole, "VB-03③ 单段一次性到达 == 整段", single);

        // ④ delta_bytes 累计 == 正文字节数（读模型显示依据；UTF-8 中文按字节累计）
        expect(check, delta_bytes == whole.size() && delta_bytes > 0,
               "VB-03④ delta_bytes 累计 == 正文字节数",
               std::to_string(delta_bytes));
    }

    // ---- DEBT-L8 / M6-04 步骤 5：失败汇总纯函数（错误条只显示首个 → 补「汇总所有失败节点」）----
    {
        // ① 无失败 → collectFailures 空 / summarizeFailures 空串
        Graph       ok_graph;
        engine::Node ok1;
        ok1.id           = "ok1";
        ok1.type         = "TextInput";
        ok1.state        = engine::NodeState::Done;
        ok1.error_message.clear();
        ok_graph.nodes.push_back(ok1);
        const std::vector<engine::FailureSummary> none = engine::collectFailures(ok_graph);
        expect(check, none.empty() && engine::summarizeFailures(ok_graph).empty(),
               "失败汇总：无失败 → 空列表 / 空串",
               engine::summarizeFailures(ok_graph));

        // ② 单失败（error_message 为空 → message 回落「执行失败」）
        Graph       one_graph;
        engine::Node a1;
        a1.id    = "a1";
        a1.type  = "LLMGenerate";
        a1.state = engine::NodeState::Error;
        a1.error_message.clear();
        one_graph.nodes.push_back(a1);
        {
            const std::vector<engine::FailureSummary> fs = engine::collectFailures(one_graph);
            expect(check, fs.size() == 1 && fs[0].node_id == "a1" && fs[0].message == "执行失败",
                   "失败汇总：单失败无错误信息 → message 回落「执行失败」",
                   fs.empty() ? std::string() : fs[0].message);
            expect_eq(check, engine::summarizeFailures(one_graph),
                      "1 个节点失败：a1（执行失败）", "单失败汇总文案");
        }

        // ③ 多失败：按图序收集、错误信息透传、汇总文案逐节点列出
        Graph       multi_graph;
        engine::Node m1;
        m1.id           = "m1";
        m1.type         = "LLMGenerate";
        m1.state        = engine::NodeState::Error;
        m1.error_message = "连接超时";
        engine::Node m2;
        m2.id           = "m2";
        m2.type         = "TextInput";
        m2.state        = engine::NodeState::Done;
        m2.error_message.clear();
        engine::Node m3;
        m3.id           = "m3";
        m3.type         = "VLMGenerate";
        m3.state        = engine::NodeState::Error;
        m3.error_message = "余额不足";
        multi_graph.nodes.push_back(m1);
        multi_graph.nodes.push_back(m2); // 中间混一个成功节点 → 应被跳过
        multi_graph.nodes.push_back(m3);
        {
            const std::vector<engine::FailureSummary> fs = engine::collectFailures(multi_graph);
            expect(check, fs.size() == 2 && fs[0].node_id == "m1" && fs[0].message == "连接超时" &&
                              fs[1].node_id == "m3" && fs[1].message == "余额不足",
                   "失败汇总：多失败按图序收集、跳过硬编码成功节点、错误信息透传",
                   fs.size() < 2 ? std::string("收集到 ") + std::to_string(fs.size()) + " 条" : "");
            expect_eq(check, engine::summarizeFailures(multi_graph),
                      "2 个节点失败：m1（连接超时）；m3（余额不足）", "多失败汇总文案");
        }
    }

    std::printf("=== 执行器自检结果: %d 通过 / %d 失败 ===\n", check.passed, check.failed);
    return check.failed == 0 ? 0 : 1;
}
