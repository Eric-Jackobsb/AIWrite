// provider_spec_parse.cpp -- Provider 配置表：解析 / 收尾校验 / 最小兜底表
//
// ARCH-Z1（2026-10-23）：从 provider_spec.cpp 拆出（原 31〜244 行 + 651〜1087 行）。
//   职责：JSON 读取小工具（类型不符 => type_ok=false）· 明文密钥双重检测 · 允许字段集 ·
//         parse_models / parse_endpoints / parse_web / applyEntry / validateFinal /
//         fallbackSpecs（C++ 内联唯一例外，2 条）。
//   official 与 web 走**同一套**加载/合并/校验（守 §1 规则 4）。
//   内部契约：只有 applyEntry / validateFinal / fallbackSpecs 跨 TU 可见
//   （ai/include/provider_spec_detail.h）；其余 parse_* / is_implemented_* 仅本 TU 使用。

#include "provider_spec.h"
#include "provider_spec_detail.h"

#include "string_util.h"   // CLN-05：trim 收敛

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace aiwrite::ai {

using nlohmann::json;
using provider_detail::joinCn;
using provider_detail::startsWith;

namespace {
// ---- JSON 读取小工具（类型不符 → 置 *type_ok=false；调用方据此跳过该条）----
bool read_string(const json& node, const char* key, std::string* out, bool* type_ok)
{
    if (!node.contains(key)) {
        return false;
    }
    if (!node[key].is_string()) {
        *type_ok = false;
        return false;
    }
    *out = node[key].get<std::string>();
    return true;
}

bool read_bool(const json& node, const char* key, bool* out, bool* type_ok)
{
    if (!node.contains(key)) {
        return false;
    }
    if (!node[key].is_boolean()) {
        *type_ok = false;
        return false;
    }
    *out = node[key].get<bool>();
    return true;
}

bool read_int(const json& node, const char* key, int* out, bool* type_ok)
{
    if (!node.contains(key)) {
        return false;
    }
    if (!node[key].is_number_integer()) {
        *type_ok = false;
        return false;
    }
    *out = node[key].get<int>();
    return true;
}

bool read_string_array(const json& node, const char* key, std::vector<std::string>* out,
                       bool* type_ok)
{
    if (!node.contains(key)) {
        return false;
    }
    if (!node[key].is_array()) {
        *type_ok = false;
        return false;
    }
    std::vector<std::string> items;
    for (const json& item : node[key]) {
        if (!item.is_string()) {
            *type_ok = false;
            return false;
        }
        items.push_back(item.get<std::string>());
    }
    *out = std::move(items);
    return true;
}

bool read_string_map(const json& node, const char* key,
                     std::vector<std::pair<std::string, std::string>>* out, bool* type_ok)
{
    if (!node.contains(key)) {
        return false;
    }
    if (!node[key].is_object()) {
        *type_ok = false;
        return false;
    }
    std::vector<std::pair<std::string, std::string>> items;
    for (auto it = node[key].begin(); it != node[key].end(); ++it) {
        if (!it.value().is_string()) {
            *type_ok = false;
            return false;
        }
        items.emplace_back(it.key(), it.value().get<std::string>());
    }
    *out = std::move(items);
    return true;
}

// ---- 明文密钥检测（键名 + 值双重；`token_expr` 是表达式，允许）----
bool looks_like_secret_value(const std::string& value)
{
    const std::string text = utils::trim(value);
    if (startsWith(text, "sk-") && text.size() >= 12) {
        return true;
    }
    if (text.size() >= 32 && text.find('-') == std::string::npos &&
        text.find(' ') == std::string::npos) {
        bool alnum_only = true;
        for (const char ch : text) {
            if (!std::isalnum(static_cast<unsigned char>(ch))) {
                alnum_only = false;
                break;
            }
        }
        if (alnum_only) {
            return true; // 32+ 位纯字母数字（典型密钥形态）
        }
    }
    return false;
}

bool is_secret_key(const std::string& key)
{
    const std::string name = utils::toLower(key);
    if (name == "token_expr" || name == "cookie_names" || name == "notes" ||
        name == "docs_url" || name == "key_ref_default") {
        return false; // 表达式 / Cookie 名清单 / 引用名 / 说明，不是密钥本体
    }
    static const char* kNames[] = {"api_key",  "apikey",      "api-key",  "secret",
                                   "password", "cookie",      "cookies",  "token",
                                   "access_token", "bearer",  "authorization"};
    for (const char* candidate : kNames) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

void scan_secrets(const json& node, const std::string& path, std::vector<std::string>* hits)
{
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string child = path + "." + it.key();
            if (is_secret_key(it.key())) {
                hits->push_back(child);
                continue;
            }
            if (it.value().is_string() && looks_like_secret_value(it.value().get<std::string>())) {
                hits->push_back(child + "（值形似密钥）");
                continue;
            }
            scan_secrets(it.value(), child, hits);
        }
        return;
    }
    if (node.is_array()) {
        for (std::size_t i = 0; i < node.size(); ++i) {
            scan_secrets(node[i], path + "[" + std::to_string(i) + "]", hits);
        }
    }
}

// ---- 允许字段集（未知字段 → 警告；'_' 前缀 = 纯文档字段，忽略且不警告）----
const std::set<std::string>& allowed_entry_fields()
{
    static const std::set<std::string> fields = {
        "id",       "display",          "kind",       "protocol",  "api_base",
        "chat_path", "auth_style",      "auth_header", "extra_headers", "env_names",
        "key_ref_default", "models",    "vision_model_default", "capabilities",
        "limits",   "web",              "verified",   "notes",     "docs_url"};
    return fields;
}

const std::set<std::string>& allowed_model_fields()
{
    static const std::set<std::string> fields = {"id", "label", "vision"};
    return fields;
}

const std::set<std::string>& allowed_caps_fields()
{
    static const std::set<std::string> fields = {"vision", "seed", "system_role", "stream"};
    return fields;
}

const std::set<std::string>& allowed_limits_fields()
{
    static const std::set<std::string> fields = {"image_max_bytes", "connect_timeout_s",
                                                 "read_timeout_s"};
    return fields;
}

const std::set<std::string>& allowed_web_fields()
{
    static const std::set<std::string> fields = {
        "adapter",     "login_url",       "window_title",   "endpoints",  "probe_paths",
        "cookie_names", "token_expr",     "input_selector", "send",       "answer_selector",
        "done_when",   "answer_poll_ms",  "answer_max_polls",
        "attach",      // M7B（B12-C2）：先放开白名单（**解析但运行期不消费**）—— 取值归批 3
        // M8-14：站点图片上传字段（上传模板；见 ai/upload/**）
        "upload_adapter", "attach_selector", "upload_accept"};
    return fields;
}

const std::set<std::string>& allowed_endpoint_fields()
{
    static const std::set<std::string> fields = {"host",       "completion_path",
                                                 "challenge_path", "session_create_path",
                                                 "session_fetch_path", "users_path"};
    return fields;
}

void collect_unknown(const json& node, const std::set<std::string>& allowed,
                     const std::string& where, std::vector<std::string>* warnings)
{
    if (!node.is_object()) {
        return;
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it.key().empty() && it.key()[0] == '_') {
            continue; // 纯文档字段
        }
        if (allowed.find(it.key()) == allowed.end()) {
            warnings->push_back(where + " 未知字段 " + it.key() + "（已忽略）");
        }
    }
}

} // namespace

