#include "nodes/nodes.h"

#include "ai/deepseek_official_provider.h"
#include "utils/credential.h"
#include "ai/deepseek_web_client.h"
#include "utils/log.h"
#include "web/session_store.h"
#include "web/webview_host.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace aiwrite::nodes {
namespace {

using nlohmann::json;

// 端口值文本化（Text/Number/Bool/其它一律转可读文本）
std::string text_of(const json& value)
{
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return {};
    }
    return value.dump();
}

// 支持 \n \t \r \\ 转义（分隔符 / 模板参数的习惯写法）
std::string unescape(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            result += text[i];
            continue;
        }
        switch (text[++i]) {
        case 'n': result += '\n'; break;
        case 't': result += '\t'; break;
        case 'r': result += '\r'; break;
        case '\\': result += '\\'; break;
        default:
            result += '\\';
            result += text[i];
            break;
        }
    }
    return result;
}

} // namespace

// --- N-01 文本输入 -----------------------------------------------------------
json execute_text_input(const json& /*inputs*/, const json& params,
                        engine::ExecutionContext& /*ctx*/)
{
    const std::string text = params.value("text", std::string());
    if (text.empty()) {
        throw engine::NodeError("文本内容为空：请在参数面板填写「文本内容」");
    }
    return text;
}

// --- N-02 图片输入 -----------------------------------------------------------
json execute_image_input(const json& /*inputs*/, const json& params,
                         engine::ExecutionContext& ctx)
{
    const std::string path = params.value("path", std::string());
    std::error_code   ec;
    if (path.empty() || !std::filesystem::exists(std::filesystem::path(path), ec)) {
        throw engine::NodeError("图片文件不存在: " + path);
    }
    ctx.console("[图片输入] " + path);
    return path;
}

// --- N-03 提示词模板 ---------------------------------------------------------
// 变量规则（M2 约定）：
//   1) 命名输入：{端口id} → 该值
//   2) 变长输入 vars：按边顺序绑定 {1}、{2}…；恰好 1 个值时额外提供 {vars}
//   3) 未匹配的 {…} 原样保留（便于用户看出名字写错）
json execute_prompt_template(const json& inputs, const json& params,
                             engine::ExecutionContext& /*ctx*/)
{
    const std::string template_text = unescape(params.value("template", std::string()));

    std::unordered_map<std::string, std::string> variables;
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        if (it.key() == "vars" && it.value().is_array()) {
            const json& list = it.value();
            for (std::size_t index = 0; index < list.size(); ++index) {
                variables[std::to_string(index + 1)] = text_of(list[index]);
            }
            if (list.size() == 1) {
                variables["vars"] = text_of(list.front());
            }
            continue;
        }
        variables[it.key()] = text_of(it.value());
    }

    std::string result;
    result.reserve(template_text.size());
    for (std::size_t i = 0; i < template_text.size(); ++i) {
        if (template_text[i] != '{') {
            result += template_text[i];
            continue;
        }
        const std::size_t end = template_text.find('}', i + 1);
        if (end == std::string::npos) {
            result += template_text.substr(i);
            break;
        }
        const std::string name  = template_text.substr(i + 1, end - i - 1);
        const auto        found = variables.find(name);
        if (found == variables.end()) {
            result += template_text.substr(i, end - i + 1); // 未匹配：原样保留
        }
        else {
            result += found->second;
        }
        i = end;
    }
    return result;
}

// --- N-04 文本合并 -----------------------------------------------------------
json execute_text_merge(const json& inputs, const json& params, engine::ExecutionContext& /*ctx*/)
{
    const std::string separator    = unescape(params.value("separator", std::string("\n")));
    const bool        ignore_empty = params.value("ignore_empty", true);

    std::string result;
    const auto  it = inputs.find("texts");
    if (it != inputs.end() && it->is_array()) {
        bool first = true;
        for (const json& item : *it) {
            const std::string text = text_of(item);
            if (ignore_empty && text.empty()) {
                continue;
            }
            if (!first) {
                result += separator;
            }
            result += text;
            first = false;
        }
    }
    return result;
}

