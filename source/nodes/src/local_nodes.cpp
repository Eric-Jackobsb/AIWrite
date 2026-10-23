#include "nodes.h"

#include "provider_spec.h"          // M_patchB L1：配置表（生效提供商 / 站点端点）
#include "provider_resolve.h"   // M_patchB L1：EffectiveProvider
#include "inference_provider.h" // M_patchAB §3.1（B2-b · PB2-12）：能力门控（表驱动）

#include "deepseek_official_provider.h"
#include "cancel_token.h"     // M_patchAB §2.1（PB-02）：取消令牌 / 可中断等待
#include "credential.h"
#include "config.h"           // M_patchAB §2.6（PB-08 残项）：timeout.* / error.* 接线
#include "retry_policy.h"     // M_patchAB §2.6：可重试状态 / 退避 / 次数钳制（纯函数）
#include "asset_store.h"         // P7a-04：资源令牌解析（统一资源目录）
#include "paths.h"               // P7a-01/02：image 端口多值 + 多选路径解析
#include "deepseek_web_client.h"
#include "dom_web_client.h"         // L3（PB2-13）：通用 DOM 站点适配器
#include "upload_registry.h" // M8-14：站点上传模板（网页版图片上传 → 再发送）
#include "image_preprocess.h" // M_8nodes_extension L1-c（§4.2）：图片预处理（裁剪 / 缩放）
#include "image_decode.h" // L1-c（§4.2）：目录批量 —— 内容嗅探过滤（sniffImage）
#include "log.h"
#include "session_store.h"
#include "webview_host.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "local_nodes_detail.h"  // ARCH-Z1：公用函数（nodes_detail）

namespace aiwrite::nodes {

using namespace nodes_detail;  // 原匿名命名空间函数（同 TU 直接调用，名字查找不变）
using nlohmann::json;
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
//  P7a-02：`path` 支持**多值**（每行一个路径，见 `paths::split_path_list`）
//   * 单张 → 输出字符串（**旧行为不变**，I19 兼容）
//   * 多张 → 输出字符串数组，经变长 `image` 端口交给「图片理解」
json execute_image_input(const json& /*inputs*/, const json& params,
                         engine::ExecutionContext& ctx)
{
    const std::string              raw  = params.value("path", std::string());
    std::vector<std::string>       list = aiwrite::paths::splitPathList(raw);

    // M_8nodes_extension L1-c（§4.2）：目录批量 —— 扫描目录下全部图片（扩展名 + 内容嗅探）
    const std::string directory = params.value("directory", std::string());
    if (!directory.empty()) {
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(directory), ec) ||
            !std::filesystem::is_directory(std::filesystem::path(directory), ec)) {
            throw engine::NodeError("图片目录不存在: " + directory);
        }
        const std::vector<std::string> collected = collect_directory_images(directory);
        if (collected.empty()) {
            throw engine::NodeError("图片目录下没有可用的图片: " + directory +
                                    "（支持的格式：png / jpg / jpeg / bmp / webp / gif）");
        }
        ctx.console("[图片输入] 目录批量：" + directory + " → " +
                    std::to_string(collected.size()) + " 张");
        list.insert(list.end(), collected.begin(), collected.end());
    }
    if (list.empty()) {
        throw engine::NodeError("图片路径为空：请在参数面板「图片路径」选择图片，"
                                "或「图片目录」选择目录（可多选，每行一个；P7a-02）");
    }

    // P7a-04/06/07：令牌 → 资源目录里的真实路径；**旧绝对路径原样放行**（I19）并给迁移提示
    std::vector<std::string> images;
    std::vector<std::string> problems;
    std::vector<std::string> legacy;
    images.reserve(list.size());
    for (const std::string& value : list) {
        if (aiwrite::asset::needsMigration(value)) {
            legacy.push_back(value);
        }
        bool             missing = false;
        std::string      error;
        const std::string local = aiwrite::asset::toLocalPath(value, &missing, &error);
        if (missing || local.empty()) {
            problems.push_back(error.empty() ? ("图片不可用: " + value) : error);
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(local), ec)) {
            problems.push_back("图片文件不存在: " + local + "（来自 " + value + "；请重新选择图片）");
            continue;
        }
        images.push_back(local);
    }

    if (!problems.empty()) {
        std::string text = "图片不可用（" + std::to_string(problems.size()) + " 项）：";
        for (std::size_t index = 0; index < problems.size(); ++index) {
            if (index != 0) {
                text += "；";
            }
            text += problems[index];
        }
        throw engine::NodeError(text);
    }
    if (!legacy.empty()) {
        ctx.console(aiwrite::asset::migrationHint(legacy.front()));
    }

    // M_8nodes_extension L1-c（§4.2）：图片预处理 —— 读取后、编码前（零参数变化 = 零开销）
    const ai::ImagePreprocessParams pre = preprocess_params_from(params);
    if (pre.resize_mode != "none" || pre.crop_enabled) {
        std::vector<std::string> processed;
        const std::string        pre_error =
            apply_image_preprocess(images, pre, ctx, &processed, "[图片输入]");
        if (!pre_error.empty()) {
            throw engine::NodeError("图片输入预处理失败：" + pre_error);
        }
        images = std::move(processed);
    }

    if (images.size() == 1) {
        ctx.console("[图片输入] " + images.front());
        return images.front();
    }
    ctx.console("[图片输入] " + std::to_string(images.size()) + " 张：" +
                aiwrite::paths::joinPathList(images));
    return images;
}