namespace provider_detail {

bool is_implemented_protocol(const std::string& protocol)
{
    for (const std::string& candidate : implementedProtocols()) {
        if (candidate == protocol) {
            return true;
        }
    }
    return false;
}

bool is_implemented_adapter(const std::string& adapter)
{
    for (const std::string& candidate : implementedWebAdapters()) {
        if (candidate == adapter) {
            return true;
        }
    }
    return false;
}

// 解析 models 数组（元素可为对象或字符串；未知字段 → 警告）
void parse_models(const json& entry, ProviderSpec* out, const std::string& where,
                  std::vector<std::string>* warnings, bool* type_ok)
{
    if (!entry.contains("models")) {
        return;
    }
    if (!entry["models"].is_array()) {
        *type_ok = false;
        return;
    }
    std::vector<ProviderModelSpec> models;
    for (const json& item : entry["models"]) {
        if (item.is_string()) {
            ProviderModelSpec model;
            model.id    = item.get<std::string>();
            model.label = model.id;
            models.push_back(std::move(model));
            continue;
        }
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) {
            *type_ok = false;
            return;
        }
        collect_unknown(item, allowed_model_fields(), where + " models[]", warnings);
        ProviderModelSpec model;
        model.id     = item["id"].get<std::string>();
        model.label  = item.value("label", model.id);
        model.vision = item.value("vision", false);
        models.push_back(std::move(model));
    }
    out->models = std::move(models);
}