// --- N-05 提供商配置 ---------------------------------------------------------
// 只输出「是否配置了 Key」的布尔值，绝不输出 Key 明文（设计 §8.4）
json execute_provider_config(const json& /*inputs*/, const json& params,
                             engine::ExecutionContext& ctx)
{
    json provider;
    provider["provider"]    = params.value("provider", std::string("deepseek"));
    provider["mode"]        = params.value("mode", std::string("official"));
    provider["api_base"]    = params.value("api_base", std::string("https://api.deepseek.com"));
    provider["model"]       = params.value("model", std::string("deepseek-chat"));
    provider["has_api_key"] = !params.value("api_key", std::string()).empty();

    ctx.console("[提供商配置] " + provider["provider"].get<std::string>() + " / " +
                provider["mode"].get<std::string>() + " / " + provider["model"].get<std::string>() +
                "（API Key " + (provider["has_api_key"].get<bool>() ? "已配置" : "未配置") + "）");
    return provider;
}

// --- N-08 文本输出（M5 起推送到 Output 窗口；本轮进 Console 并透传）----------
json execute_text_output(const json& inputs, const json& /*params*/, engine::ExecutionContext& ctx)
{
    std::string text;
    const auto  it = inputs.find("text");
    if (it != inputs.end()) {
        text = text_of(*it);
    }
    ctx.console("[文本输出] " + text);
    return text;
}

// --- N-09 图片预览（M5）------------------------------------------------------
json execute_image_preview(const json& inputs, const json& /*params*/,
                           engine::ExecutionContext& ctx)
{
    std::string path;
    const auto  it = inputs.find("image");
    if (it != inputs.end()) {
        path = text_of(*it);
    }
    ctx.console("[图片预览] " + path + "（预览窗口在 M5）");
    return path;
}