// --- N-03 提示词模板 ---------------------------------------------------------
// 变量规则（M2 约定）：
//   1) 命名输入：{端口id} → 该值
//   2) 变长输入 vars：按边顺序绑定 {1}、{2}…；恰好 1 个值时额外提供 {vars}
//   3) 未匹配的 {…}：名字「像占位符」（{1}/{var}/{vars}）⇒ **报错**（PW-10 · PQ-13 已拍板，
//      不再原样保留）；否则（JSON 字面量 {…} 等）原样保留，避免误伤
namespace {

// PW-10（PQ-13）：判定 {…} 的名字是否「像占位符」。
//  * 数字（{1}/{2}…）或标识符（{vars}/{端口id}：字母/下划线开头 + 字母数字/下划线）
//  * 其余（含 JSON 字面量 {"a": 1}、含空格 / 冒号等）不算占位符 → 原样保留
bool is_placeholder_name(const std::string& name)
{
    if (name.empty()) {
        return false;
    }
    const bool all_digits =
        std::all_of(name.begin(), name.end(),
                    [](unsigned char ch) { return std::isdigit(ch) != 0; });
    if (all_digits) {
        return true;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (std::isalpha(first) == 0 && first != '_') {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_';
    });
}

} // namespace

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
            // PW-10（PQ-13）：未被任何输入填充的占位符 ⇒ 报错（Console + 节点失败），
            // 不再「原样保留」；仅对「像占位符」的名字报错，JSON 字面量 {…} 不误伤
            if (is_placeholder_name(name)) {
                throw engine::NodeError("模板存在未填充的占位符 {" + name +
                                        "}：变量名未匹配任何输入（变长输入多值只有 {1}/{2}…，"
                                        "单值才提供 {vars}）");
            }
            result += template_text.substr(i, end - i + 1); // 非占位符字面量：原样保留
        }
        else {
            result += found->second;
        }
        i = end;
    }
    return result;
}