// 解析 endpoints（缺失字段 → 沿用内置默认 + 警告；未知字段 → 警告）
void parse_endpoints(const json& web, ProviderSpec* out, const std::string& where,
                     std::vector<std::string>* warnings, bool* type_ok)
{
    if (!web.contains("endpoints")) {
        return;
    }
    if (!web["endpoints"].is_object()) {
        *type_ok = false;
        return;
    }
    const json& node = web["endpoints"];
    collect_unknown(node, allowed_endpoint_fields(), where + " web.endpoints", warnings);
    ProviderWebEndpoints endpoints = out->web.endpoints;
    bool                 ok        = true;
    std::string          value;
    if (read_string(node, "host", &value, &ok)) {
        endpoints.host = value;
    }
    if (read_string(node, "completion_path", &value, &ok)) {
        endpoints.completion_path = value;
    }
    if (read_string(node, "challenge_path", &value, &ok)) {
        endpoints.challenge_path = value;
    }
    if (read_string(node, "session_create_path", &value, &ok)) {
        endpoints.session_create_path = value;
    }
    if (read_string(node, "session_fetch_path", &value, &ok)) {
        endpoints.session_fetch_path = value;
    }
    if (read_string(node, "users_path", &value, &ok)) {
        endpoints.users_path = value;
    }
    if (!ok) {
        *type_ok = false;
        return;
    }
    std::vector<std::string> missing;
    for (const char* key : {"host", "completion_path", "challenge_path", "session_create_path",
                            "session_fetch_path", "users_path"}) {
        if (!node.contains(key)) {
            missing.push_back(key);
        }
    }
    if (!missing.empty()) {
        warnings->push_back(where + " web.endpoints 缺少 " + joinCn(missing) +
                            "：已沿用内置默认值");
    }
    out->web.endpoints = endpoints;
}


// 解析 web 子对象（kind=web）
void parse_web(const json& entry, ProviderSpec* out, const std::string& where,
               std::vector<std::string>* warnings, bool* type_ok)
{
    if (!entry.contains("web")) {
        return;
    }
    if (!entry["web"].is_object()) {
        *type_ok = false;
        return;
    }
    const json& web = entry["web"];
    collect_unknown(web, allowed_web_fields(), where + " web", warnings);
    read_string(web, "adapter", &out->web.adapter, type_ok);
    read_string(web, "login_url", &out->web.login_url, type_ok);
    read_string(web, "window_title", &out->web.window_title, type_ok);
    read_string(web, "token_expr", &out->web.token_expr, type_ok);
    read_string(web, "input_selector", &out->web.input_selector, type_ok);
    read_string(web, "answer_selector", &out->web.answer_selector, type_ok);
    read_string(web, "attach", &out->web.attach, type_ok);  // M7B（B12-C2）：只解析、不消费
    // M8-14：站点图片上传字段（上传模板消费；见 ai/upload/upload_registry.cpp）
    read_string(web, "upload_adapter", &out->web.upload_adapter, type_ok);
    read_string(web, "attach_selector", &out->web.attach_selector, type_ok);
    read_string(web, "upload_accept", &out->web.upload_accept, type_ok);
    read_string_array(web, "probe_paths", &out->web.probe_paths, type_ok);
    read_string_array(web, "cookie_names", &out->web.cookie_names, type_ok);
    bool ok = true;
    read_int(web, "answer_poll_ms", &out->web.answer_poll_ms, &ok);
    read_int(web, "answer_max_polls", &out->web.answer_max_polls, &ok);
    if (!ok) {
        *type_ok = false;
        return;
    }
    if (web.contains("send")) {
        if (!web["send"].is_object()) {
            *type_ok = false;
            return;
        }
        const json& send = web["send"];
        static const std::set<std::string> kSendFields = {"kind", "value", "selector"};
        collect_unknown(send, kSendFields, where + " web.send", warnings);
        read_string(send, "kind", &out->web.send_kind, type_ok);
        std::string value;
        if (read_string(send, "value", &value, type_ok)) {
            out->web.send_value = value;
        }
        if (read_string(send, "selector", &value, type_ok)) {
            out->web.send_value = value;
        }
    }
    if (web.contains("done_when")) {
        if (!web["done_when"].is_object()) {
            *type_ok = false;
            return;
        }
        const json& done = web["done_when"];
        static const std::set<std::string> kDoneFields = {"kind", "selector"};
        collect_unknown(done, kDoneFields, where + " web.done_when", warnings);
        read_string(done, "kind", &out->web.done_kind, type_ok);
        read_string(done, "selector", &out->web.done_selector, type_ok);
    }
    parse_endpoints(web, out, where, warnings, type_ok);
}


