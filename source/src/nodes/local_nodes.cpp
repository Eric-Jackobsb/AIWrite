#include "nodes/nodes.h"

#include "ai/provider_spec.h"          // M_patchB L1：配置表（生效提供商 / 站点端点）
#include "engine/provider_resolve.h"   // M_patchB L1：EffectiveProvider

#include "ai/deepseek_official_provider.h"
#include "utils/credential.h"
#include "ai/deepseek_web_client.h"
#include "ai/dom_web_client.h"         // L3（PB2-13）：通用 DOM 站点适配器
#include "utils/log.h"
#include "web/session_store.h"
#include "web/webview_host.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

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

// ---- 推理节点公共：官方 / 兼容 API 的 Key 解析（PB-06 三级优先级 + 首次自动入库）----
//  1) 节点参数 api_key → 2) 环境变量（**表内 env 名按序**；空列表 = DEEPSEEK_API_KEY）
//  3) provider 输入来源「提供商配置」节点的 api_key / api_key_ref（凭据库）
//  * M_patchB L1：env 名与引用名默认值来自配置表（effective），换厂商不再串味
//  * tag：Console 前缀（"[文本生成]" / "[图片理解]"），保证既有日志文案不变
//  * 返回空 key = 未配置（由调用方给出可操作错误）
struct OfficialKeyResolution {
    std::string key;
    std::string source;
    std::string ref;
};

OfficialKeyResolution resolve_official_key(const json& params, const json& /*inputs*/,
                                           const engine::ExecutionContext& ctx,
                                           const std::string& tag,
                                           const engine::EffectiveProvider& effective)
{
    OfficialKeyResolution resolution;
    const std::vector<std::string> env_names =
        effective.spec != nullptr ? effective.spec->env_names : std::vector<std::string>();

    const std::string param_key = params.value("api_key", std::string());
    if (!param_key.empty()) {
        resolution.key    = param_key;
        resolution.source = "node";
    }
    else {
        resolution.key = ai::resolve_api_key(std::string(), env_names);
        if (!resolution.key.empty()) {
            resolution.source = "env";
        }
    }
    if (!resolution.key.empty() || ctx.graph == nullptr || ctx.current_node_id.empty()) {
        return resolution;
    }
    const engine::Edge* edge = ctx.graph->findEdgeIntoInput(ctx.current_node_id, "provider");
    if (edge == nullptr) {
        return resolution;
    }
    const engine::Node* source_node = ctx.graph->findNode(edge->from_node);
    if (source_node == nullptr) {
        return resolution;
    }
    std::string param_key_from_config;
    if (const engine::Param* key_param = source_node->findParam("api_key")) {
        param_key_from_config = key_param->text();
    }
    if (const engine::Param* ref_param = source_node->findParam("api_key_ref")) {
        resolution.ref = ref_param->text();
    }
    if (resolution.ref.empty()) {
        resolution.ref = effective.key_ref; // 表内默认引用名（如 brain-ai/zhipu）
    }
    const utils::ResolvedSecret resolved =
        utils::resolve_secret(param_key_from_config, resolution.ref);
    resolution.key    = resolved.key;
    resolution.source = resolved.source;
    if (resolved.key.empty()) {
        return resolution;
    }
    ctx.console(tag + " 凭据来源=" + resolved.source + "（长度 " +
                std::to_string(resolved.key.size()) + "）");
    if (resolved.source == "node" && !resolution.ref.empty()) {
        std::string save_error;
        if (utils::save_credential(resolution.ref, resolved.key, &save_error)) {
            ctx.console(tag + " 已把 API Key 存入凭据库（ref=" + resolution.ref + "），下次无需再填");
        }
        else {
            ctx.console(tag + " 凭据库保存失败：" + save_error);
        }
    }
    return resolution;
}

// 生效提供商（M_patchB L1 / PB2-05）：由「当前节点 + provider 输入连线」解析，含配置表默认值
engine::EffectiveProvider effective_provider_of(const engine::ExecutionContext& ctx)
{
    if (ctx.graph == nullptr || ctx.current_node_id.empty()) {
        return {};
    }
    const engine::Node* node = ctx.graph->findNode(ctx.current_node_id);
    if (node == nullptr) {
        return {};
    }
    return engine::resolve_effective_provider(*ctx.graph, *node);
}

