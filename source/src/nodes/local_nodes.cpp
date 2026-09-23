#include "nodes/nodes.h"

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

// --- N-06 文本生成（M2 占位：等待 Provider 接线）-----------------------------
json execute_llm_generate(const json& inputs, const json& /*params*/,
                          engine::ExecutionContext& /*ctx*/)
{
    std::string model = "deepseek-chat";
    if (const auto it = inputs.find("provider"); it != inputs.end() && it->is_object()) {
        model = it->value("model", model);
    }
    const std::size_t prompt_size =
        inputs.contains("prompt") ? text_of(inputs["prompt"]).size() : 0;
    throw engine::NodeError("文本生成尚未接线（M4-05 官方 API / M4-06 网页版）：已收到提示词 " +
                            std::to_string(prompt_size) + " 字节，模型 " + model);
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