// --- N-04 文本合并 -----------------------------------------------------------
//  M_8nodes_extension L1-b（§4.4）：排序 / 前缀后缀 / 带源标签 / 去重 / 空行增强
json execute_text_merge(const json& inputs, const json& params, engine::ExecutionContext& /*ctx*/)
{
    const std::string separator    = unescape(params.value("separator", std::string("\n")));
    const bool        ignore_empty = params.value("ignore_empty", true);
    const std::string sort_mode      = params.value("sort_mode", std::string("edge_order"));
    const std::string prefix         = unescape(params.value("prefix", std::string()));
    const std::string suffix         = unescape(params.value("suffix", std::string()));
    const std::string source_label   = params.value("source_label", std::string("none"));
    const bool        dedup          = params.value("dedup", false);
    const std::string empty_line_mode = params.value("empty_line_mode", std::string("keep"));

    // 1) 收集全部输入段（保序 = 边的插入序）
    //    M_node_expansion L2：递归展平嵌套数组 —— TextSplit.texts（单边 → 数组）直连
    //    变长 texts 端口时，数组被收集为 [ [..] ]，这里展平为文本段
    std::vector<std::string> items;
    const auto               it = inputs.find("texts");
    if (it != inputs.end() && it->is_array()) {
        std::function<void(const json&)> flatten = [&](const json& value) {
            if (value.is_array()) {
                for (const json& child : value) {
                    flatten(child);
                }
                return;
            }
            items.push_back(text_of(value));
        };
        flatten(*it);
    }

    // 2) 排序：by_name = 内容字典序（stable —— 等值段保持相对顺序）
    if (sort_mode == "by_name") {
        std::stable_sort(items.begin(), items.end());
    }

    // 3) 去重：保序去重（保留首次出现的那一份）
    if (dedup) {
        std::vector<std::string> unique;
        for (const std::string& item : items) {
            if (std::find(unique.begin(), unique.end(), item) == unique.end()) {
                unique.push_back(item);
            }
        }
        items = std::move(unique);
    }

    // 4) 空行增强（在 ignore_empty 之上叠加）：
    //    remove = 移除全部空段；collapse = 连续空段压缩为一个；keep = 原样（受 ignore_empty 控制）
    std::vector<std::string> filtered;
    if (empty_line_mode == "remove") {
        for (const std::string& item : items) {
            if (!item.empty()) {
                filtered.push_back(item);
            }
        }
    }
    else if (empty_line_mode == "collapse") {
        bool prev_empty = false;
        for (const std::string& item : items) {
            if (item.empty()) {
                if (prev_empty) {
                    continue; // 连续空段：只保留第一个
                }
                prev_empty = true;
            }
            else {
                prev_empty = false;
            }
            filtered.push_back(item);
        }
    }
    else { // keep
        filtered = items;
    }

    // 5) 输出：分隔符连接 + 每段前缀 / 后缀 / 带源标签
    //    * 空段跳过（ignore_empty）仅作用于 keep 模式；collapse / remove 已在上游处理空段
    std::string result;
    bool        first = true;
    for (std::size_t index = 0; index < filtered.size(); ++index) {
        const std::string& text = filtered[index];
        if (ignore_empty && text.empty() && empty_line_mode == "keep") {
            continue;
        }
        if (!first) {
            result += separator;
        }
        if (source_label == "[nX]") {
            result += "[" + std::to_string(index + 1) + "] ";
        }
        result += prefix;
        result += text;
        result += suffix;
        first = false;
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

    std::shared_ptr<const ai::ProviderSpecs> specs = ai::providerSpecsSnapshot();
    const ai::ProviderSpec*                  spec  = nullptr;
    if (specs) {
        spec = specs->find(provider_id);
    }

    json provider;
    provider["provider"]    = provider_id;
    provider["mode"]        = params.value("mode", std::string("official"));
    provider["api_base"]    = spec != nullptr
                                  ? ai::resolveApiBase(*spec, params.value("api_base", std::string()))
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
                                   ? ai::resolveKeyRef(*spec, params.value("api_key_ref", std::string()))
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

// --- N-06 文本生成 -----------------------------------------------------------
//  * provider.mode == "web"    → 网页版（M4-06 已接线：Cookie+userToken → PoW → SSE）
//  * provider.mode == "official" → 官方 API（M4-05 待接线，抛错提示可先切网页版）
//  * 输出：单输出端口 → 直接返回生成文本
} // namespace aiwrite::nodes
