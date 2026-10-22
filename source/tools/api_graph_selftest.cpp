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


// ---------------------------------------------------------- 图模型自检 -------

void expect(Check& check, bool ok, const std::string& name, const std::string& detail)
{
    if (ok) {
        ++check.passed;
        std::printf("   PASS  %s\n", name.c_str());
    }
    else {
        ++check.failed;
        std::printf("   FAIL  %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ->  ",
                    detail.c_str());
    }
}

void expect_eq(Check& check, const std::string& actual, const std::string& wanted,
               const std::string& name)
{
    expect(check, actual == wanted, name, "期望 " + wanted + "，实际 " + actual);
}

// 创建节点并返回其 id（空 id 表示失败）
std::string add_node(Check& check, engine::Graph& graph, const std::string& type,
                     const std::string& name)
{
    std::string error;
    const std::string id = graph.addNode(type, 0.0f, 0.0f, &error);
    expect(check, !id.empty(), name, error);
    return id;
}

int graph_selftest()
{
    using engine::Graph;
    using engine::NodeCategory;
    using engine::NodeRegistry;
    using engine::PortType;

    Check check;
    std::printf("[P1] 图模型 / 节点注册表 / 撤销栈 自检\n");

    // ---------------------------------------------------- 1. 节点注册表 -----
    engine::registerAllNodes();
    NodeRegistry& registry = NodeRegistry::instance();
    expect_eq(check, std::to_string(registry.size()), "14",
              "注册表包含 14 个节点类型"); // M_node_expansion L2：8 + 6 文本处理节点

    const char* expected_types[] = {"TextInput",      "ImageInput",    "PromptTemplate",
                                    "TextMerge",      "ProviderConfig", "LLMGenerate",
                                    "VLMGenerate",    "TextOutput",
                                    "TextSplit",      "TextExtract",  "TextClean",
                                    "TextReplace",    "TextStats",    "TextDiff"};
    for (const char* type : expected_types) {
        expect(check, registry.find(type) != nullptr, std::string("已注册 ") + type);
    }

    struct CategoryCount {
        NodeCategory category;
        std::size_t  count;
        const char*  name;
    };
    const CategoryCount categories[] = {
        {NodeCategory::Input, 2, "输入"}, {NodeCategory::Process, 2, "处理"},
        {NodeCategory::Config, 1, "配置"}, {NodeCategory::Inference, 2, "推理"},
        {NodeCategory::Output, 1, "输出"},
        {NodeCategory::Auxiliary, 6, "辅助功能"}}; // M7：ImagePreview 移除后「输出」只剩 TextOutput
    for (const CategoryCount& item : categories) {
        expect(check, registry.listByCategory(item.category).size() == item.count,
               std::string("分类 ") + item.name + " 节点数 = " + std::to_string(item.count));
    }

    if (const engine::Definition* text_input = registry.find("TextInput")) {
        expect(check,
               text_input->outputs.size() == 1 && text_input->outputs[0].id == "text" &&
                   text_input->outputs[0].type == PortType::Text,
               "TextInput 输出端口 = text:text");
        expect(check, text_input->params.size() == 1 && text_input->params[0].is_required,
               "TextInput 参数 text 为必填");
    }
    if (const engine::Definition* llm = registry.find("LLMGenerate")) {
        expect(check,
               llm->inputs.size() == 2 && llm->inputs[0].id == "prompt" &&
                   llm->inputs[1].type == PortType::Provider,
               "LLMGenerate 输入 = prompt:text + provider:provider");
        bool has_temperature = false;
        bool has_max_tokens  = false;
        for (const engine::Param& param : llm->params) {
            if (param.id == "temperature" && param.min_value && param.max_value) {
                has_temperature = true;
            }
            if (param.id == "max_tokens" && param.min_value && param.max_value) {
                has_max_tokens = true;
            }
        }
        expect(check, has_temperature && has_max_tokens,
               "LLMGenerate 含 temperature / max_tokens 范围");
    }
    if (const engine::Definition* provider = registry.find("ProviderConfig")) {
        bool has_mode = false;
        for (const engine::Param& param : provider->params) {
            if (param.id == "mode" &&
                std::find(param.enum_options.begin(), param.enum_options.end(),
                          std::string("official")) != param.enum_options.end()) {
                has_mode = true;
            }
        }
        expect(check,
               has_mode && provider->outputs.size() == 1 &&
                   provider->outputs[0].type == PortType::Provider,
               "ProviderConfig = provider 输出 + mode 枚举");
    }

    // ---------------------------------------------------- 1.5 M_8nodes_extension L1-a ----
    // VL1-01：推理节点公共参数面 —— N-06 / N-07 参数集一致，对称差集 = {model}（N-06 特有）
    // VL1-02：参数二级分组归属（采样 / 输出 / 高级）
    // VL1-12：网页版忽略参数（top_k / stop / frequency_penalty / presence_penalty / seed）
    //         在注册表里的标注存在（UI 灰色「（网页版忽略）」的依据）
    if (const engine::Definition* llm = registry.find("LLMGenerate")) {
        if (const engine::Definition* vlm = registry.find("VLMGenerate")) {
            std::vector<std::string> llm_ids;
            std::vector<std::string> vlm_ids;
            for (const engine::Param& param : llm->params) {
                llm_ids.push_back(param.id);
            }
            for (const engine::Param& param : vlm->params) {
                vlm_ids.push_back(param.id);
            }
            std::vector<std::string> diff;
            for (const std::string& id : llm_ids) {
                if (std::find(vlm_ids.begin(), vlm_ids.end(), id) == vlm_ids.end()) {
                    diff.push_back(id);
                }
            }
            for (const std::string& id : vlm_ids) {
                if (std::find(llm_ids.begin(), llm_ids.end(), id) == llm_ids.end()) {
                    diff.push_back(id);
                }
            }
            std::string diff_text;
            for (std::size_t i = 0; i < diff.size(); ++i) {
                if (i != 0) {
                    diff_text += ",";
                }
                diff_text += diff[i];
            }
            // N-06 特有 = {model}；N-07 特有 ⊆ {image_order, 图片预处理参数}（L1-b §4.7 / L1-c §4.7）
            const std::vector<std::string> vlm_only = {
                "image_order", "resize_mode", "resize_max_width", "resize_max_height",
                "crop_enabled", "crop_x", "crop_y", "crop_w", "crop_h"};
            const auto id_in = [](const std::vector<std::string>& ids, const std::string& id) {
                return std::find(ids.begin(), ids.end(), id) != ids.end();
            };
            bool n06_only_ok = true;
            for (const std::string& id : llm_ids) {
                if (id_in(vlm_ids, id)) {
                    continue;
                }
                // N-06 特有 = model / response_format（L1-e §4.6）/ n（候选数）
                n06_only_ok = n06_only_ok && (id == "model" || id == "response_format" || id == "n");
            }
            bool n07_only_ok = true;
            for (const std::string& id : vlm_ids) {
                if (id_in(llm_ids, id)) {
                    continue;
                }
                n07_only_ok = n07_only_ok && id_in(vlm_only, id);
            }
            expect(check, n06_only_ok && n07_only_ok,
                   "VL1-01 公共参数面：N-06 特有 = model；N-07 特有 ⊆ 节点专属集（实际差集: " + diff_text + "）");
        }
    }
    const auto group_has = [&check, &registry](const char* node_type, const std::string& group,
                                               const std::vector<std::string>& ids) {
        const engine::Definition* def = registry.find(node_type);
        for (const std::string& id : ids) {
            bool found = false;
            if (def != nullptr) {
                for (const engine::Param& param : def->params) {
                    if (param.id == id) {
                        found = param.group == group;
                        break;
                    }
                }
            }
            expect(check, found,
                   std::string("VL1-02 分组归属：") + node_type + "." + id + " ∈ " + group);
        }
    };
    group_has("LLMGenerate", "sampling",
              {"temperature", "top_p", "top_k", "frequency_penalty", "presence_penalty", "seed"});
    group_has("LLMGenerate", "output", {"system_prompt", "mode", "max_tokens", "stop", "stream"});
    group_has("LLMGenerate", "advanced",
              {"timeout_connect_ms", "timeout_first_byte_ms", "timeout_stream_idle_ms",
               "retry_enabled", "retry_count", "retry_interval_ms"});
    group_has("VLMGenerate", "sampling",
              {"temperature", "top_p", "top_k", "frequency_penalty", "presence_penalty", "seed"});
    group_has("VLMGenerate", "output", {"system_prompt", "mode", "max_tokens", "stop", "stream"});
    {
        // VL1-12：网页版忽略标注依据存在（§3.4 集合 = top_k / stop / frequency_penalty /
        //   presence_penalty / seed / response_format；UI 据此显示灰色「（网页版忽略）」）
        const engine::Definition* llm = registry.find("LLMGenerate");
        const char* ignored_ids[]     = {"top_k", "stop", "frequency_penalty", "presence_penalty",
                                         "seed"};
        for (const char* id : ignored_ids) {
            bool annotated = false;
            if (llm != nullptr) {
                for (const engine::Param& param : llm->params) {
                    if (param.id == id &&
                        param.description.find("网页版忽略") != std::string::npos) {
                        annotated = true;
                        break;
                    }
                }
            }
            expect(check, annotated, std::string("VL1-12 网页版忽略标注：") + id);
        }
    }

    // ---------------------------------------------------- 2. Graph 增删 -----
    Graph graph;
    const std::string text_id = add_node(check, graph, "TextInput", "创建 TextInput 节点");
    expect_eq(check, text_id, "n1", "节点 id = n1");
    if (const engine::Node* node = graph.findNode(text_id)) {
        expect(check,
               node->params.size() == 1 &&
                   node->params[0].value == node->params[0].default_value,
               "新建节点的参数值 = 默认值");
    }

    std::string error;
    const std::string unknown_id = graph.addNode("NotExists", 0.0f, 0.0f, &error);
    expect(check, unknown_id.empty() && !error.empty(), "未注册类型被拒绝", error);

    const std::string llm_id = add_node(check, graph, "LLMGenerate", "创建 LLMGenerate 节点");
    if (!text_id.empty() && !llm_id.empty()) {
        const std::string edge_id = graph.addEdge(text_id, "text", llm_id, "prompt", &error);
        expect(check, !edge_id.empty(), "连接 text -> prompt 成功", error);
        expect_eq(check, edge_id, "e1", "连线 id = e1");

        expect(check, graph.addEdge(text_id, "text", llm_id, "prompt", &error).empty(),
               "重复连线被拒绝", error);

        expect(check, graph.addEdge(text_id, "text", text_id, "text", &error).empty(),
               "自连接被拒绝", error);

        const std::string vlm_id = add_node(check, graph, "VLMGenerate", "创建 VLMGenerate 节点");
        if (!vlm_id.empty()) {
            expect(check,
                   graph.addEdge(text_id, "text", vlm_id, "image", &error).empty() &&
                       error.find("类型不兼容") != std::string::npos,
                   "text -> image 被拒绝（类型校验）", error);

            const std::string tpl_id =
                add_node(check, graph, "PromptTemplate", "创建 PromptTemplate 节点");
            if (!tpl_id.empty()) {
                expect(check, !graph.addEdge(text_id, "text", tpl_id, "vars", &error).empty(),
                       "text -> any 端口允许连接", error);
            }
        }

        const std::string text2_id =
            add_node(check, graph, "TextInput", "创建第二个 TextInput 节点");
        if (!text2_id.empty()) {
            const std::size_t before = graph.edges.size();
            expect(check,
                   !graph.addEdge(text2_id, "text", llm_id, "prompt", &error).empty() &&
                       graph.edges.size() == before,
                   "输入端口已有连接时被替换（边数不变）", error);
            const engine::Edge* into = graph.findEdgeIntoInput(llm_id, "prompt");
            expect(check, into != nullptr && into->from_node == text2_id,
                   "替换后连线来源 = 第二个 TextInput", into != nullptr ? into->from_node : "null");
        }

        graph.removeNode(llm_id);
        bool dangling = false;
        for (const engine::Edge& item : graph.edges) {
            if (item.from_node == llm_id || item.to_node == llm_id) {
                dangling = true;
            }
        }
        expect(check, !dangling, "删除节点时相关连线被一并清理");

        // 直接删除连线（右键菜单「删除连线」/ 工具栏「删除选中」走的正是这条路径）
        Graph edge_graph;
        const std::string edge_a = add_node(check, edge_graph, "TextInput", "删边用例 A");
        const std::string edge_b = add_node(check, edge_graph, "TextOutput", "删边用例 B");
        if (!edge_a.empty() && !edge_b.empty()) {
            std::string edge_error;
            const std::string removed_edge_id =
                edge_graph.addEdge(edge_a, "text", edge_b, "text", &edge_error);
            expect(check, !removed_edge_id.empty(), "删边用例：连线创建成功", edge_error);
            expect(check, edge_graph.removeEdge(removed_edge_id), "removeEdge 删除连线成功");
            expect(check, edge_graph.edges.empty() && edge_graph.nodes.size() == 2,
                   "删边后：连线 0 条、节点保留");
            expect(check, !edge_graph.removeEdge(removed_edge_id), "重复删除同一条连线返回 false");
        }
    }

    // 可变长输入端口允许叠加多条连线（TextMerge.texts）
    {
        Graph merge_graph;
        const std::string merge_id = add_node(check, merge_graph, "TextMerge", "创建 TextMerge 节点");
        const std::string a_id     = add_node(check, merge_graph, "TextInput", "TextMerge 用例 A");
        const std::string b_id     = add_node(check, merge_graph, "TextInput", "TextMerge 用例 B");
        if (!merge_id.empty() && !a_id.empty() && !b_id.empty()) {
            merge_graph.addEdge(a_id, "text", merge_id, "texts", &error);
            merge_graph.addEdge(b_id, "text", merge_id, "texts", &error);
            expect(check, merge_graph.inputConnectionCount(merge_id, "texts") == 2,
                   "可变长输入端口可叠加多条连线");
        }
    }

    // 复制节点
    {
        Graph clone_graph;
        const std::string source_id = add_node(check, clone_graph, "TextInput", "克隆用例源节点");
        if (!source_id.empty()) {
            if (engine::Node* source = clone_graph.findNode(source_id)) {
                source->params[0].value = std::string("原始文本");
            }

            std::string clone_error;
            const std::string copy_id =
                clone_graph.cloneNode(source_id, 24.0f, 24.0f, &clone_error);
            expect(check, !copy_id.empty() && copy_id != source_id, "复制节点得到新 id", clone_error);

            const engine::Node* source = clone_graph.findNode(source_id);
            const engine::Node* copy   = clone_graph.findNode(copy_id);
            if (source != nullptr && copy != nullptr) {
                expect(check, copy->title == source->title + " 副本", "复制节点标题带「副本」");
                expect(check, copy->params[0].text() == "原始文本", "复制节点保留参数值");
                expect(check, copy->x == source->x + 24.0f, "复制节点位置偏移");
            }
        }
    }

    // ------------------------------------------------ 3. 参数校验 -----------
    {
        Graph param_graph;
        const std::string param_text_id =
            add_node(check, param_graph, "TextInput", "参数校验用 TextInput");
        if (engine::Node* text = param_graph.findNode(param_text_id)) {
            std::string reason;
            expect(check, !Graph::validateParam(text->params[0], &reason),
                   "必填文本为空 → 校验失败", reason);
            text->params[0].value = std::string("hello");
            expect(check, Graph::validateParam(text->params[0], &reason), "填写后校验通过", reason);
        }

        const std::string param_llm_id =
            add_node(check, param_graph, "LLMGenerate", "参数校验用 LLMGenerate");
        if (engine::Node* llm = param_graph.findNode(param_llm_id)) {
            engine::Param* temperature = llm->findParam("temperature");
            expect(check, temperature != nullptr, "找到 temperature 参数");
            if (temperature != nullptr) {
                std::string reason;
                temperature->value = 5.0; // 超出 0 ~ 2
                expect(check, !Graph::validateParam(*temperature, &reason),
                       "temperature 超范围 → 校验失败", reason);
                temperature->value = 0.9;
                expect(check, Graph::validateParam(*temperature, &reason),
                       "temperature 合法值 → 校验通过", reason);
            }
        }

        const std::string provider_id =
            add_node(check, param_graph, "ProviderConfig", "参数校验用 ProviderConfig");
        if (engine::Node* provider = param_graph.findNode(provider_id)) {
            engine::Param* mode = provider->findParam("mode");
            expect(check, mode != nullptr, "找到 mode 参数");
            if (mode != nullptr) {
                std::string reason;
                mode->value = std::string("unknown-mode");
                expect(check, !Graph::validateParam(*mode, &reason), "枚举非法值 → 校验失败", reason);
                mode->value = std::string("web");
                expect(check, Graph::validateParam(*mode, &reason), "枚举合法值 → 校验通过", reason);
            }
        }

        const std::string image_id =
            add_node(check, param_graph, "ImageInput", "参数校验用 ImageInput");
        if (engine::Node* image = param_graph.findNode(image_id)) {
            engine::Param* path = image->findParam("path");
            if (path != nullptr) {
                std::string reason;
                path->value = std::string("C:/not-exists/definitely-missing.png");
                expect(check, !Graph::validateParam(*path, &reason),
                       "图片路径不存在 → 校验失败", reason);
            }
        }

        expect(check,
               engine::numericIdOf("n12") == 12 && engine::numericIdOf("e3") == 3 &&
                   engine::numericIdOf("x") == 0,
               "numericIdOf 解析 n12 / e3 / 非法值");
    }

    // ------------------------------------------------ 4. 撤销 / 重做 -------
    {
        Graph current;
        engine::UndoStack undo;
        const std::string first_id = add_node(check, current, "TextInput", "撤销用例：初始节点");
        (void)first_id;

        undo.push(current, "创建第二个节点");
        const std::string second_id =
            add_node(check, current, "TextOutput", "撤销用例：第二个节点");
        (void)second_id;
        expect(check, current.nodes.size() == 2, "操作后节点数 = 2");

        Graph restored;
        std::string label;
        expect(check, undo.undo(current, restored, &label) && restored.nodes.size() == 1,
               "撤销后回到 1 个节点", label);
        expect(check, undo.canRedo(), "撤销后可以重做");
        current = restored;

        expect(check, undo.redo(current, restored, &label) && restored.nodes.size() == 2,
               "重做后回到 2 个节点", label);
        current = restored;
        expect(check, !undo.canRedo(), "重做后重做栈为空");

        Graph snapshot;
        for (int i = 0; i < 60; ++i) {
            undo.push(snapshot, "深度测试");
        }
        expect(check, undo.undoDepth() == engine::UndoStack::kMaxDepth, "撤销栈深度被限制为 50",
               std::to_string(undo.undoDepth()));

        undo.push(current, "新操作");
        expect(check, !undo.canRedo(), "产生新操作后重做分支被丢弃");

        undo.clear();
        expect(check, !undo.canUndo() && !undo.canRedo(), "clear() 清空撤销与重做栈");
    }

    // ------------------------------------------------ 5. 工作流序列化（设计 §4.7）----
    {
        Graph source;
        source.name          = "往返测试";
        source.description   = "round-trip";
        source.viewport_x    = 12.5f;
        source.viewport_y    = -30.0f;
        source.viewport_zoom = 1.5f;

        const std::string ser_a = add_node(check, source, "TextInput", "序列化用例 A");
        const std::string ser_b = add_node(check, source, "LLMGenerate", "序列化用例 B");
        const std::string ser_c = add_node(check, source, "ProviderConfig", "序列化用例 C");
        if (!ser_a.empty() && !ser_b.empty() && !ser_c.empty()) {
            std::string edge_error;
            source.addEdge(ser_a, "text", ser_b, "prompt", &edge_error);
            // 与默认工作流一致：ProviderConfig → LLMGenerate.provider
            source.addEdge(ser_c, "provider", ser_b, "provider", &edge_error);

            if (engine::Node* node_a = source.findNode(ser_a)) {
                node_a->title = "序列化用例 A";
                if (engine::Param* param = node_a->findParam("text")) {
                    param->value = std::string("往返文本");
                }
            }
            if (engine::Node* node_b = source.findNode(ser_b)) {
                if (engine::Param* param = node_b->findParam("temperature")) {
                    param->value = 1.25;
                }
            }
        }

        const nlohmann::json document = engine::graphToJson(source);
        expect(check,
               document.contains("version") && document.contains("name") &&
                   document.contains("nodes") && document.contains("edges") &&
                   document.contains("viewport"),
               "序列化：包含 version / name / nodes / edges / viewport");
        expect(check, document["nodes"].size() == 3 && document["edges"].size() == 2,
               "序列化：节点/连线数量正确",
               std::to_string(document["nodes"].size()) + "/" + std::to_string(document["edges"].size()));

        bool secret_written = false;
        for (const auto& node_json : document["nodes"]) {
            if (node_json.contains("params") && node_json["params"].contains("api_key")) {
                secret_written = true;
            }
        }
        expect(check, !secret_written, "序列化：is_secret 参数（api_key）不写入文件");

        Graph       restored;
        std::string restore_error;
        expect(check, engine::graphFromJson(document, restored, &restore_error),
               "反序列化：成功", restore_error);
        expect(check, restored.nodes.size() == 3 && restored.edges.size() == 2,
               "反序列化：节点/连线数量一致");
        expect(check, restored.name == source.name && restored.description == source.description,
               "反序列化：name / description 一致");
        expect(check, std::fabs(restored.viewport_zoom - source.viewport_zoom) < 0.001f,
               "反序列化：viewport.zoom 一致");

        const engine::Node* restored_a = restored.findNode(ser_a);
        expect(check,
               restored_a != nullptr && restored_a->type == "TextInput" &&
                   restored_a->title == "序列化用例 A",
               "反序列化：节点 id/类型/标题一致");
        const engine::Param* restored_text =
            (restored_a != nullptr) ? restored_a->findParam("text") : nullptr;
        expect(check, restored_text != nullptr && restored_text->text() == "往返文本",
               "反序列化：文本参数一致");

        const engine::Node* restored_b = restored.findNode(ser_b);
        const engine::Param* restored_temperature =
            (restored_b != nullptr) ? restored_b->findParam("temperature") : nullptr;
        expect(check,
               restored_temperature != nullptr &&
                   std::fabs(restored_temperature->number() - 1.25) < 0.001,
               "反序列化：数值参数一致");
        const engine::Edge* into_provider = restored.findEdgeIntoInput(ser_b, "provider");
        expect(check, into_provider != nullptr && into_provider->from_node == ser_c,
               "反序列化：provider 连线一致（ProviderConfig → LLMGenerate.provider）");

        // ---- 文件往返（default.json 走的正是这条路径）----
        const std::filesystem::path temp_file =
            std::filesystem::temp_directory_path() / "aiwrite_workflow_roundtrip.json";
        std::string io_error;
        expect(check, engine::saveWorkflow(source, temp_file, &io_error), "保存到文件成功", io_error);
        Graph from_file;
        expect(check, engine::loadWorkflow(temp_file, from_file, &io_error), "从文件加载成功", io_error);
        expect(check,
               from_file.nodes.size() == source.nodes.size() &&
                   from_file.edges.size() == source.edges.size(),
               "文件往返：节点/连线数量一致");
        std::error_code remove_error;
        std::filesystem::remove(temp_file, remove_error);

        // ---- 非法输入被拒绝 ----
        std::string bad_error;
        Graph       bad_graph;
        expect(check,
               !engine::graphFromJson(nlohmann::json::object(), bad_graph, &bad_error) &&
                   !bad_error.empty(),
               "反序列化：缺少 nodes 时被拒绝");
        expect(check,
               !engine::graphFromJson(nlohmann::json{{"nodes", "not-an-array"}}, bad_graph, &bad_error),
               "反序列化：nodes 类型错误时被拒绝");

        // ---- 参数批量应用（F2 / PD-05：把当前节点参数应用到图中全部同类型节点）----
        {
            Graph             batch_graph;
            const std::string a_id = add_node(check, batch_graph, "LLMGenerate", "批量A");
            const std::string b_id = add_node(check, batch_graph, "LLMGenerate", "批量B");
            const std::string c_id = add_node(check, batch_graph, "LLMGenerate", "批量C");
            const std::string t_id = add_node(check, batch_graph, "TextInput", "批量文本");

            if (engine::Node* a = batch_graph.findNode(a_id)) {
                if (engine::Param* temperature = a->findParam("temperature")) {
                    temperature->value = 1.25;
                }
                if (engine::Param* system_prompt = a->findParam("system_prompt")) {
                    system_prompt->value = std::string("批量系统提示词");
                }
                if (engine::Param* mode = a->findParam("mode")) {
                    mode->value = std::string("official");
                }
                a->x = 100.0f; // 位置不应被批量应用带走
                a->y = 200.0f;
            }

            std::string batch_error;
            const int   affected = batch_graph.copyParamsToSameType(a_id, &batch_error);
            expect(check, affected == 2, "批量应用：更新 2 个同类节点", batch_error);

            const engine::Node* b = batch_graph.findNode(b_id);
            const engine::Node* c = batch_graph.findNode(c_id);
            if (b != nullptr && c != nullptr) {
                const engine::Param* b_temp = b->findParam("temperature");
                const engine::Param* b_sys  = b->findParam("system_prompt");
                const engine::Param* b_mode = b->findParam("mode");
                const engine::Param* c_temp = c->findParam("temperature");
                expect(check,
                       b_temp != nullptr && std::fabs(b_temp->number() - 1.25) < 0.001 &&
                           c_temp != nullptr && std::fabs(c_temp->number() - 1.25) < 0.001,
                       "批量应用：数值参数同步");
                expect(check, b_sys != nullptr && b_sys->text() == "批量系统提示词",
                       "批量应用：文本参数同步");
                expect(check, b_mode != nullptr && b_mode->text() == "official",
                       "批量应用：枚举参数同步");
                expect(check, b->title == c->title && b->x == 0.0f && b->y == 0.0f &&
                                 c->x == 0.0f && c->y == 0.0f,
                       "批量应用：不改动标题与位置");
            }

            const engine::Node* text_node = batch_graph.findNode(t_id);
            if (text_node != nullptr) {
                const engine::Param* text_param = text_node->findParam("text");
                expect(check, text_param != nullptr && text_param->text().empty(),
                       "批量应用：异类节点不受影响");
            }

            // 密钥类参数不复制（避免误扩散），其余同类参数正常复制
            Graph             secret_graph;
            const std::string s1 = add_node(check, secret_graph, "ProviderConfig", "密钥A");
            const std::string s2 = add_node(check, secret_graph, "ProviderConfig", "密钥B");
            if (engine::Node* provider = secret_graph.findNode(s1)) {
                if (engine::Param* key = provider->findParam("api_key")) {
                    key->value = std::string("sk-secret");
                }
                if (engine::Param* base = provider->findParam("api_base")) {
                    base->value = std::string("https://example.test");
                }
            }
            expect(check, secret_graph.copyParamsToSameType(s1, nullptr) == 1,
                   "批量应用：同类 ProviderConfig 命中 1 个");
            const engine::Node* target_provider = secret_graph.findNode(s2);
            if (target_provider != nullptr) {
                const engine::Param* key  = target_provider->findParam("api_key");
                const engine::Param* base = target_provider->findParam("api_base");
                expect(check, key != nullptr && key->text().empty(),
                       "批量应用：is_secret 参数不复制");
                expect(check, base != nullptr && base->text() == "https://example.test",
                       "批量应用：同类非密钥参数已复制");
            }

            // 未知节点 → 返回 0 且写 error（不崩溃、不改图）
            std::string missing_error;
            expect(check,
                   batch_graph.copyParamsToSameType("n404", &missing_error) == 0 &&
                       !missing_error.empty(),
                   "批量应用：未知节点返回 0 并写 error");
        }
    }

    std::printf("=== 图模型自检结果: %d 通过 / %d 失败 ===\n", check.passed, check.failed);
    return check.failed == 0 ? 0 : 1;
}