// 把一层里的一个条目应用到 spec（已存在 → 字段级覆盖；不存在 → 新建）
// 返回 false = 致命错误（调用方跳过该条；其余条目与已有表不受影响）
bool applyEntry(const json& entry, const std::string& layer, ProviderSpec* out,
                 SpecLoadReport* report)
{
    const std::string id  = utils::trim(entry["id"].get<std::string>());
    const std::string tag = "「" + layer + "」" + id;

    // ---- 明文密钥（键名 / 值双重检测）----
    std::vector<std::string> hits;
    scan_secrets(entry, id, &hits);
    for (const std::string& hit : hits) {
        report->warnings.push_back(tag + " " + hit +
                                   " 形似/就是密钥：已拒绝该字段（表内禁止明文密钥，请用"
                                   " env_names 或 key_ref_default）");
    }
    collect_unknown(entry, allowed_entry_fields(), tag, &report->warnings);

    bool type_ok = true;
    read_string(entry, "display", &out->display, &type_ok);
    read_string(entry, "kind", &out->kind, &type_ok);
    read_string(entry, "protocol", &out->protocol, &type_ok);
    read_string(entry, "api_base", &out->api_base, &type_ok);
    read_string(entry, "chat_path", &out->chat_path, &type_ok);
    read_string(entry, "auth_style", &out->auth_style, &type_ok);
    read_string(entry, "auth_header", &out->auth_header, &type_ok);
    read_string(entry, "key_ref_default", &out->key_ref_default, &type_ok);
    read_string(entry, "vision_model_default", &out->vision_model_default, &type_ok);
    read_string(entry, "notes", &out->notes, &type_ok);
    read_string(entry, "docs_url", &out->docs_url, &type_ok);
    read_string_array(entry, "env_names", &out->env_names, &type_ok);
    read_string_map(entry, "extra_headers", &out->extra_headers, &type_ok);
    read_bool(entry, "verified", &out->verified, &type_ok);
    if (entry.contains("capabilities")) {
        if (!entry["capabilities"].is_object()) {
            type_ok = false;
        }
        else {
            const json& caps = entry["capabilities"];
            collect_unknown(caps, allowed_caps_fields(), tag + " capabilities", &report->warnings);
            read_bool(caps, "vision", &out->caps.vision, &type_ok);
            read_bool(caps, "seed", &out->caps.seed, &type_ok);
            read_bool(caps, "system_role", &out->caps.system_role, &type_ok);
            read_bool(caps, "stream", &out->caps.stream, &type_ok);
        }
    }
    if (entry.contains("limits")) {
        if (!entry["limits"].is_object()) {
            type_ok = false;
        }
        else {
            const json& limits = entry["limits"];
            collect_unknown(limits, allowed_limits_fields(), tag + " limits", &report->warnings);
            if (limits.contains("image_max_bytes")) {
                if (!limits["image_max_bytes"].is_number_integer()) {
                    type_ok = false;
                }
                else {
                    out->limits.image_max_bytes = static_cast<std::size_t>(
                        limits["image_max_bytes"].get<long long>());
                }
            }
            read_int(limits, "connect_timeout_s", &out->limits.connect_timeout_s, &type_ok);
            read_int(limits, "read_timeout_s", &out->limits.read_timeout_s, &type_ok);
        }
    }
    parse_models(entry, out, tag, &report->warnings, &type_ok);
    parse_web(entry, out, tag, &report->warnings, &type_ok);
    if (!type_ok) {
        report->errors.push_back(tag + " 字段类型不符（已跳过该条）");
        return false;
    }

    // ---- 必填（official 与 web 同一套）----
    out->id = id;
    if (utils::trim(out->display).empty() || utils::trim(out->kind).empty() || utils::trim(out->protocol).empty()) {
        report->errors.push_back(tag + " 缺少必填字段（display / kind / protocol；已跳过该条）");
        return false;
    }
    if (out->kind != "official" && out->kind != "web") {
        report->errors.push_back(tag + " kind 非法（应为 official 或 web；已跳过该条）");
        return false;
    }
    if (out->kind == "web" && utils::trim(out->web.login_url).empty()) {
        report->errors.push_back(tag + " kind=web 缺少 web.login_url（登录页；已跳过该条）");
        return false;
    }

    // ---- 默认值补全（保持内置条目与改造前常量一致）----
    if (out->kind == "official") {
        if (utils::trim(out->chat_path).empty()) {
            out->chat_path = defaultChatPath(out->protocol);
        }
        if (utils::trim(out->auth_style).empty()) {
            out->auth_style = defaultAuthStyle(out->protocol);
        }
        if (utils::trim(out->auth_header).empty() && out->auth_style != "none") {
            out->auth_header = defaultAuthHeader(out->auth_style);
        }
    }
    if (utils::trim(out->key_ref_default).empty()) {
        out->key_ref_default = "brain-ai/" + out->id;
    }
    if (out->kind == "web") {
        if (out->web.window_title.empty()) {
            out->web.window_title = "AIwrite · 网页版登录（登录后关闭窗口）";
        }
        if (out->web.probe_paths.empty() && startsWith(out->web.adapter, "builtin:")) {
            out->web.probe_paths = {out->web.endpoints.users_path,
                                    out->web.endpoints.session_fetch_path};
        }
    }
    return true;
}