// Console 一行：让用户看清「到底用了哪家、走哪条路、地址是什么」
void console_provider_line(const engine::ExecutionContext& ctx, const std::string& tag,
                           const engine::EffectiveProvider& effective, const std::string& mode)
{
    std::string text = tag + " 提供商=" + effective.display + "（来源 " +
                       (effective.spec_origin.empty() ? std::string("未在表内")
                                                      : effective.spec_origin) +
                       "）/ 模式=" + mode;
    if (!effective.api_base.empty()) {
        text += " / 地址=" + effective.api_base;
    }
    if (!effective.model.empty()) {
        text += " / 模型=" + effective.model;
    }
    ctx.console(text);
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
    // M_patchB L1（PB2-05）：句柄里输出**按配置表解析后的生效值**（地址 / 模型 / 引用名 / kind）
    const std::string provider_id =
        params.value("provider", std::string("deepseek"));
    const std::string model_custom = params.value("model_custom", std::string());

    std::shared_ptr<const ai::ProviderSpecs> specs = ai::provider_specs_snapshot();
    const ai::ProviderSpec*                  spec  = nullptr;
    if (specs) {
        spec = specs->find(provider_id);
    }

    json provider;
    provider["provider"]    = provider_id;
    provider["mode"]        = params.value("mode", std::string("official"));
    provider["api_base"]    = spec != nullptr
                                  ? ai::resolve_api_base(*spec, params.value("api_base", std::string()))
                                  : params.value("api_base", std::string("https://api.deepseek.com"));
    const std::string model =
        spec != nullptr
            ? ai::resolve_model(*spec, params.value("model", std::string()), model_custom, false)
            : (model_custom.empty() ? params.value("model", std::string("deepseek-chat"))
                                    : model_custom);
    provider["model"]        = model;        // 生效模型名（推理节点据此调用）
    provider["model_custom"] = model_custom; // 原样透出（供界面 / 诊断）
    provider["kind"]         = spec != nullptr ? spec->kind : std::string("official");
    provider["display"]      = spec != nullptr ? spec->display : provider_id;
    provider["spec_origin"]  = spec != nullptr ? spec->origin : std::string();
    provider["key_ref"]      = spec != nullptr
                                   ? ai::resolve_key_ref(*spec, params.value("api_key_ref", std::string()))
                                   : params.value("api_key_ref", std::string());
    provider["has_api_key"]  = !params.value("api_key", std::string()).empty();
    if (spec != nullptr && spec->kind == "web") {
        provider["mode"]        = "web"; // 网页版条目：模式强制一致
        provider["site_login"]  = spec->web.login_url;
        provider["site_adapter"] = spec->web.adapter;
    }

    ctx.console("[提供商配置] " + provider["provider"].get<std::string>() + " / " +
                provider["mode"].get<std::string>() + " / " + model +
                (model_custom.empty() ? "" : "（自定义模型名）") + "（API Key " +
                (provider["has_api_key"].get<bool>() ? "已配置" : "未配置") +
                (spec != nullptr ? "；来源 " + spec->origin : "；不在配置表内") + "）");
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
    // ---- M_patchB L1（PB2-05）：生效提供商来自配置表（默认地址 / 生效模型 / 凭据引用名）----
    const engine::EffectiveProvider effective = effective_provider_of(ctx);
    std::string mode  = params.value("mode", std::string("official"));
    std::string model = params.value("model", std::string("deepseek-chat"));
    if (const auto it = inputs.find("provider"); it != inputs.end() && it->is_object()) {
        mode  = it->value("mode", mode);
        model = it->value("model", model);
    }
    if (effective.resolved) {
        mode  = effective.mode;  // 表优先（web 条目自动锁 web；老工作流参数行为不变）
        model = effective.model; // 表解析后的生效模型名（model_custom 优先，其次节点值，最后表内首选）
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
        // PB-05 + M_patchB L1：官方 / 兼容 API —— 端点·认证·超时·env 名按**配置表**取值
        ai::OfficialChatRequest official;
        if (effective.spec != nullptr) {
            official.options         = ai::provider_options_from(*effective.spec);
            official.image_max_bytes = effective.spec->limits.image_max_bytes;
        }
        official.api_base         = effective.api_base; // 工作流参数优先 → 表默认 → 引擎默认
        official.options.api_base = effective.api_base;
        official.model            = model;
        official.system_prompt    = system_prompt;
        official.prompt           = prompt_raw;
        official.temperature      = params.value("temperature", 0.7);
        official.max_tokens       = params.value("max_tokens", 2048);
        official.top_p            = params.value("top_p", 1.0);
        official.seed             = params.value("seed", 0); // M_rerun：新 seed 重跑
        // PB-06：Key 三级优先级 —— 节点参数 → 环境变量（表内 env 名按序）→ provider 来源节点凭据库
        official.api_key = resolve_official_key(params, inputs, ctx, "[文本生成]", effective).key;
        if (official.api_key.empty()) {
            const std::string env_text =
                effective.spec != nullptr && !effective.spec->env_names.empty()
                    ? effective.spec->env_names.front()
                    : std::string("DEEPSEEK_API_KEY");
            throw engine::NodeError("文本生成（官方 API）缺少 API Key：请在「提供商配置」填写 API Key，"
                                    "或设置环境变量 " + env_text + "（凭据引用名：" +
                                    (effective.key_ref.empty() ? std::string("未设置") : effective.key_ref) +
                                    "）");
        }
        console_provider_line(ctx, "[文本生成]", effective, "official");
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

    // 网页版：凭证（Cookie + userToken）只在内存，且**按站点**归档（PB2-18）
    //  * 站点**只能**来自生效条目自己的 `web` 段（PB2-22 / 决策 D-22② / 不变量 I14：**不**回落内置默认站点）
    //  * 站点与当前登录窗口不一致时，ensure_session 按串行策略重开窗口
    //    （页面内 PoW 求解依赖该站点页面；决策 D-20）
    const std::string site_error = ai::web_site_error(effective.spec);
    if (!site_error.empty()) {
        throw engine::NodeError("文本生成（网页版）不可用：" + site_error);
    }
    const ai::ProviderWebSpec site      = ai::strict_web_spec_for(effective.spec);
    const std::string        site_id    = ai::strict_web_provider_id_for(effective.spec);
    const std::string        site_label = site.login_url;

    // ---- L3（PB2-13）：通用 DOM 适配器（选择器驱动）—— 站点 = 纯数据，程序里无站点专有 C++ ----
    if (site.adapter == "dom") {
        if (ai::web_login_only(effective.spec)) {
            throw engine::NodeError(
                "文本生成（网页版）不可用：站点「" + site_label +
                "」是**登录型条目**（缺生成字段）——可用于登录 / 协议探测；生成需先补齐 "
                "web.input_selector / send / answer_selector（见使用说明 §9）"
                "；可用 --web-adapter-selftest --provider " + site_id + " 诊断选择器");
        }
        const std::string dom_field_warnings = ai::web_site_field_warnings(effective.spec);
        if (!dom_field_warnings.empty()) {
            ctx.console("[文本生成] " + dom_field_warnings); // 决策 D-26：可回落 + 警告
        }
        ctx.console("[文本生成] 站点：" + site_label + "（适配器 dom · 选择器驱动；提示词 " +
                    std::to_string(prompt.size()) + " 字节）");
        ai::DomChatRequest dom_request;
        dom_request.prompt      = prompt;
        dom_request.site        = site;
        dom_request.provider_id = site_id;
        const ai::DomChatResult dom = ai::dom_chat(dom_request);
        if (!dom.ok) {
            throw engine::NodeError("文本生成（网页版 · DOM 适配器）失败：" + dom.error);
        }
        if (!dom.warning.empty()) {
            ctx.console("[文本生成] " + dom.warning); // R13：如实提示（可能仍在生成中）
        }
        ctx.console("[文本生成] DOM 适配器完成：输出 " + std::to_string(dom.text.size()) +
                    " 字节，轮询 " + std::to_string(dom.polls) + " 次 / " +
                    std::to_string(dom.elapsed_ms) + " ms" +
                    (dom.steps.empty() ? "" : ("｜" + dom.steps)));
        return dom.text;
    }

    if (!ai::web_adapter_implemented(site.adapter)) {
        // R12：表里有、程序没实现 → 运行时**明确报错**（登录 / 协议探测仍可用）
        throw engine::NodeError("文本生成（网页版）不可用：站点「" + site_label + "」的适配器 " +
                                site.adapter +
                                " 本版本尚未实现（已实现：builtin:deepseek、dom）"
                                "——该站点可用于登录 / 协议探测");
    }
    const std::string field_warnings = ai::web_site_field_warnings(effective.spec);
    if (!field_warnings.empty()) {
        ctx.console("[文本生成] " + field_warnings); // 决策 D-26：可回落 + 警告
    }
    const web::LoginRequest  boot     = web::boot_login_request(site, site_id);
    const std::string        site_key = web::login_request_site(boot);

    if (!web::SessionStore::instance().has_token(site_key) || !web::window_on_site(site_key)) {
        std::string boot_error;
        ctx.console("[文本生成] 正在准备网页版会话（站点 " + site_label + "；首次约数秒）…");
        if (!web::ensure_session(boot, 25000, &boot_error)) {
            throw engine::NodeError("文本生成（网页版）不可用：" + boot_error);
        }
    }
    const web::Session session = web::SessionStore::instance().snapshot(site_key);

    ai::WebChatRequest request;
    request.prompt           = prompt;
    request.model_type       = (model == "expert") ? "expert" : "default";
    request.thinking_enabled = (model == "deepseek-reasoner");
    // M_patchB L1（PB2-05 / PB2-17）：站点端点来自**生效条目**（PB2-22：站点不可用时上面已抛错，不会用内置默认）
    if (!site.endpoints.host.empty()) {
        request.endpoints = site.endpoints;
    }
    console_provider_line(ctx, "[文本生成]", effective, "web");
    if (effective.resolved && !effective.is_web()) {
        ctx.console("[文本生成] 提示：当前「提供商」不是网页版条目，站点参数用内置默认"
                    "（DeepSeek 网页版）；如需按表配置站点，请在「提供商配置 → 提供商」选择 "
                    "一个网页版条目（如 deepseek-web）");
    }
    else if (effective.is_web()) {
        ctx.console("[文本生成] 站点：" + site_label + "（适配器 " +
                    (site.adapter.empty() ? std::string("builtin") : site.adapter) + "）");
    }
    // PB-03-min：把执行器的增量回调接到网页版 SSE（边收边吐 → UI 逐字呈现）
    if (ctx.on_delta) {
        request.on_delta = ctx.on_delta;
    }

    // 网页版不支持采样参数：参数保持默认即无提示；改过则提示一次（官方 API 生效见 PB-05）
    const double temperature = params.value("temperature", 0.7);
    const int    max_tokens  = params.value("max_tokens", 2048);
    const double top_p       = params.value("top_p", 1.0);
    if (params.value("seed", 0) > 0) {
        ctx.console("[文本生成] 提示：网页版不支持 seed，已忽略（重跑本身即会产生不同结果；"
                    "需要可复现请切官方 API）");
    }
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

// --- N-07 图片理解（M5-02：OpenAI 兼容多模态 /chat/completions）---------------
//  * 输入：prompt + image（可多张）+ provider 句柄
//  * 图片 → data URL（`ai::encode_image_data_url`）→ `messages[].content` 数组
//  * 模型名：provider 句柄的 model（「提供商配置 → 模型（自定义）」优先，见 D-07）
//  * 网页版没有图片入口 → 明确报错并指向 official
json execute_vlm_generate(const json& inputs, const json& params, engine::ExecutionContext& ctx)
{
    // ---- M_patchB L1（PB2-05）：生效提供商来自配置表（能力表决定「支不支持视觉」）----
    const engine::EffectiveProvider effective = effective_provider_of(ctx);
    std::string mode         = "official";
    std::string model        = std::string();
    std::string model_custom = std::string();
    if (const auto it = inputs.find("provider"); it != inputs.end() && it->is_object()) {
        mode         = it->value("mode", mode);
        model        = it->value("model", model);
        model_custom = it->value("model_custom", model_custom);
    }
    if (effective.resolved) {
        mode  = effective.mode;
        model = effective.model;
    }

    const std::string prompt = inputs.contains("prompt") ? text_of(inputs["prompt"]) : std::string();
    if (prompt.empty()) {
        throw engine::NodeError("图片理解：提示词为空（请连接「提示词模板」或「文本输入」到 prompt）");
    }

    // 图片来自 image 输入：可为单值（ImageInput）或数组（多图场景）
    std::vector<std::string> images;
    if (const auto it = inputs.find("image"); it != inputs.end()) {
        if (it->is_array()) {
            for (const json& item : *it) {
                const std::string path = text_of(item);
                if (!path.empty()) {
                    images.push_back(path);
                }
            }
        }
        else {
            const std::string path = text_of(*it);
            if (!path.empty()) {
                images.push_back(path);
            }
        }
    }
    if (images.empty()) {
        throw engine::NodeError("图片理解：未连接图片（请把「图片输入」节点的 image 连到本节点 image 端口）");
    }

    if (mode == "web") {
        // M_patchB L1：文案按**表内显示名**生成（保留「暂不支持网页版」以便既有断言与用户习惯）
        throw engine::NodeError(
            "图片理解暂不支持网页版（" + effective.display +
            "）：请把「提供商配置」改为视觉 API 条目（如 zhipu / siliconflow），"
            "或在「模型（自定义）」填写视觉模型名（如 glm-4v-flash）");
    }
    // 能力门控（表驱动）：表内声明 vision=false 的模型直接拒绝；表外模型放行（交给后端报错）
    if (effective.spec != nullptr) {
        bool declared = false;
        if (!ai::spec_model_supports_vision(*effective.spec, model, &declared)) {
            const std::string suggested = !effective.spec->vision_model_default.empty()
                                              ? effective.spec->vision_model_default
                                              : std::string("（见 --provider-dump）");
            throw engine::NodeError("图片理解：" + effective.display + " 的模型 " + model +
                                    " 不支持视觉（表内视觉模型：" + suggested +
                                    "；可在「模型（自定义）」填写）");
        }
        if (!declared && !model.empty()) {
            ctx.console("[图片理解] 提示：配置表未声明模型 " + model +
                        " 的视觉能力，已按允许调用处理（若失败请改用表内视觉模型）");
        }
    }
    if (model.empty()) {
        throw engine::NodeError("图片理解：未解析到模型名（请连接「提供商配置」节点，"
                                "并在「模型（自定义）」填写视觉模型名，如 glm-4v-flash）");
    }
    if (model == "deepseek-chat" || model == "deepseek-reasoner") {
        ctx.console("[图片理解] 提示：DeepSeek 官方 API 无视觉模型；请把「提供商配置」的 API 地址与"
                    "「模型（自定义）」改为兼容的视觉服务（如智谱 https://open.bigmodel.cn/api/paas/v4 + "
                    "glm-4v-flash）");
    }
    for (const std::string& path : images) {
        std::error_code code;
        if (!std::filesystem::exists(std::filesystem::path(path), code)) {
            throw engine::NodeError("图片理解：图片文件不存在: " + path);
        }
    }

    const std::string system_prompt = params.value("system_prompt", std::string());

    ai::OfficialChatRequest official;
    if (effective.spec != nullptr) {
        official.options         = ai::provider_options_from(*effective.spec);
        official.image_max_bytes = effective.spec->limits.image_max_bytes;
    }
    official.api_base         = effective.api_base; // 工作流参数优先 → 表默认 → 引擎默认
    official.options.api_base = effective.api_base;
    official.model            = model;
    official.system_prompt    = system_prompt;
    official.prompt           = prompt;
    official.images           = images;
    official.temperature      = params.value("temperature", 0.7);
    official.max_tokens       = params.value("max_tokens", 2048);
    official.top_p            = params.value("top_p", 1.0);
    official.api_key = resolve_official_key(params, inputs, ctx, "[图片理解]", effective).key;
    if (official.api_key.empty()) {
        const std::string env_text =
            effective.spec != nullptr && !effective.spec->env_names.empty()
                ? effective.spec->env_names.front()
                : std::string("DEEPSEEK_API_KEY");
        throw engine::NodeError("图片理解（官方/兼容 API）缺少 API Key：请在「提供商配置」填写"
                                " API Key（会自动入库），或设置环境变量 " + env_text +
                                "，或使用凭据库 " +
                                (effective.key_ref.empty() ? std::string("（未设置引用名）")
                                                           : effective.key_ref));
    }
    console_provider_line(ctx, "[图片理解]", effective, "official");

    ctx.console("[图片理解] 请求：" + official.model + "，图片 " + std::to_string(images.size()) +
                " 张，提示词 " + std::to_string(prompt.size()) + " 字节" +
                (system_prompt.empty() ? "" : "（含系统提示词）"));
    const ai::OfficialChatResult result = ai::official_chat(official);
    if (!result.ok || result.text.empty()) {
        throw engine::NodeError("图片理解失败：" +
                                (result.error.empty() ? std::string("未取到文本") : result.error));
    }
    ctx.console("[图片理解] 完成：HTTP " + std::to_string(result.http_status) + "，输出 " +
                std::to_string(result.text.size()) + " 字节，耗时 " +
                std::to_string(static_cast<long long>(result.elapsed_ms)) + " ms");
    return result.text;
}

} // namespace aiwrite::nodes

