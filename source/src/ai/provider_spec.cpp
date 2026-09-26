#include "ai/provider_spec.h"

#include "utils/log.h"
#include "utils/paths.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

namespace aiwrite::ai {
namespace {

using nlohmann::json;

constexpr int kSchemaVersion = 1;

std::string trim(const std::string& text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool starts_with(const std::string& text, const std::string& prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

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
    const std::string text = trim(value);
    if (starts_with(text, "sk-") && text.size() >= 12) {
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
    const std::string name = lower(key);
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
        "done_when",   "answer_poll_ms",  "answer_max_polls"};
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

const ProviderWebEndpoints& default_deepseek_web_endpoints()
{
    static const ProviderWebEndpoints endpoints; // 与改造前常量逐字一致
    return endpoints;
}

// M_patchB L1 续（PB2-17）：生效条目 → 网页版站点参数（web 条目用自己；否则回落表内第一个 web 条目）
ProviderWebSpec web_spec_for(const ProviderSpec* spec, const ProviderSpecs* table)
{
    if (spec != nullptr && spec->kind == "web") {
        return spec->web;
    }
    if (table == nullptr) {
        return ProviderWebSpec{};
    }
    for (const ProviderSpec& item : table->items) {
        if (item.kind == "web") {
            return item.web; // 内置表第一个 web 条目 = deepseek-web（保持改造前行为）
        }
    }
    return ProviderWebSpec{};
}

std::string web_provider_id_for(const ProviderSpec* spec, const ProviderSpecs* table)
{
    if (spec != nullptr && spec->kind == "web") {
        return spec->id;
    }
    if (table == nullptr) {
        return {};
    }
    for (const ProviderSpec& item : table->items) {
        if (item.kind == "web") {
            return item.id;
        }
    }
    return {};
}

// ---- M_patchB L1 修订（PB2-22 / D-22② / I14）：**严格**站点解析（不回落）----
ProviderWebSpec strict_web_spec_for(const ProviderSpec* spec)
{
    if (spec == nullptr || spec->kind != "web") {
        return {}; // 非网页版条目**没有**站点（绝不回落到「表内第一个 web 条目」）
    }
    return spec->web;
}

std::string strict_web_provider_id_for(const ProviderSpec* spec)
{
    if (spec == nullptr || spec->kind != "web") {
        return {};
    }
    return spec->id;
}

namespace {

// 统一的三条可操作引导（界面 / 运行前校验 / 运行期文案**逐字一致**）
std::string web_site_guidance()
{
    return "；可选：① 把「提供商」改为网页版条目（如 deepseek-web）；"
           "② 新建网页版站点条目（放进 ~/.brain-ai/providers.d/，格式见使用说明 §9 / M_patchB 附录 D）；"
           "③ 把「模式」改回 official";
}

// 字段名 / Cookie 名清单拼接（界面与警告文案共用）
std::string join_names(const std::vector<std::string>& items)
{
    std::string joined;
    for (const std::string& item : items) {
        if (!joined.empty()) {
            joined += "、";
        }
        joined += item;
    }
    return joined;
}

} // namespace

std::string web_site_error(const ProviderSpec* spec)
{
    if (spec == nullptr) {
        return "配置表里没有该提供商条目，无法确定网页版站点" + web_site_guidance();
    }
    const std::string display = spec->display.empty() ? spec->id : spec->display;
    if (spec->kind != "web") {
        return "「" + display + "」不是网页版条目（没有网页版站点，**不会**回落到内置默认站点）" +
               web_site_guidance();
    }
    if (spec->web.login_url.empty()) {
        return "「" + display +
               "」缺少 web.login_url（站点登录页）——无法确定要打开哪个站点"
               "（**不会**回落到内置默认站点）" +
               web_site_guidance();
    }
    return {};
}

std::string web_site_field_warnings(const ProviderSpec* spec)
{
    if (spec == nullptr || spec->kind != "web") {
        return {};
    }
    std::vector<std::string> login_missing; // 影响「自动探测凭证 / 展示」
    std::vector<std::string> gen_missing;   // 生成字段（未就绪）
    if (spec->web.window_title.empty()) {
        login_missing.push_back("window_title");
    }
    if (spec->web.probe_paths.empty()) {
        login_missing.push_back("probe_paths");
    }
    if (spec->web.token_expr.empty()) {
        login_missing.push_back("token_expr");
    }
    if (spec->web.cookie_names.empty()) {
        login_missing.push_back("cookie_names");
    }
    if (spec->web.input_selector.empty()) {
        gen_missing.push_back("input_selector");
    }
    if (spec->web.send_kind.empty() || spec->web.send_value.empty()) {
        gen_missing.push_back("send");
    }
    if (spec->web.answer_selector.empty()) {
        gen_missing.push_back("answer_selector");
    }
    std::string text;
    if (!login_missing.empty()) {
        // M_patchB L4（PB2-27④ / 不变量 I16）：DOM 站点**不适用**协议探测 →
        // 不得写成「无法自动探测凭证」（那是 DeepSeek 协议站点的语义，会被读作「登录失败」）
        if (!probe_is_applicable(spec->web)) {
            text = "条目缺 " + join_names(login_missing) +
                   "：**协议探测不适用**（DOM 站点：登录态由浏览器 profile 维持，"
                   "登录 / 生成不受影响；探测仅做只读诊断）";
        }
        else {
            // 决策 D-26 / PB2-26：这些字段**可以**回落默认值，但对非 DeepSeek 站点是**误导** → 点明后果
            text = "条目缺 " + join_names(login_missing) +
                   "：该站点**无法自动探测凭证**（登录仍可用）";
        }
    }
    if (!gen_missing.empty()) {
        if (!text.empty()) {
            text += "；";
        }
        text += "生成未就绪（缺 " + join_names(gen_missing) + "）";
    }
    return text;
}

// M_patchB L1 修订（PB2-26）：**登录型站点条目** —— 生成字段（选择器 / 发送 / 取答案）未就绪
//  * 登录 / 协议探测**可用**；生成会在运行时因「适配器未实现 / 生成字段未就绪」**明确报错**
//  * 不新增 schema 字段：L3 落地后把字段补齐即**自动**变为就绪
bool web_login_only(const ProviderSpec* spec)
{
    if (spec == nullptr || spec->kind != "web" || spec->web.adapter != "dom") {
        return false;
    }
    return spec->web.input_selector.empty() || spec->web.send_kind.empty() ||
           spec->web.send_value.empty() || spec->web.answer_selector.empty();
}

// ---- M_patchB L4（PB2-27）：站点无关的登录态判定 / 显示条件 / 探测适用性（纯函数）----
// 说明：三条都**不含**任何 DeepSeek 专有物（不读 userToken / ds_session_id / /api/v0/*，不变量 I15）
WebSessionVerdict web_session_state(const ProviderSpec* spec, const WebSessionEvidence& evidence)
{
    WebSessionVerdict verdict;
    if (spec == nullptr || spec->kind != "web") {
        verdict.state  = WebSessionState::unknown;
        verdict.reason = "该条目不是网页版条目：没有站点登录态";
        return verdict;
    }

    // ① 条目声明了 cookie_names → 命中任一即「已登录」（D-27 ①，站点无关证据优先）
    if (!spec->web.cookie_names.empty()) {
        for (const std::string& name : evidence.cookie_names) {
            for (const std::string& wanted : spec->web.cookie_names) {
                if (name == wanted) {
                    verdict.state  = WebSessionState::logged_in;
                    verdict.reason = "该站点 Cookie「" + name + "」已就位（判据：条目 cookie_names 命中）";
                    return verdict;
                }
            }
        }
        if (!evidence.cookies_known) {
            verdict.state  = WebSessionState::unknown;
            verdict.reason = "尚未读取该站点 Cookie（点「打开登录窗口」，或直接点运行由程序复读）";
            return verdict;
        }
        verdict.state  = WebSessionState::logged_out;
        verdict.reason = "该站点 Cookie 里没有 " + join_names(spec->web.cookie_names) +
                         "（未登录，或会话已失效）";
        return verdict;
    }

    // ② 未配 cookie_names → 该 origin 只要有 Cookie 即视为已登录（D-27「并集」）
    if (evidence.cookie_count > 0) {
        verdict.state  = WebSessionState::logged_in;
        verdict.reason = "该站点已有 " + std::to_string(evidence.cookie_count) +
                         " 条 Cookie（判据：该 origin Cookie 非空；条目未配 cookie_names）";
        return verdict;
    }
    if (evidence.cookies_known) {
        verdict.state  = WebSessionState::logged_out;
        verdict.reason = "该站点没有 Cookie（未登录）";
        return verdict;
    }
    verdict.state  = WebSessionState::unknown;
    verdict.reason = "尚未读取该站点 Cookie（点「打开登录窗口」，或直接点运行由程序复读）";
    return verdict;
}

std::string web_session_state_label(WebSessionState state)
{
    switch (state) {
    case WebSessionState::logged_in:
        return "已登录";
    case WebSessionState::logged_out:
        return "未登录";
    default:
        return "未确认";
    }
}

bool web_shows_user_token(const ProviderSpec* spec)
{
    return spec != nullptr && spec->kind == "web" && !spec->web.token_expr.empty();
}

bool probe_is_applicable(const ProviderWebSpec& web)
{
    if (web.adapter.empty() || starts_with(web.adapter, "builtin:")) {
        return true; // 内置适配器：协议探测（站点端点 / PoW / token）就是为它设计的
    }
    // 非内置（dom 等）：只有**显式**配了探测字段才适用（endpoints 对 dom 无意义，加载期已警告）
    return !web.probe_paths.empty() || !web.token_expr.empty();
}

bool probe_is_applicable(const ProviderSpec* spec)
{
    return spec != nullptr && spec->kind == "web" && probe_is_applicable(spec->web);
}


bool web_adapter_implemented(const std::string& adapter)
{
    if (adapter.empty()) {
        return true; // 空 = 用内置默认适配器行为
    }
    for (const std::string& candidate : implemented_web_adapters()) {
        if (candidate == adapter) {
            return true;
        }
    }
    return false;
}

const ProviderModelSpec* ProviderSpec::find_model(const std::string& model_id) const
{
    for (const ProviderModelSpec& model : models) {
        if (model.id == model_id) {
            return &model;
        }
    }
    return nullptr;
}

std::string default_chat_path(const std::string& protocol)
{
    if (protocol == "anthropic") {
        return "/v1/messages";
    }
    if (protocol == "gemini") {
        return "/v1beta/models/{model}:generateContent";
    }
    return "/chat/completions";
}

std::string default_auth_style(const std::string& protocol)
{
    if (protocol == "anthropic") {
        return "x-api-key";
    }
    if (protocol == "gemini") {
        return "query";
    }
    return "bearer";
}

std::string default_auth_header(const std::string& auth_style)
{
    if (auth_style == "api-key") {
        return "api-key";
    }
    if (auth_style == "x-api-key") {
        return "x-api-key";
    }
    if (auth_style == "query") {
        return "key";
    }
    if (auth_style == "none") {
        return {};
    }
    return "Authorization";
}

std::vector<std::string> implemented_protocols()
{
    // L1 已接线：openai 兼容（含智谱 / 硅基流动 / Ollama / 任意用户条目）
    //            + 内置网页版适配器 deepseek-web（现有 web_chat/PoW）
    // L3（PB2-13）：通用 DOM 适配器（选择器驱动）→ protocol=dom 也已实现
    return {"openai", "deepseek-web", "dom"};
}

std::vector<std::string> implemented_web_adapters()
{
    // L3（PB2-13）：通用 DOM 适配器（选择器驱动）已实现（站点=纯数据；缺选择器 → 登录型条目）
    return {"builtin:deepseek", "dom"};
}

std::string protocol_display(const std::string& protocol)
{
    if (protocol == "openai") {
        return "OpenAI 兼容";
    }
    if (protocol == "anthropic") {
        return "Anthropic Messages";
    }
    if (protocol == "gemini") {
        return "Gemini generateContent";
    }
    if (protocol == "dom") {
        return "通用 DOM 适配器";
    }
    if (protocol == "deepseek-web") {
        return "DeepSeek 网页版（内置）";
    }
    return protocol;
}

std::string SpecLoadReport::summary() const
{
    std::string text = "配置表：" + std::to_string(files.size()) + " 层";
    if (fallback_used) {
        text += "（最小兜底）";
    }
    if (!errors.empty()) {
        text += "，错误 " + std::to_string(errors.size());
    }
    if (!warnings.empty()) {
        text += "，警告 " + std::to_string(warnings.size());
    }
    return text;
}

std::string SpecLoadReport::error_of(const std::string& id) const
{
    for (const std::string& error : errors) {
        if (error.find(id) != std::string::npos) {
            return error;
        }
    }
    return {};
}

const ProviderSpec* ProviderSpecs::find(const std::string& id) const
{
    for (const ProviderSpec& spec : items) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

std::vector<std::string> ProviderSpecs::ids(const std::string& kind) const
{
    std::vector<std::string> result;
    result.reserve(items.size());
    for (const ProviderSpec& spec : items) {
        if (kind.empty() || spec.kind == kind) {
            result.push_back(spec.id);
        }
    }
    return result;
}

std::size_t ProviderSpecs::count_kind(const std::string& kind) const
{
    return ids(kind).size();
}


// ============================================================================
//  解析 / 合并 / 校验（PB2-01；official 与 web **同一套**）
// ============================================================================
namespace {

bool is_implemented_protocol(const std::string& protocol)
{
    for (const std::string& candidate : implemented_protocols()) {
        if (candidate == protocol) {
            return true;
        }
    }
    return false;
}

bool is_implemented_adapter(const std::string& adapter)
{
    for (const std::string& candidate : implemented_web_adapters()) {
        if (candidate == adapter) {
            return true;
        }
    }
    return false;
}

std::string join_list(const std::vector<std::string>& items)
{
    std::string text;
    for (const std::string& item : items) {
        if (!text.empty()) {
            text += "、";
        }
        text += item;
    }
    return text;
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
        warnings->push_back(where + " web.endpoints 缺少 " + join_list(missing) +
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
bool apply_entry(const json& entry, const std::string& layer, ProviderSpec* out,
                 SpecLoadReport* report)
{
    const std::string id  = trim(entry["id"].get<std::string>());
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
    if (trim(out->display).empty() || trim(out->kind).empty() || trim(out->protocol).empty()) {
        report->errors.push_back(tag + " 缺少必填字段（display / kind / protocol；已跳过该条）");
        return false;
    }
    if (out->kind != "official" && out->kind != "web") {
        report->errors.push_back(tag + " kind 非法（应为 official 或 web；已跳过该条）");
        return false;
    }
    if (out->kind == "web" && trim(out->web.login_url).empty()) {
        report->errors.push_back(tag + " kind=web 缺少 web.login_url（登录页；已跳过该条）");
        return false;
    }

    // ---- 默认值补全（保持内置条目与改造前常量一致）----
    if (out->kind == "official") {
        if (trim(out->chat_path).empty()) {
            out->chat_path = default_chat_path(out->protocol);
        }
        if (trim(out->auth_style).empty()) {
            out->auth_style = default_auth_style(out->protocol);
        }
        if (trim(out->auth_header).empty() && out->auth_style != "none") {
            out->auth_header = default_auth_header(out->auth_style);
        }
    }
    if (trim(out->key_ref_default).empty()) {
        out->key_ref_default = "brain-ai/" + out->id;
    }
    if (out->kind == "web") {
        if (out->web.window_title.empty()) {
            out->web.window_title = "AIwrite · 网页版登录（登录后关闭窗口）";
        }
        if (out->web.probe_paths.empty() && starts_with(out->web.adapter, "builtin:")) {
            out->web.probe_paths = {out->web.endpoints.users_path,
                                    out->web.endpoints.session_fetch_path};
        }
    }
    return true;
}


// 合并后的语义校验：致命项 → 丢弃该条（记 error）；语义提示 → 警告
void validate_final(std::vector<ProviderSpec>* items, SpecLoadReport* report)
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
                                           join_list(implemented_protocols()) +
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
                report->warnings.push_back(tag + " 登录型站点条目：web 缺 " + join_list(missing) +
                                           "（生成未就绪 —— 登录可用；协议探测：不适用（DOM 站点））");
            }
            else if (!implemented) {
                report->warnings.push_back(tag + " web.adapter=" + spec.web.adapter +
                                           " 本版本尚未实现（已实现：" +
                                           join_list(implemented_web_adapters()) +
                                           "）——选中它会在运行时明确报错");
            }
            if (!spec.web.endpoints.host.empty() &&
                spec.web.endpoints.host != default_deepseek_web_endpoints().host) {
                report->warnings.push_back(tag + " web.adapter=dom 不支持 web.endpoints"
                                                 "（已忽略该字段）");
            }
            continue;
        }
        if (starts_with(spec.web.adapter, "builtin:")) {
            if (!is_implemented_adapter(spec.web.adapter)) {
                report->warnings.push_back(tag + " web.adapter=" + spec.web.adapter +
                                           " 本版本尚未实现（已实现：" +
                                           join_list(implemented_web_adapters()) +
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
                                           " 不支持 " + join_list(unsupported) + "（已忽略）");
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
std::vector<ProviderSpec> fallback_specs()
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

} // namespace


// ============================================================================
//  公开接口：合并 / 加载 / 缓存（PB2-01 / PB2-02 / PB2-03）
// ============================================================================

ProviderSpecs merge_provider_specs(
    const std::vector<std::pair<std::string, std::string>>& layers, SpecLoadReport* report)
{
    SpecLoadReport  local;
    SpecLoadReport& rep = report != nullptr ? *report : local;
    rep                 = SpecLoadReport{};

    std::vector<ProviderSpec>          items;
    std::map<std::string, std::size_t> index;

    for (const auto& layer : layers) {
        const std::string& label = layer.first;
        if (trim(layer.second).empty()) {
            continue; // 文件不存在（loader 不传）或空文件
        }
        rep.files.push_back(label);

        json root;
        try {
            root = json::parse(layer.second);
        }
        catch (const std::exception& ex) {
            rep.errors.push_back("「" + label + "」JSON 解析失败：" + ex.what() + "（已忽略该层）");
            continue;
        }
        if (!root.is_object()) {
            rep.errors.push_back("「" + label + "」顶层应为对象（已忽略该层）");
            continue;
        }
        if (!root.contains("schema_version") || !root["schema_version"].is_number_integer()) {
            rep.errors.push_back("「" + label + "」缺少 schema_version（应为整数 1；已忽略该层）");
            continue;
        }
        if (root["schema_version"].get<int>() != kSchemaVersion) {
            rep.errors.push_back("「" + label + "」schema_version=" +
                                 std::to_string(root["schema_version"].get<int>()) +
                                 " 不受支持（本程序支持 " + std::to_string(kSchemaVersion) +
                                 "；已忽略该层）");
            continue;
        }
        const bool user_layer = starts_with(label, "user");
        if (root.contains("replace_all")) {
            if (!root["replace_all"].is_boolean()) {
                rep.warnings.push_back("「" + label + "」replace_all 类型不符（已忽略）");
            }
            else if (root["replace_all"].get<bool>()) {
                if (!user_layer) {
                    rep.warnings.push_back("「" + label + "」内置层不支持 replace_all（已忽略）");
                }
                else {
                    items.clear();
                    index.clear();
                    rep.warnings.push_back("「" + label + "」replace_all=true：已忽略下层条目");
                }
            }
        }
        if (!root.contains("providers") || !root["providers"].is_array()) {
            rep.errors.push_back("「" + label + "」缺少 providers 数组（已忽略该层）");
            continue;
        }
        for (const json& entry : root["providers"]) {
            if (!entry.is_object()) {
                rep.errors.push_back("「" + label + "」providers[] 含非对象条目（已跳过）");
                continue;
            }
            if (!entry.contains("id") || !entry["id"].is_string() ||
                trim(entry["id"].get<std::string>()).empty()) {
                rep.errors.push_back("「" + label + "」providers[] 条目缺少 id（已跳过）");
                continue;
            }
            const std::string id = trim(entry["id"].get<std::string>());

            ProviderSpec working;
            const auto   found = index.find(id);
            if (found != index.end()) {
                working = items[found->second]; // 字段级合并：只覆盖本层出现的字段
            }
            if (!apply_entry(entry, label, &working, &rep)) {
                continue; // 该条被跳过；已有条目不受影响
            }
            working.id     = id;
            working.origin = label;
            if (found == index.end()) {
                index[id] = items.size();
                items.push_back(std::move(working));
            }
            else {
                items[found->second] = std::move(working);
            }
        }
    }

    validate_final(&items, &rep);
    if (items.empty()) {
        items             = fallback_specs();
        rep.fallback_used = true;
        rep.warnings.push_back(
            "配置表缺失或不可用：已使用最小兜底表（custom-official + deepseek）");
    }
    rep.ok = rep.errors.empty();

    ProviderSpecs result;
    result.items  = std::move(items);
    result.report = rep;
    return result;
}


ProviderSpecs load_provider_specs()
{
    std::vector<std::pair<std::string, std::string>> layers;

    const auto add_file = [&layers](const std::filesystem::path& file, const std::string& label) {
        std::error_code ec;
        if (!std::filesystem::exists(file, ec) || std::filesystem::is_directory(file, ec)) {
            return;
        }
        std::ifstream stream(file, std::ios::binary);
        if (!stream) {
            return;
        }
        std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
        if (trim(text).empty()) {
            return;
        }
        layers.emplace_back(label, std::move(text));
    };

    // ① 随程序发布的内置表（权威默认）
    add_file(paths::providers_asset_file(), "builtin");
    // ② 开发态兜底（源码树直跑 / 自检）
#ifdef AIWRITE_SOURCE_DIR
    add_file(std::filesystem::path(AIWRITE_SOURCE_DIR) / "assets" / "providers.json", "source");
#endif
    // ③ 用户新增条目（文件名升序）
    std::error_code                    ec;
    std::vector<std::filesystem::path> user_files;
    const std::filesystem::path        user_dir = paths::user_providers_dir();
    if (std::filesystem::is_directory(user_dir, ec)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(user_dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
                user_files.push_back(entry.path());
            }
        }
    }
    std::sort(user_files.begin(), user_files.end());
    for (const std::filesystem::path& file : user_files) {
        add_file(file, "user.d/" + file.filename().string());
    }
    // ④ 用户字段级覆盖
    add_file(paths::user_providers_file(), "user");

    return merge_provider_specs(layers, nullptr);
}

namespace {

std::mutex& specs_mutex()
{
    static std::mutex value;
    return value;
}

std::shared_ptr<const ProviderSpecs>& specs_holder()
{
    static std::shared_ptr<const ProviderSpecs> value;
    return value;
}

} // namespace

std::shared_ptr<const ProviderSpecs> provider_specs_snapshot()
{
    std::lock_guard<std::mutex> lock(specs_mutex());
    if (!specs_holder()) {
        specs_holder() = std::make_shared<const ProviderSpecs>(load_provider_specs());
        const SpecLoadReport& report = specs_holder()->report;
        log::info("[配置表] " + report.summary() + "，条目 " +
                  std::to_string(specs_holder()->items.size()) + " 条");
        for (const std::string& warning : report.warnings) {
            log::warn("[配置表] " + warning);
        }
        for (const std::string& error : report.errors) {
            log::error("[配置表] " + error);
        }
    }
    return specs_holder();
}

const ProviderSpecs& provider_specs()
{
    return *provider_specs_snapshot();
}

void reload_provider_specs()
{
    std::shared_ptr<const ProviderSpecs> fresh =
        std::make_shared<const ProviderSpecs>(load_provider_specs());
    {
        std::lock_guard<std::mutex> lock(specs_mutex());
        specs_holder() = fresh;
    }
    log::info("[配置表] 已重新加载：" + fresh->report.summary() + "，条目 " +
              std::to_string(fresh->items.size()) + " 条（含网页版站点 " +
              std::to_string(fresh->count_kind("web")) + " 个）");
    for (const std::string& warning : fresh->report.warnings) {
        log::warn("[配置表] " + warning);
    }
    for (const std::string& error : fresh->report.errors) {
        log::error("[配置表] " + error);
    }
}


// ============================================================================
//  解析辅助（纯函数；节点 / 提示 / 自检共用）
// ============================================================================

std::string resolve_api_base(const ProviderSpec& spec, const std::string& param_value)
{
    const std::string value = trim(param_value);
    return value.empty() ? spec.api_base : value;
}

std::string resolve_key_ref(const ProviderSpec& spec, const std::string& param_value)
{
    const std::string value = trim(param_value);
    return value.empty() ? spec.key_ref_default : value;
}

std::string resolve_env_name(const ProviderSpec& spec)
{
    return spec.env_names.empty() ? std::string() : spec.env_names.front();
}

std::string resolve_model(const ProviderSpec& spec, const std::string& param_model,
                          const std::string& model_custom, bool for_vision)
{
    const std::string custom = trim(model_custom);
    if (!custom.empty()) {
        return custom; // M5-02：自定义模型名优先
    }
    const std::string node_model = trim(param_model);
    if (!node_model.empty() && spec.find_model(node_model) != nullptr) {
        return node_model; // 节点模型属于该条目 → 用它
    }
    if (for_vision) {
        if (!spec.vision_model_default.empty()) {
            return spec.vision_model_default;
        }
        for (const ProviderModelSpec& model : spec.models) {
            if (model.vision) {
                return model.id;
            }
        }
    }
    if (!spec.models.empty()) {
        return spec.models.front().id; // 表内首选
    }
    return node_model; // 表未声明模型 → 自由填写
}

bool spec_model_supports_vision(const ProviderSpec& spec, const std::string& model,
                                bool* declared)
{
    const ProviderModelSpec* found = spec.find_model(model);
    if (declared != nullptr) {
        *declared = found != nullptr;
    }
    if (found == nullptr) {
        return true; // 表外模型：不拦截，交给后端报错（PB2-12 规则）
    }
    return found->vision;
}


// ============================================================================
//  自检：配置表离线断言（PB2-01；供 aiwrite --provider-selftest /
//        api_probe --exec-selftest 共用；返回失败项数）
// ============================================================================
namespace {

struct CheckCounter {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name, const std::string& detail = {})
    {
        if (condition) {
            ++passed;
            std::printf("   PASS  %s\n", name.c_str());
            return;
        }
        ++failed;
        std::printf("   FAIL  %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ->  ",
                    detail.c_str());
    }
};

bool has_text(const std::vector<std::string>& items, const std::string& needle)
{
    for (const std::string& item : items) {
        if (item.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 合成的基础层（纯离线夹具；不依赖磁盘上的 providers.json）
std::string base_layer_json()
{
    return R"JSON({
  "schema_version": 1,
  "providers": [
    {
      "id": "alpha", "display": "Alpha（官方 API）", "kind": "official", "protocol": "openai",
      "api_base": "https://alpha.example.com/v1", "env_names": ["ALPHA_API_KEY"],
      "key_ref_default": "brain-ai/alpha",
      "capabilities": { "vision": true, "seed": true, "system_role": true },
      "models": [ {"id": "alpha-text"}, {"id": "alpha-vision", "vision": true} ],
      "vision_model_default": "alpha-vision",
      "limits": { "image_max_bytes": 1048576, "connect_timeout_s": 7, "read_timeout_s": 42 }
    },
    {
      "id": "alpha-web", "display": "Alpha（网页版）", "kind": "web", "protocol": "builtin-web",
      "web": {
        "adapter": "builtin:deepseek",
        "login_url": "https://alpha.example.com/chat",
        "window_title": "Alpha 登录",
        "endpoints": { "host": "https://alpha.example.com",
                       "completion_path": "/api/v0/chat/completion",
                       "challenge_path": "/api/v0/chat/create_pow_challenge",
                       "session_create_path": "/api/v0/chat_session/create",
                       "session_fetch_path": "/api/v0/chat_session/fetch_page",
                       "users_path": "/api/v0/users/current" }
      },
      "models": [ {"id": "default"} ]
    }
  ]
})JSON";
}

} // namespace

int provider_spec_selftest(int* passed_out)
{
    CheckCounter counter;
    std::printf("[配置表自检] Provider 规格表（M_patchB L1 / PB2-01）\n");

    // ---------- 1) 基础层解析 ----------
    {
        SpecLoadReport      report;
        const ProviderSpecs specs =
            merge_provider_specs({{"builtin", base_layer_json()}}, &report);
        counter.check(specs.items.size() == 2, "基础层解析：2 条",
                      "实际 " + std::to_string(specs.items.size()));
        counter.check(specs.ids("") == std::vector<std::string>({"alpha", "alpha-web"}),
                      "条目顺序与 id 列表");
        const ProviderSpec* alpha = specs.find("alpha");
        counter.check(alpha != nullptr && alpha->origin == "builtin", "origin = builtin");
        counter.check(alpha != nullptr && alpha->chat_path == "/chat/completions",
                      "chat_path 默认补全（openai）");
        counter.check(alpha != nullptr && alpha->auth_style == "bearer" &&
                          alpha->auth_header == "Authorization",
                      "auth_style / auth_header 默认补全");
        counter.check(alpha != nullptr && alpha->limits.read_timeout_s == 42,
                      "limits 读入（read_timeout_s=42）");
        counter.check(alpha != nullptr && alpha->models.size() == 2 &&
                          alpha->find_model("alpha-vision") != nullptr &&
                          alpha->find_model("alpha-vision")->vision,
                      "models 解析（含逐模型 vision）");
        counter.check(report.errors.empty(), "基础层无 error");
    }

    // ---------- 2) 用户层：字段级覆盖 ----------
    {
        const std::string user = R"JSON({
  "schema_version": 1,
  "providers": [ { "id": "alpha", "api_base": "https://mirror.example.com/v1" } ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs =
            merge_provider_specs({{"builtin", base_layer_json()}, {"user", user}}, &report);
        const ProviderSpec* alpha = specs.find("alpha");
        counter.check(alpha != nullptr && alpha->api_base == "https://mirror.example.com/v1",
                      "用户层覆盖 api_base 生效");
        counter.check(alpha != nullptr && alpha->display == "Alpha（官方 API）",
                      "未写字段保持下层值（display 不变）");
        counter.check(alpha != nullptr && alpha->origin == "user", "origin 变为 user");
        counter.check(specs.find("alpha-web") != nullptr, "其余条目不受影响");
    }

    // ---------- 3) 用户层：新增条目 ----------
    {
        const std::string user = R"JSON({
  "schema_version": 1,
  "providers": [ { "id": "my-ai", "display": "我的自建服务", "kind": "official",
                   "protocol": "openai", "api_base": "http://192.168.1.10:8000/v1",
                   "env_names": ["MY_AI_KEY"] } ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs(
            {{"builtin", base_layer_json()}, {"user.d/10-my.json", user}}, &report);
        const ProviderSpec* added = specs.find("my-ai");
        counter.check(added != nullptr && added->key_ref_default == "brain-ai/my-ai",
                      "新增条目：key_ref_default 自动补全");
        counter.check(added != nullptr && added->origin == "user.d/10-my.json",
                      "新增条目 origin 记录文件名");
        counter.check(specs.items.size() == 3, "新增后共 3 条");
    }


    // ---------- 4) 必填缺失 / 类型错误 / 未知字段 / 明文密钥 ----------
    {
        const std::string broken = R"JSON({
  "schema_version": 1,
  "providers": [
    { "id": "no-kind", "display": "缺 kind", "protocol": "openai" },
    { "id": "bad-models", "display": "models 类型不符", "kind": "official",
      "protocol": "openai", "models": "not-an-array" },
    { "id": "unknown-field", "display": "未知字段", "kind": "official",
      "protocol": "openai", "api_base": "https://x.example.com", "whatever": 1 },
    { "id": "with-secret", "display": "含密钥", "kind": "official", "protocol": "openai",
      "api_base": "https://y.example.com", "api_key": "sk-0123456789abcdef" }
  ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs({{"user", broken}}, &report);
        counter.check(report.errors.size() >= 2, "必填缺失 + 类型错误各记一次 error",
                      "errors=" + std::to_string(report.errors.size()));
        counter.check(specs.find("no-kind") == nullptr, "缺少 kind 的条目被跳过");
        counter.check(specs.find("bad-models") == nullptr, "models 类型不符的条目被跳过");
        counter.check(specs.find("unknown-field") != nullptr &&
                          has_text(report.warnings, "未知字段"),
                      "未知字段 → 警告但条目保留");
        counter.check(specs.find("with-secret") != nullptr &&
                          has_text(report.warnings, "密钥"),
                      "明文密钥 → 警告（字段被拒绝）");
    }

    // ---------- 5) 坏 JSON / schema_version / 层忽略但其他层生效 ----------
    {
        const std::string bad_json = "{ \"schema_version\": 1, \"providers\": [ }";
        SpecLoadReport    report;
        const ProviderSpecs specs = merge_provider_specs(
            {{"builtin", base_layer_json()}, {"user.d/broken.json", bad_json}}, &report);
        counter.check(has_text(report.errors, "JSON 解析失败"), "坏 JSON → error 记录");
        counter.check(specs.items.size() == 2, "坏层不影响有效层（仍 2 条）");

        const std::string wrong_version = R"JSON({ "schema_version": 2, "providers": [] })JSON";
        SpecLoadReport    report2;
        merge_provider_specs({{"user", wrong_version}}, &report2);
        counter.check(has_text(report2.errors, "schema_version"),
                      "schema_version 不匹配 → error");

        const std::string no_providers = R"JSON({ "schema_version": 1 })JSON";
        SpecLoadReport    report3;
        merge_provider_specs({{"user", no_providers}}, &report3);
        counter.check(has_text(report3.errors, "providers"), "缺少 providers 数组 → error");
    }

    // ---------- 6) 空表 → 最小兜底；replace_all ----------
    {
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs({}, &report);
        counter.check(report.fallback_used && specs.items.size() == 2,
                      "空表 → 最小兜底 2 条");
        counter.check(specs.find("deepseek") != nullptr &&
                          specs.find("custom-official") != nullptr,
                      "兜底表含 deepseek 与 custom-official");
        counter.check(specs.find("deepseek") != nullptr &&
                          specs.find("deepseek")->api_base == "https://api.deepseek.com",
                      "兜底 deepseek 地址与改造前一致");

        const std::string replace_all = R"JSON({
  "schema_version": 1, "replace_all": true,
  "providers": [ { "id": "only-one", "display": "唯一", "kind": "official",
                   "protocol": "openai", "api_base": "https://z.example.com" } ]
})JSON";
        SpecLoadReport      report2;
        const ProviderSpecs specs2 =
            merge_provider_specs({{"builtin", base_layer_json()}, {"user", replace_all}},
                                 &report2);
        counter.check(specs2.items.size() == 1 && specs2.find("only-one") != nullptr,
                      "replace_all=true：只保留该层条目");
        counter.check(has_text(report2.warnings, "replace_all"), "replace_all 有提示");
    }


    // ---------- 7) web 条目：两种形态的校验与提示 ----------
    {
        const std::string web_layer = R"JSON({
  "schema_version": 1,
  "providers": [
    { "id": "dom-site", "display": "DOM 站点", "kind": "web", "protocol": "dom",
      "web": { "adapter": "dom", "login_url": "https://site.example.com/" } },
    { "id": "partial-endpoints", "display": "缺端点", "kind": "web", "protocol": "builtin-web",
      "web": { "adapter": "builtin:deepseek", "login_url": "https://s2.example.com/",
               "endpoints": { "host": "https://s2.example.com" } } },
    { "id": "builtin-with-selector", "display": "多余字段", "kind": "web",
      "protocol": "builtin-web",
      "web": { "adapter": "builtin:deepseek", "login_url": "https://s3.example.com/",
               "input_selector": "#prompt" } }
  ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs({{"user", web_layer}}, &report);
        // M_patchB L1 修订（PB2-26）：dom 缺生成字段 → **登录型站点条目**（警告，**不**丢弃）
        const ProviderSpec* login_only = specs.find("dom-site");
        counter.check(login_only != nullptr && web_login_only(login_only) &&
                          !has_text(report.errors, "dom-site") &&
                          has_text(report.warnings, "登录型站点条目"),
                      "dom 缺生成字段 → 登录型站点条目（警告，不丢弃；生成未就绪）");
        const ProviderSpec* partial = specs.find("partial-endpoints");
        counter.check(partial != nullptr &&
                          partial->web.endpoints.host == "https://s2.example.com" &&
                          partial->web.endpoints.completion_path ==
                              default_deepseek_web_endpoints().completion_path,
                      "builtin 端点缺字段 → 沿用内置默认");
        counter.check(has_text(report.warnings, "web.endpoints"), "缺端点有警告");
        const ProviderSpec* extra = specs.find("builtin-with-selector");
        counter.check(extra != nullptr && !extra->web.probe_paths.empty(),
                      "builtin 站点：probe_paths 自动补默认两条");
        counter.check(has_text(report.warnings, "不支持"), "adapter 不支持字段 → 警告（R12）");
    }

    // ---------- 8) 未实现协议（表里有、程序没有）→ 警告而非报错 ----------
    {
        const std::string future = R"JSON({
  "schema_version": 1,
  "providers": [ { "id": "future-api", "display": "未来协议", "kind": "official",
                   "protocol": "anthropic", "api_base": "https://api.anthropic.com" } ]
})JSON";
        SpecLoadReport      report;
        const ProviderSpecs specs = merge_provider_specs({{"user", future}}, &report);
        counter.check(specs.find("future-api") != nullptr, "未实现协议的条目仍保留（可显示）");
        counter.check(has_text(report.warnings, "尚未实现"), "未实现协议 → 警告（R9）");
    }

    // ---------- 9) 解析辅助（节点与提示共用） ----------
    {
        const ProviderSpecs fixture =
            merge_provider_specs({{"builtin", base_layer_json()}}, nullptr);
        const ProviderSpec* alpha = fixture.find("alpha");
        counter.check(alpha != nullptr, "解析辅助夹具就绪");
        if (alpha != nullptr) {
            counter.check(resolve_api_base(*alpha, "https://mine.example.com") ==
                              "https://mine.example.com",
                          "resolve_api_base：节点参数优先");
            counter.check(resolve_api_base(*alpha, "  ") == "https://alpha.example.com/v1",
                          "resolve_api_base：空 → 表默认");
            counter.check(resolve_key_ref(*alpha, "") == "brain-ai/alpha",
                          "resolve_key_ref：空 → 表内引用名");
            counter.check(resolve_env_name(*alpha) == "ALPHA_API_KEY",
                          "resolve_env_name：取表内首个");
            counter.check(resolve_model(*alpha, "", "my-model", false) == "my-model",
                          "resolve_model：自定义优先");
            counter.check(resolve_model(*alpha, "alpha-vision", "", false) == "alpha-vision",
                          "resolve_model：节点值在表内 → 用它");
            counter.check(resolve_model(*alpha, "deepseek-chat", "", false) == "alpha-text",
                          "resolve_model：节点值不在表内 → 表内首选");
            counter.check(resolve_model(*alpha, "", "", true) == "alpha-vision",
                          "resolve_model：视觉 → vision_model_default");
            bool declared = false;
            counter.check(spec_model_supports_vision(*alpha, "alpha-text", &declared) == false &&
                              declared,
                          "视觉门控：表内非视觉模型 → false");
            counter.check(spec_model_supports_vision(*alpha, "alpha-vision", &declared) == true &&
                              declared,
                          "视觉门控：表内视觉模型 → true");
            counter.check(spec_model_supports_vision(*alpha, "free-model", &declared) == true &&
                              !declared,
                          "视觉门控：表外模型 → 允许（declared=false）");
        }
    }

    // ---------- 10) 真实配置表可加载（磁盘缺失 → 兜底） ----------
    {
        const ProviderSpecs& specs = provider_specs();
        counter.check(!specs.items.empty(), "provider_specs() 至少 1 条");
        counter.check(specs.find("deepseek") != nullptr, "真实表中存在 deepseek");
        const ProviderSpec* real = specs.find("deepseek");
        std::printf("   ----  真实表：来源=%s；%s；条目 %zu 条\n",
                    real != nullptr ? real->origin.c_str() : "?",
                    specs.report.summary().c_str(), specs.items.size());
    }

    std::printf("[配置表自检] %d 项通过 / %d 项失败\n", counter.passed, counter.failed);
    if (passed_out != nullptr) {
        *passed_out = counter.passed;
    }
    return counter.failed;
}



} // namespace aiwrite::ai
