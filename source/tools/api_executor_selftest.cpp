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

// ------------------------------------------------------ 执行器自检（P3-1）-----
engine::Edge make_edge(const std::string& id, const std::string& from_node,
                       const std::string& from_port, const std::string& to_node,
                       const std::string& to_port)
{
    engine::Edge edge;
    edge.id        = id;
    edge.from_node = from_node;
    edge.from_port = from_port;
    edge.to_node   = to_node;
    edge.to_port   = to_port;
    return edge;
}

bool contains_text(const std::vector<std::string>& messages, const std::string& needle)
{
    return std::any_of(messages.begin(), messages.end(), [&needle](const std::string& text) {
        return text.find(needle) != std::string::npos;
    });
}

// M2-03 拓扑排序 + M2-06 加载/运行前校验（P3-2 会在此追加执行相关断言）
int executor_selftest()
{
    using engine::Graph;
    using engine::ValidationMessages;

    Check check;
    std::printf("[P3-1] 拓扑排序 / 加载校验 / 运行前校验 自检\n");
    engine::registerAllNodes();

    // ------------------------------------------------ 1. 拓扑排序（Kahn）-----
    {
        Graph                    graph;
        std::vector<std::string> order;
        std::string              error;
        expect(check, graph.topologicalOrder(&order, &error) && order.empty(),
               "空图：拓扑排序成功且序列为空", error);
    }
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "线性图 n1");
        const std::string n2 = add_node(check, graph, "PromptTemplate", "线性图 n2");
        const std::string n3 = add_node(check, graph, "TextOutput", "线性图 n3");
        graph.edges.push_back(make_edge("e1", n1, "text", n2, "vars"));
        graph.edges.push_back(make_edge("e2", n2, "text", n3, "text"));

        std::vector<std::string> order;
        std::string              error;
        const bool               ok = graph.topologicalOrder(&order, &error);
        expect(check, ok && order.size() == 3, "线性图：拓扑排序成功", error);
        if (order.size() == 3) {
            expect(check, order[0] == n1 && order[1] == n2 && order[2] == n3,
                   "线性图：顺序为 n1→n2→n3", order[0] + "→" + order[1] + "→" + order[2]);
        }
    }
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "菱形图 n1");
        const std::string n2 = add_node(check, graph, "PromptTemplate", "菱形图 n2");
        const std::string n3 = add_node(check, graph, "PromptTemplate", "菱形图 n3");
        const std::string n4 = add_node(check, graph, "TextMerge", "菱形图 n4");
        graph.edges.push_back(make_edge("e1", n1, "text", n2, "vars"));
        graph.edges.push_back(make_edge("e2", n1, "text", n3, "vars"));
        graph.edges.push_back(make_edge("e3", n2, "text", n4, "texts"));
        graph.edges.push_back(make_edge("e4", n3, "text", n4, "texts"));

        std::vector<std::string> order;
        std::string              error;
        const bool               ok = graph.topologicalOrder(&order, &error);
        expect(check, ok && order.size() == 4 && order.front() == n1 && order.back() == n4,
               "菱形图：起点最先、汇点最后", error);
    }
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "孤立节点 n1");
        const std::string n2 = add_node(check, graph, "PromptTemplate", "孤立节点 n2");
        const std::string n3 = add_node(check, graph, "TextOutput", "孤立节点 n3");
        graph.edges.push_back(make_edge("e1", n2, "text", n3, "text"));

        std::vector<std::string> order;
        std::string              error;
        const bool               ok = graph.topologicalOrder(&order, &error);
        expect(check, ok && order.size() == 3, "孤立节点也进入拓扑序列", error);
        expect(check, ok && !order.empty() && order.front() == n1, "孤立节点（无入边）排在最前");
    }

    {
        Graph             graph;
        const std::string p1 = add_node(check, graph, "PromptTemplate", "环图 p1");
        const std::string p2 = add_node(check, graph, "PromptTemplate", "环图 p2");
        graph.edges.push_back(make_edge("e1", p1, "text", p2, "vars"));
        graph.edges.push_back(make_edge("e2", p2, "text", p1, "vars"));

        std::vector<std::string> order;
        std::string              error;
        const bool               ok = graph.topologicalOrder(&order, &error);
        expect(check, !ok && !error.empty(), "两节点环：拓扑排序失败");
        expect(check, !ok && error.find(p1) != std::string::npos &&
                         error.find(p2) != std::string::npos,
               "环错误信息列出环内节点", error);
    }
    {
        Graph             graph;
        const std::string p1 = add_node(check, graph, "PromptTemplate", "自环图 p1");
        graph.edges.push_back(make_edge("e1", p1, "text", p1, "vars"));

        std::vector<std::string> order;
        std::string              error;
        expect(check, !graph.topologicalOrder(&order, &error), "自环：拓扑排序失败", error);
    }
    {
        Graph                    graph;
        std::vector<std::string> ids;
        for (int i = 0; i < 20; ++i) {
            const std::string id =
                add_node(check, graph, "PromptTemplate", "20 节点链 #" + std::to_string(i + 1));
            ids.push_back(id);
            if (i > 0) {
                graph.edges.push_back(
                    make_edge("e" + std::to_string(i), ids[i - 1], "text", ids[i], "vars"));
            }
        }
        std::vector<std::string> order;
        std::string              error;
        const bool               ok = graph.topologicalOrder(&order, &error);
        expect(check, ok && order.size() == 20, "20 节点链：拓扑排序成功", error);
        expect(check, ok && !order.empty() && order.front() == ids.front() && order.back() == ids.back(),
               "20 节点链：首尾顺序正确");
    }
    {
        Graph             graph;
        const std::string n1 = add_node(check, graph, "PromptTemplate", "悬空边图 n1");
        graph.edges.push_back(make_edge("e1", "nope", "text", n1, "vars"));

        std::vector<std::string> order;
        std::string              error;
        expect(check, graph.topologicalOrder(&order, &error) && order.size() == 1,
               "悬空边被拓扑排序忽略（交给校验报告）", error);

        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "起点节点不存在"),
               "悬空边：加载校验拒绝");
    }

    // ------------------------------------- 2. 加载校验 / 运行前校验（示例图）----
    Graph             sample;
    const std::string text_in = add_node(check, sample, "TextInput", "示例 n1（文本输入）");
    const std::string tmpl    = add_node(check, sample, "PromptTemplate", "示例 n2（模板）");
    const std::string llm     = add_node(check, sample, "LLMGenerate", "示例 n3（文本生成）");
    const std::string out     = add_node(check, sample, "TextOutput", "示例 n4（文本输出）");
    const std::string provider = add_node(check, sample, "ProviderConfig", "示例 n5（提供商配置）");
    sample.edges.push_back(make_edge("e1", text_in, "text", tmpl, "vars"));
    sample.edges.push_back(make_edge("e2", tmpl, "text", llm, "prompt"));
    sample.edges.push_back(make_edge("e3", provider, "provider", llm, "provider"));
    sample.edges.push_back(make_edge("e4", llm, "text", out, "text"));
    if (engine::Node* node = sample.findNode(text_in)) {
        if (engine::Param* param = node->findParam("text")) {
            param->value = "标题：秋日";
        }
    }
    {
        ValidationMessages errors;
        const bool         ok = engine::validateWorkflow(sample, &errors);
        expect(check, ok, "示例工作流（5 节点 4 连线）：加载校验通过",
               errors.empty() ? std::string() : errors.front());
    }
    { // 无 Key 时只给 warning，不阻断（临时清环境变量后恢复）
        std::string saved;
        if (const char* value = std::getenv("DEEPSEEK_API_KEY")) {
            saved = value;
            if (!saved.empty()) {
                _putenv_s("DEEPSEEK_API_KEY", "");
            }
        }
        ValidationMessages errors;
        ValidationMessages warnings;
        const bool         ok = engine::validateBeforeRun(sample, &errors, &warnings);
        expect(check, ok, "未配置 Key：运行前校验仍通过（不阻断）");
        expect(check, contains_text(warnings, "未配置 API Key"), "未配置 Key：给出 warning");
        if (!saved.empty()) {
            _putenv_s("DEEPSEEK_API_KEY", saved.c_str());
        }
    }

    { // 必填输入未连接：LLMGenerate.prompt（结构仍合法 → 仅运行前拒绝）
        Graph graph = sample;
        graph.removeEdge("e2");
        ValidationMessages errors;
        ValidationMessages warnings;
        expect(check, !engine::validateBeforeRun(graph, &errors, &warnings),
               "必填输入未连接：运行前校验拒绝");
        expect(check, contains_text(errors, "必填输入") && contains_text(errors, llm),
               "错误信息含节点 id 与「必填输入」", errors.empty() ? std::string() : errors.front());
        ValidationMessages load_errors;
        expect(check, engine::validateWorkflow(graph, &load_errors),
               "必填输入未连接：加载校验仍通过（分层清晰）");
    }
    { // 可变长输入无入边：TextMerge.texts
        Graph             graph = sample;
        const std::string merge = add_node(check, graph, "TextMerge", "悬空 TextMerge");
        ValidationMessages errors;
        ValidationMessages warnings;
        expect(check, !engine::validateBeforeRun(graph, &errors, &warnings),
               "可变长输入无入边：运行前校验拒绝");
        expect(check, contains_text(errors, merge) && contains_text(errors, "至少需要 1 条连线"),
               "错误信息说明可变长输入要求");
    }
    { // 必填参数为空：PromptTemplate.template
        Graph graph = sample;
        if (engine::Node* node = graph.findNode(tmpl)) {
            if (engine::Param* param = node->findParam("template")) {
                param->value = "";
            }
        }
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) && contains_text(errors, "必填"),
               "必填参数为空：加载校验拒绝");
    }
    { // 端口类型不兼容：Text 输出 → Image 输入
        Graph             graph = sample;
        const std::string vlm   = add_node(check, graph, "VLMGenerate", "图片理解（类型不兼容用例）");
        graph.edges.push_back(make_edge("e9", llm, "text", vlm, "image"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "端口类型不兼容"),
               "端口类型不兼容：加载校验拒绝");
    }
    { // 端口不存在
        Graph graph = sample;
        graph.edges.push_back(make_edge("e9", tmpl, "nope", out, "text"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "起点端口不存在"),
               "端口不存在：加载校验拒绝");
    }
    { // 非可变长输入被重复占用
        Graph graph = sample;
        graph.edges.push_back(make_edge("e9", tmpl, "text", out, "text"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "重复占用"),
               "输入端口重复占用：加载校验拒绝");
    }
    { // 未知节点类型
        Graph       graph;
        engine::Node unknown;
        unknown.id   = "n1";
        unknown.type = "NotExists";
        graph.nodes.push_back(unknown);
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "未知节点类型"),
               "未知节点类型：加载校验拒绝");
    }
    { // 缺少提供商配置节点 → warning（provider 输入可选，不阻断）
        Graph graph = sample;
        graph.removeNode(provider);
        ValidationMessages errors;
        ValidationMessages warnings;
        const bool         ok = engine::validateBeforeRun(graph, &errors, &warnings);
        expect(check, ok, "缺少提供商节点：运行前校验仍通过（provider 为可选输入）");
        expect(check, contains_text(warnings, "提供商配置"), "缺少提供商节点：给出 warning");
    }
    { // web 模式 → 登录提示 warning（不阻断）
        Graph graph = sample;
        if (engine::Node* node = graph.findNode(provider)) {
            if (engine::Param* mode = node->findParam("mode")) {
                mode->value = "web";
            }
        }
        ValidationMessages errors;
        ValidationMessages warnings;
        expect(check, engine::validateBeforeRun(graph, &errors, &warnings), "web 模式：运行前校验通过");
        expect(check, contains_text(warnings, "网页版模式"), "web 模式：给出登录提示 warning");
    }
    { // 空图
        Graph              graph;
        ValidationMessages errors;
        ValidationMessages warnings;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "工作流为空"),
               "空图：加载校验拒绝");
        expect(check, !engine::validateBeforeRun(graph, &errors, &warnings), "空图：运行前校验拒绝");
    }

    // ---- DEBT-L9 / M6-06 校验边界用例补强（三级校验已齐 · 补齐未覆盖边界）----
    { // 节点 id 重复
        Graph             graph;
        engine::Node dup;
        dup.id   = "n1";
        dup.type = "TextInput";
        graph.nodes.push_back(dup);
        graph.nodes.push_back(dup); // 同 id 两份
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "节点 id 重复"),
               "节点 id 重复：加载校验拒绝",
               errors.empty() ? std::string() : errors.front());
    }
    { // 终点节点不存在（与「起点节点不存在」对称的边界）
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "终点悬空图 n1");
        graph.edges.push_back(make_edge("e1", n1, "text", "ghost", "text"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "终点节点不存在"),
               "终点节点不存在：加载校验拒绝",
               errors.empty() ? std::string() : errors.front());
    }
    { // 终点端口不存在（与「起点端口不存在」对称的边界）
        Graph             graph;
        const std::string n1 = add_node(check, graph, "TextInput", "终点端口悬空图 n1");
        const std::string n2 = add_node(check, graph, "TextOutput", "终点端口悬空图 n2");
        graph.edges.push_back(make_edge("e1", n1, "text", n2, "ghost"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "终点端口不存在"),
               "终点端口不存在：加载校验拒绝",
               errors.empty() ? std::string() : errors.front());
    }
    { // 上级节点（Composition_node）规格不存在
        Graph             graph;
        engine::Node comp;
        comp.id           = "n1";
        comp.type         = "Composition_node";
        comp.composite_id = "no-such-composite-spec";
        graph.nodes.push_back(comp);
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "上级节点规格不存在"),
               "上级节点规格不存在：加载校验拒绝",
               errors.empty() ? std::string() : errors.front());
    }
    { // 环（A→B→A）：拓扑排序失败 → 加载校验拒绝
        Graph             graph;
        const std::string pt1 = add_node(check, graph, "PromptTemplate", "环图 pt1");
        const std::string pt2 = add_node(check, graph, "PromptTemplate", "环图 pt2");
        graph.edges.push_back(make_edge("e1", pt1, "text", pt2, "vars"));
        graph.edges.push_back(make_edge("e2", pt2, "text", pt1, "vars"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "环"),
               "环：加载校验拒绝",
               errors.empty() ? std::string() : errors.front());
    }

    // ---- POL-C-01 / M6-06 校验边界补强（阶段三 · 正确性 · 2026-10-21）----
    { // 参数越界（整体级加载校验）：LLMGenerate.temperature = 5.0 超出 0~2
        Graph graph = sample;
        if (engine::Node* node = graph.findNode(llm)) {
            if (engine::Param* param = node->findParam("temperature")) {
                param->value = 5.0;
            }
        }
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "不能大于"),
               "参数越界：加载校验拒绝且文案可操作（数值上限）",
               errors.empty() ? std::string() : errors.front());
    }
    { // 枚举非法值（整体级加载校验）：ProviderConfig.mode 非法
        Graph graph = sample;
        if (engine::Node* node = graph.findNode(provider)) {
            if (engine::Param* mode = node->findParam("mode")) {
                mode->value = "nonsense-mode";
            }
        }
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "取值不在枚举范围内"),
               "枚举非法值：加载校验拒绝且文案可操作",
               errors.empty() ? std::string() : errors.front());
    }
    { // 自环（A→A）：加载校验级拒绝（拓扑排序失败路径，与 A→B→A 对称）
        Graph             graph;
        const std::string pt1 = add_node(check, graph, "PromptTemplate", "自环校验图 pt1");
        graph.edges.push_back(make_edge("e1", pt1, "text", pt1, "vars"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) &&
                         contains_text(errors, "环"),
               "自环：加载校验拒绝",
               errors.empty() ? std::string() : errors.front());
    }
    { // 多错误一次报告：未知节点类型 + 悬空边**同时**报告（found 累积，不短路）
        Graph       graph;
        engine::Node unknown;
        unknown.id   = "n1";
        unknown.type = "NotExists";
        graph.nodes.push_back(unknown);
        const std::string n2 = add_node(check, graph, "TextInput", "多错误图 n2");
        graph.edges.push_back(make_edge("e1", "ghost", "text", n2, "text"));
        ValidationMessages errors;
        expect(check, !engine::validateWorkflow(graph, &errors) && errors.size() >= 2,
               "多错误一次报告：一次校验返回全部错误（≥2 条，不短路）",
               errors.empty() ? std::string() : errors.front());
    }
    { // 可变长输入多条入边合法（正边界：不误报「重复占用」）
        Graph             graph = sample;
        const std::string merge = add_node(check, graph, "TextMerge", "变长双入边 merge");
        graph.edges.push_back(make_edge("e9", text_in, "text", merge, "texts"));
        graph.edges.push_back(make_edge("e10", tmpl, "text", merge, "texts"));
        ValidationMessages errors;
        expect(check, engine::validateWorkflow(graph, &errors),
               "可变长输入多条入边：加载校验通过（非可变长才拒重复占用）",
               errors.empty() ? std::string() : errors.front());
        ValidationMessages run_errors;
        ValidationMessages warnings;
        expect(check, engine::validateBeforeRun(graph, &run_errors, &warnings),
               "可变长输入多条入边：运行前校验通过",
               run_errors.empty() ? std::string() : run_errors.front());
    }
    { // 可选输入 provider 未连线：运行前校验不拒绝（is_optional 语义）
        Graph graph = sample;
        graph.removeEdge("e3"); // provider → llm.provider（可选输入）
        ValidationMessages errors;
        ValidationMessages warnings;
        expect(check, engine::validateBeforeRun(graph, &errors, &warnings),
               "可选输入未连线：运行前校验通过（is_optional 不强制）",
               errors.empty() ? std::string() : errors.front());
    }
    { // 运行前校验继承加载校验（子集关系）：必填参数为空 → 运行前同样拒绝
        Graph graph = sample;
        if (engine::Node* node = graph.findNode(tmpl)) {
            if (engine::Param* param = node->findParam("template")) {
                param->value = "";
            }
        }
        ValidationMessages errors;
        ValidationMessages warnings;
        expect(check, !engine::validateBeforeRun(graph, &errors, &warnings),
               "运行前校验继承加载校验：必填参数为空同样拒绝",
               errors.empty() ? std::string() : errors.front());
    }

    std::printf("=== 执行器自检结果: %d 通过 / %d 失败 ===\n", check.passed, check.failed);
    return check.failed == 0 ? 0 : 1;
}