// --- N-06 文本生成 -----------------------------------------------------------
//  * provider.mode == "web"    → 网页版（M4-06 已接线：Cookie+userToken → PoW → SSE）
//  * provider.mode == "official" → 官方 API（M4-05 待接线，抛错提示可先切网页版）
//  * 输出：单输出端口 → 直接返回生成文本
json execute_llm_generate(const json& inputs, const json& params, engine::ExecutionContext& ctx)
{
    std::string mode  = params.value("mode", std::string("official"));
    std::string model = params.value("model", std::string("deepseek-chat"));
    if (const auto it = inputs.find("provider"); it != inputs.end() && it->is_object()) {
        mode  = it->value("mode", mode);
        model = it->value("model", model);
    }

    const std::string prompt_raw = inputs.contains("prompt") ? text_of(inputs["prompt"]) : "";
    if (prompt_raw.empty()) {
        throw engine::NodeError("文本生成：提示词为空（请连接 PromptTemplate 或直接填写 prompt）");
    }

    // 网页版：把 system_prompt 前置拼进 prompt（网页版请求体没有 system 角色槽位）
    const std::string system_prompt = params.value("system_prompt", std::string());
    std::string       prompt        = prompt_raw;
    if (mode == "web" && !system_prompt.empty()) {
        prompt = system_prompt + "\n\n" + prompt;
    }

    if (mode != "web") {
        // PB-05：官方 API（OpenAI 兼容 /chat/completions）—— 落地“API 使用”
        ai::OfficialChatRequest official;
        official.api_base      = params.value("api_base", std::string("https://api.deepseek.com"));
        official.api_key       = ai::resolve_api_key(params.value("api_key", std::string()));
        official.model         = model;
        official.system_prompt = system_prompt;
        official.prompt        = prompt_raw;
        official.temperature   = params.value("temperature", 0.7);
        official.max_tokens    = params.value("max_tokens", 2048);
        official.top_p         = params.value("top_p", 1.0);
        if (const auto it = inputs.find("provider"); it != inputs.end() && it->is_object()) {
            official.api_base = it->value("api_base", official.api_base);
            // Key 通常在「提供商配置」节点上 → 由 provider 句柄带过来（运行态值）
            // Key 不在 provider 句柄里（设计 §8.4），改为到来源节点读参数
            if (official.api_key.empty() && ctx.graph != nullptr &&
                !ctx.current_node_id.empty()) {
                if (const engine::Edge* edge =
                        ctx.graph->findEdgeIntoInput(ctx.current_node_id, "provider")) {
                    if (const engine::Node* source = ctx.graph->findNode(edge->from_node)) {
                        std::string param_key;
                        std::string ref;
                        if (const engine::Param* key_param = source->findParam("api_key")) {
                            param_key = key_param->text();
                        }
                        if (const engine::Param* ref_param = source->findParam("api_key_ref")) {
                            ref = ref_param->text();
                        }
                        // PB-06：三级优先级 —— 节点参数 → 环境变量 → 凭据库（api_key_ref）
                        const utils::ResolvedSecret resolved = utils::resolve_secret(param_key, ref);
                        official.api_key                    = resolved.key;
                        if (!resolved.key.empty()) {
                            ctx.console("[文本生成] 凭据来源=" + resolved.source + "（长度 " +
                                        std::to_string(resolved.key.size()) + "）");
                            if (resolved.source == "node" && !ref.empty()) {
                                std::string save_error;
                                if (utils::save_credential(ref, resolved.key, &save_error)) {
                                    ctx.console("[文本生成] 已把 API Key 存入凭据库（ref=" + ref +
                                                "），下次无需再填");
                                }
                                else {
                                    ctx.console("[文本生成] 凭据库保存失败：" + save_error);
                                }
                            }
                        }
                    }
                }
            }
        }
        if (official.api_key.empty()) {
            throw engine::NodeError("文本生成（官方 API）缺少 API Key：请在「提供商配置」填写 API Key，"
                                    "或设置环境变量 DEEPSEEK_API_KEY（凭据管理器见 PB-06）");
        }
        ctx.console("[文本生成] 官方 API 请求：" + official.model + "，提示词 " +
                    std::to_string(official.prompt.size()) + " 字节" +
                    (system_prompt.empty() ? "" : "（含系统提示词）"));
        const ai::OfficialChatResult official_result = ai::official_chat(official);
        if (!official_result.ok || official_result.text.empty()) {
            throw engine::NodeError("文本生成（官方 API）失败：" +
                                    (official_result.error.empty() ? std::string("未取到文本")
                                                                   : official_result.error));
        }
        ctx.console("[文本生成] 官方 API 完成：HTTP " + std::to_string(official_result.http_status) +
                    "，输出 " + std::to_string(official_result.text.size()) + " 字节，耗时 " +
                    std::to_string(static_cast<long long>(official_result.elapsed_ms)) + " ms");
        return official_result.text;
    }

    // 网页版：凭证（Cookie + userToken）只在内存；没有则自动引导一次（profile 已登录即可用）
    if (web::SessionStore::instance().snapshot().user_token.empty()) {
        std::string boot_error;
        ctx.console("[文本生成] 正在准备网页版会话（首次约数秒）…");
        if (!web::ensure_session(25000, &boot_error)) {
            throw engine::NodeError("文本生成（网页版）不可用：" + boot_error);
        }
    }
    const web::Session session = web::SessionStore::instance().snapshot();

    ai::WebChatRequest request;
    request.prompt           = prompt;
    request.model_type       = (model == "expert") ? "expert" : "default";
    request.thinking_enabled = (model == "deepseek-reasoner");
    // PB-03-min：把执行器的增量回调接到网页版 SSE（边收边吐 → UI 逐字呈现）
    if (ctx.on_delta) {
        request.on_delta = ctx.on_delta;
    }

    // 网页版不支持采样参数：参数保持默认即无提示；改过则提示一次（官方 API 生效见 PB-05）
    const double temperature = params.value("temperature", 0.7);
    const int    max_tokens  = params.value("max_tokens", 2048);
    const double top_p       = params.value("top_p", 1.0);
    if (temperature != 0.7 || max_tokens != 2048 || top_p != 1.0) {
        ctx.console("[文本生成] 提示：网页版不支持 temperature / max_tokens / top_p，已忽略"
                    "（这些参数仅对官方 API 生效）");
    }

    ctx.console("[文本生成] 网页版请求：模型 " + model + "，提示词 " +
                std::to_string(prompt.size()) + " 字节" +
                (system_prompt.empty() ? "" : "（含系统提示词 " +
                                                 std::to_string(system_prompt.size()) + " 字节）"));
    const ai::WebChatResult result = ai::web_chat(session, request);
    if (!result.ok || result.text.empty()) {
        throw engine::NodeError("文本生成（网页版）失败：" +
                                (result.error.empty() ? std::string("未取到文本") : result.error));
    }

    log::info("[文本生成] 网页版完成：HTTP " + std::to_string(result.http_status) + "，输出 " +
              std::to_string(result.text.size()) + " 字节");
    ctx.console("[文本生成] 完成，输出 " + std::to_string(result.text.size()) + " 字节");
    return result.text;
}

// --- N-07 多模态生成（M2 占位：等待 Provider 接线）---------------------------
json execute_vlm_generate(const json& inputs, const json& /*params*/,
                          engine::ExecutionContext& /*ctx*/)
{
    std::string model = "deepseek-vl";
    if (const auto it = inputs.find("provider"); it != inputs.end() && it->is_object()) {
        model = it->value("model", model);
    }
    throw engine::NodeError("多模态生成尚未接线（M5-02）：模型 " + model);
}

} // namespace aiwrite::nodes