// 合并后的语义校验：致命项 → 丢弃该条（记 error）；语义提示 → 警告
void validateFinal(std::vector<ProviderSpec>* items, SpecLoadReport* report)
{
    std::vector<std::string> drop_ids;
    for (const ProviderSpec& spec : *items) {
        const std::string tag = "「" + spec.origin + "」" + spec.id;
        if (spec.kind == "official") {
            if (spec.api_base.empty()) {
                report->warnings.push_back(tag + " 未填 api_base：请在节点「API 地址」填写"
                                                  "（或在该条目里补 api_base）");
            }
            if (!is_implemented_protocol(spec.protocol)) {
                report->warnings.push_back(tag + " protocol=" + spec.protocol +
                                           " 本版本尚未实现（已实现：" +
                                           joinCn(implementedProtocols()) +
                                           "）——选中它会在运行时明确报错");
            }
            continue;
        }
        // ---- kind=web ----
        if (spec.web.adapter.empty()) {
            report->errors.push_back(tag + " kind=web 缺少 web.adapter"
                                           "（builtin:deepseek 或 dom；已跳过该条）");
            drop_ids.push_back(spec.id);
            continue;
        }
        if (spec.web.adapter == "dom") {
            std::vector<std::string> missing;
            if (spec.web.input_selector.empty()) {
                missing.push_back("input_selector");
            }
            if (spec.web.send_kind.empty() || spec.web.send_value.empty()) {
                missing.push_back("send");
            }
            if (spec.web.answer_selector.empty()) {
                missing.push_back("answer_selector");
            }
            const bool implemented = is_implemented_adapter(spec.web.adapter);
            if (!missing.empty()) {
                // M_patchB L1 修订（PB2-26）：**登录型站点条目**（生成字段未就绪）→ **警告**，**不**跳过该条
                //  * 登录可用；生成会在运行时**明确报错**（不猜选择器、不静默降级）
                //  * L4（PB2-28 / 不变量 I16）：DOM 站点**不适用**协议探测 → 文案不得写成「协议探测可用」
                report->warnings.push_back(tag + " 登录型站点条目：web 缺 " + joinCn(missing) +
                                           "（生成未就绪 —— 登录可用；协议探测：不适用（DOM 站点））");
            }
            else if (!implemented) {
                report->warnings.push_back(tag + " web.adapter=" + spec.web.adapter +
                                           " 本版本尚未实现（已实现：" +
                                           joinCn(implementedWebAdapters()) +
                                           "）——选中它会在运行时明确报错");
            }
            if (!spec.web.endpoints.host.empty() &&
                spec.web.endpoints.host != defaultDeepseekWebEndpoints().host) {
                report->warnings.push_back(tag + " web.adapter=dom 不支持 web.endpoints"
                                                 "（已忽略该字段）");
            }
            continue;
        }
        if (startsWith(spec.web.adapter, "builtin:")) {
            if (!is_implemented_adapter(spec.web.adapter)) {
                report->warnings.push_back(tag + " web.adapter=" + spec.web.adapter +
                                           " 本版本尚未实现（已实现：" +
                                           joinCn(implementedWebAdapters()) +
                                           "）——选中它会在运行时明确报错");
            }
            std::vector<std::string> unsupported;
            if (!spec.web.input_selector.empty()) {
                unsupported.push_back("input_selector");
            }
            if (!spec.web.answer_selector.empty()) {
                unsupported.push_back("answer_selector");
            }
            if (!spec.web.send_kind.empty()) {
                unsupported.push_back("send");
            }
            if (!unsupported.empty()) {
                report->warnings.push_back(tag + " web.adapter=" + spec.web.adapter +
                                           " 不支持 " + joinCn(unsupported) + "（已忽略）");
            }
            continue;
        }
        report->errors.push_back(tag + " web.adapter=" + spec.web.adapter +
                                 " 非法（应为 builtin:<name> 或 dom；已跳过该条）");
        drop_ids.push_back(spec.id);
    }
    if (drop_ids.empty()) {
        return;
    }
    std::vector<ProviderSpec> kept;
    kept.reserve(items->size());
    for (ProviderSpec& spec : *items) {
        if (std::find(drop_ids.begin(), drop_ids.end(), spec.id) == drop_ids.end()) {
            kept.push_back(std::move(spec));
        }
    }
    *items = std::move(kept);
}

// 最小兜底表（C++ 内联的唯一例外，2 条；仅 ①〜② 都缺失时使用）
std::vector<ProviderSpec> fallbackSpecs()
{
    ProviderSpec deepseek;
    deepseek.id               = "deepseek";
    deepseek.display          = "DeepSeek（官方 API）";
    deepseek.kind             = "official";
    deepseek.protocol         = "openai";
    deepseek.api_base         = "https://api.deepseek.com";
    deepseek.chat_path        = "/chat/completions";
    deepseek.auth_style       = "bearer";
    deepseek.auth_header      = "Authorization";
    deepseek.env_names        = {"DEEPSEEK_API_KEY"};
    deepseek.key_ref_default  = "brain-ai/deepseek";
    deepseek.models           = {{"deepseek-chat", "deepseek-chat（通用）", false},
                                 {"deepseek-reasoner", "deepseek-reasoner（推理）", false}};
    deepseek.caps.vision      = false;
    deepseek.caps.seed        = true;
    deepseek.caps.system_role = true;
    deepseek.verified         = true;
    deepseek.origin           = "fallback";
    deepseek.notes            = "配置表缺失：已使用最小兜底表（与改造前的内置行为一致）";

    ProviderSpec custom;
    custom.id              = "custom-official";
    custom.display         = "自定义（OpenAI 兼容）";
    custom.kind            = "official";
    custom.protocol        = "openai";
    custom.api_base        = "";
    custom.chat_path       = "/chat/completions";
    custom.auth_style      = "bearer";
    custom.auth_header     = "Authorization";
    custom.key_ref_default = "brain-ai/custom";
    custom.caps.vision     = true;
    custom.origin          = "fallback";
    custom.notes           = "配置表缺失：请在「API 地址」手填 OpenAI 兼容地址（含 /v1）";

    return {deepseek, custom};
}

} // namespace provider_detail

} // namespace aiwrite::ai
