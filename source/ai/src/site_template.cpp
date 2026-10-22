#include "site_template.h"

#include "log.h"
#include "string_util.h"   // CLN-05：trim 收敛

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

namespace aiwrite::ai {
namespace {

using ordered_json = nlohmann::ordered_json;

bool is_id_char(char ch)
{
    const unsigned char uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) != 0 || ch == '-' || ch == '_' || ch == '.';
}

// 登录窗口标题的默认值（用户留空时按显示名生成；与 §8 附录 D 的写法一致）
std::string window_title_of(const WebSiteTemplate& tpl)
{
    if (!tpl.window_title.empty()) {
        return tpl.window_title;
    }
    const std::string name = tpl.display.empty() ? tpl.id : tpl.display;
    return "AIwrite · " + name + " 登录（登录后关闭窗口）";
}

std::string notes_of(const WebSiteTemplate& tpl)
{
    if (!tpl.notes.empty()) {
        return tpl.notes;
    }
    return "由 AIwrite 界面创建（选择器未实测：生成需要 input_selector / send / answer_selector）";
}

// 自检用的**合成基础层**（纯离线夹具；不依赖磁盘上的 providers.json）
std::string selftest_base_layer_json()
{
    return R"JSON({
  "schema_version": 1,
  "providers": [
    {
      "id": "base-official", "display": "Base（官方 API）", "kind": "official",
      "protocol": "openai", "api_base": "https://base.example.com/v1",
      "models": [ { "id": "base-text" } ]
    }
  ]
})JSON";
}

// 自检计数助手（与 provider_spec_selftest 同风格：逐项打印，失败带原因）
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

} // namespace

// --------------------------------------------------------------------- 模板 --

WebSiteTemplate siteTemplateDefault()
{
    WebSiteTemplate tpl;
    tpl.id               = "my-site";
    tpl.display          = "我的网页版站点";
    tpl.login_url        = "";
    tpl.send_kind        = "key";
    tpl.send_value       = "Enter";
    tpl.answer_poll_ms   = 500;
    tpl.answer_max_polls = 120;
    tpl.notes            = "由 AIwrite 界面创建（选择器未实测：生成需要 input_selector / send / "
                           "answer_selector）";
    return tpl;
}

WebSiteTemplate siteTemplateFromSpec(const ProviderSpec* spec)
{
    if (spec == nullptr || spec->kind != "web") {
        return siteTemplateDefault();
    }
    WebSiteTemplate tpl = siteTemplateDefault();
    tpl.id           = spec->id;
    tpl.display      = spec->display.empty() ? spec->id : spec->display;
    tpl.login_url    = spec->web.login_url;
    tpl.window_title = spec->web.window_title;
    tpl.cookie_names = joinNames(spec->web.cookie_names);
    tpl.token_expr   = spec->web.token_expr;
    tpl.notes        = spec->notes;

    // 仅当源条目**本身就是 DOM 站点**时才抄选择器：
    //  * 内建站点（`builtin:*` / 空 adapter）的选择器字段在解析期会被判「不支持（已忽略）」⇒ 不抄
    //    （守 §8 附录 D：内建适配器不可被用户文件复刻）
    if (spec->web.adapter == "dom") {
        tpl.input_selector   = spec->web.input_selector;
        tpl.send_kind        = spec->web.send_kind;
        tpl.send_value       = spec->web.send_value;
        tpl.answer_selector  = spec->web.answer_selector;
        tpl.done_kind        = spec->web.done_kind;
        tpl.done_selector    = spec->web.done_selector;
        tpl.answer_poll_ms   = spec->web.answer_poll_ms;
        tpl.answer_max_polls = spec->web.answer_max_polls;
        if (tpl.notes.empty()) {
            tpl.notes = "照抄自内置条目 " + spec->id + "（自建：请按需修改选择器）";
        }
    }
    else if (tpl.notes.empty()) {
        tpl.notes = "照抄自内置站点 " + spec->id + " 的身份信息（选择器待实测填写）";
    }
    return tpl;
}

// ----------------------------------------------------------------- 校验 / 落盘 --

// 共享的条目 id 校验（站点 / 官方 API 提供商共用；`kind_label` = 「站点」/「提供商」）
std::string providerIdError(const std::string& id, const std::string& kind_label)
{
    const std::string fname = kind_label + " id";
    if (id.empty()) {
        return fname + " 不能为空（它会成为文件名 providers.d/<id>.json）";
    }
    if (id.size() > 64) {
        return fname + " 过长（≤64 字符）：" + id;
    }
    if (utils::trim(id) != id) {
        return fname + " 首尾不能有空白：" + id;
    }
    for (const char ch : id) {
        if (!is_id_char(ch)) {
            return std::string(fname + " 只能含字母、数字、`-`、`_`、`.`（当前含「") + ch +
                   "」；不允许空格 / 斜杠 / 中文）";
        }
    }
    if (std::isalnum(static_cast<unsigned char>(id[0])) == 0) {
        return fname + " 必须以字母或数字开头：" + id;
    }
    return {};
}

std::string siteIdError(const std::string& id)
{
    return providerIdError(id, "站点");
}


std::string siteTemplateError(const WebSiteTemplate& tpl)
{
    const std::string id_problem = siteIdError(tpl.id);
    if (!id_problem.empty()) {
        return id_problem;
    }
    const std::string url = utils::trim(tpl.login_url);
    if (url.empty()) {
        return "登录页地址不能为空（站点身份的唯一来源；见 §8 附录 C/D）";
    }
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
        return "登录页地址必须以 http:// 或 https:// 开头：" + url;
    }
    return {};
}

std::string siteTemplatePending(const WebSiteTemplate& tpl)
{
    std::vector<std::string> missing;
    if (utils::trim(tpl.input_selector).empty()) {
        missing.push_back("input_selector");
    }
    if (utils::trim(tpl.send_kind).empty() || utils::trim(tpl.send_value).empty()) {
        missing.push_back("send");
    }
    if (utils::trim(tpl.answer_selector).empty()) {
        missing.push_back("answer_selector");
    }
    std::string text;
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i > 0) {
            text += " / ";
        }
        text += missing[i];
    }
    return text;
}

std::vector<std::string> splitNames(const std::string& text)
{
    // 全角逗号 / 分号（U+FF0C / U+FF1B）先归一为 ASCII —— 用户从文档整段粘贴也能用
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == 0xEFU && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xBCU &&
            (static_cast<unsigned char>(text[i + 2]) == 0x8CU ||
             static_cast<unsigned char>(text[i + 2]) == 0x9BU)) {
            normalized.push_back(',');
            i += 2;
            continue;
        }
        normalized.push_back(text[i]);
    }

    std::vector<std::string> names;
    std::string              current;
    const auto               flush = [&names, &current]() {
        const std::string name = utils::trim(current);
        current.clear();
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    };
    for (const char ch : normalized) {
        if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r' || ch == '|') {
            flush();
            continue;
        }
        current.push_back(ch);
    }
    flush();
    return names;
}

std::string joinNames(const std::vector<std::string>& names)
{
    std::string text;
    for (const std::string& name : names) {
        if (name.empty()) {
            continue;
        }
        if (!text.empty()) {
            text += ",";
        }
        text += name;
    }
    return text;
}
std::string siteFileJson(const WebSiteTemplate& tpl)
{
    ordered_json entry;
    entry["id"]       = tpl.id;
    entry["display"]  = tpl.display.empty() ? tpl.id : tpl.display;
    entry["kind"]     = "web";
    entry["protocol"] = "dom";
    entry["verified"] = false; // 照抄模板一律标「未验证」（PB2-26：不实测不填 verified）
    const std::string entry_notes = notes_of(tpl);
    if (!entry_notes.empty()) {
        entry["notes"] = entry_notes;
    }

    ordered_json web;
    web["adapter"]      = "dom"; // 恒 dom：内建适配器不可被用户文件复刻（§8 附录 D）
    web["login_url"]    = tpl.login_url;
    web["window_title"] = window_title_of(tpl);
    // 只写**非空**的可选字段：留空 = 走解析期默认（也避免「已声明但为空」被误当实测值）
    if (!utils::trim(tpl.input_selector).empty()) {
        web["input_selector"] = tpl.input_selector;
    }
    if (!utils::trim(tpl.send_kind).empty() && !utils::trim(tpl.send_value).empty()) {
        web["send"] = ordered_json{{"kind", tpl.send_kind}, {"value", tpl.send_value}};
    }
    if (!utils::trim(tpl.answer_selector).empty()) {
        web["answer_selector"] = tpl.answer_selector;
    }
    if (!utils::trim(tpl.done_kind).empty()) {
        web["done_when"] = ordered_json{{"kind", tpl.done_kind}, {"selector", tpl.done_selector}};
    }
    const std::vector<std::string> cookies = splitNames(tpl.cookie_names);
    if (!cookies.empty()) {
        web["cookie_names"] = cookies;
    }
    if (!utils::trim(tpl.token_expr).empty()) {
        web["token_expr"] = tpl.token_expr;
    }
    web["answer_poll_ms"]   = tpl.answer_poll_ms;
    web["answer_max_polls"] = tpl.answer_max_polls;
    entry["web"]            = web;

    ordered_json root;
    root["schema_version"] = 1; // 信封（缺它 = 整层被忽略；见 §8 附录 C/D 实测表）
    // `_` 前缀 = 纯文档字段（解析器忽略且不告警）：给非技术用户留一句「怎么生效」
    root["_comment"] = "由 AIwrite 界面生成：AIwrite →「设置」→「提供商与站点表」→"
                       "「重新加载配置表」即可生效（无需重启）。每站一份文件，可直接分享/备份。";
    root["providers"] = ordered_json::array({entry});
    return root.dump(2) + "\n";
}

bool write_user_site_file(const WebSiteTemplate& tpl, const std::filesystem::path& dir,
                          bool overwrite, std::string* error, std::filesystem::path* written)
{
    const std::string problem = siteTemplateError(tpl);
    if (!problem.empty()) {
        if (error != nullptr) {
            *error = problem;
        }
        return false;
    }
    return writeUserProviderFile(tpl.id, siteFileJson(tpl), dir, overwrite, error, written);
}

bool writeUserProviderFile(const std::string& id, const std::string& json,
                           const std::filesystem::path& dir, bool overwrite,
                           std::string* error, std::filesystem::path* written)
{
    std::error_code             ec;
    const std::filesystem::path path = dir / (id + ".json");
    if (!overwrite && std::filesystem::exists(path, ec)) {
        if (error != nullptr) {
            *error = "已存在同名文件：" + path.string() + "（勾选「覆盖同名条目」后重试）";
        }
        return false;
    }

    std::filesystem::create_directories(dir, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "无法创建目录：" + dir.string() + "（" + ec.message() + "）";
        }
        return false;
    }

    // 原子落盘：先写 `.tmp` 再改名（中途失败不会留下半截 JSON 把整层搞坏；守 R8）
    const std::filesystem::path tmp(path.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error != nullptr) {
                *error = "无法写入文件：" + tmp.string() + "（目录不可写或磁盘已满）";
            }
            return false;
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(tmp, ec);
            if (error != nullptr) {
                *error = "写入失败（磁盘已满？）：" + tmp.string();
            }
            return false;
        }
    }
    if (overwrite) {
        std::filesystem::remove(path, ec); // Windows 的 rename 不覆盖已存在目标
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code cleanup;
        std::filesystem::remove(tmp, cleanup);
        if (error != nullptr) {
            *error = "落盘失败：" + path.string() + "（" + ec.message() + "）";
        }
        return false;
    }

    if (written != nullptr) {
        *written = path;
    }
    log::info("[站点表] 已写入用户条目 " + path.string() + "（重新加载配置表后立即可用）");
    return true;
}


// ============================================================================
//  自检（离线；`api_probe --site-assert` 调用；返回失败项数）
// ============================================================================

int siteTemplateSelftest(int* passed_out)
{
    std::printf("[站点模板] 自建网页版站点条目：生成 / 校验 / 落盘\n");
    CheckCounter counter;

    // ---- ① id 合法性（含路径穿越，防写出到 providers.d 之外）----
    counter.check(siteIdError("kimi-web").empty(), "id：kimi-web 合法");
    counter.check(siteIdError("SITE_1.2").empty(), "id：SITE_1.2 合法");
    counter.check(!siteIdError("").empty(), "id：空被拒");
    counter.check(!siteIdError("my site").empty(), "id：含空格被拒");
    counter.check(!siteIdError("../evil").empty(), "id：路径穿越 `../` 被拒");
    counter.check(!siteIdError("a/b").empty(), "id：斜杠被拒");
    counter.check(!siteIdError("-lead").empty(), "id：首字符须为字母或数字");
    counter.check(!siteIdError(std::string(65, 'a')).empty(), "id：>64 字符被拒");
    // 共享函数（自定义 AI 收口：官方 API 提供商复用同一规则）
    counter.check(siteIdError("x") == providerIdError("x", "站点"),
                  "共享：siteIdError ≡ providerIdError(id, \"站点\")");
    counter.check(providerIdError("my-provider_2.0", "提供商").empty(), "共享：提供商 id 合法");
    counter.check(providerIdError("../x", "提供商").find("提供商 id") != std::string::npos,
                  "共享：报错文案前缀为「提供商 id」");


    // ---- ② 模板校验（缺登录页 / 非 http(s) 一律拒写）----
    WebSiteTemplate tpl = siteTemplateDefault();
    counter.check(!siteTemplateError(tpl).empty(), "校验：缺登录页 → 拒写",
                  siteTemplateError(tpl));
    tpl.id        = "demo-site";
    tpl.display   = "演示站点（自建）";
    tpl.login_url = "ftp://demo.example.com";
    counter.check(!siteTemplateError(tpl).empty(), "校验：非 http(s) 登录页 → 拒写");
    tpl.login_url = "https://demo.example.com/chat";
    counter.check(siteTemplateError(tpl).empty(), "校验：id + https 登录页 → 通过",
                  siteTemplateError(tpl));
    counter.check(siteTemplatePending(tpl).find("input_selector") != std::string::npos,
                  "校验：选择器缺失 → 待实测清单点名 input_selector");

    // ---- ③ 生成的 JSON（信封 / 嵌套形状 / 白名单字段）----
    const std::string text = siteFileJson(tpl);
    counter.check(text.find("\"schema_version\": 1") != std::string::npos,
                  "JSON：带 schema_version 信封");
    counter.check(text.find("\"_comment\"") != std::string::npos,
                  "JSON：带 _comment 说明（`_` 前缀不告警）");
    counter.check(text.find("\"kind\": \"web\"") != std::string::npos, "JSON：kind=web");
    counter.check(text.find("\"adapter\": \"dom\"") != std::string::npos,
                  "JSON：web.adapter 恒为 dom");

    // ---- ④ 完整模板（照抄 DOM 条目）→ 与内置层合并后零错误 ----
    WebSiteTemplate full;
    full.id                = tpl.id;
    full.display           = tpl.display;
    full.login_url         = tpl.login_url;
    full.input_selector    = "div.chat-input-editor";
    full.send_kind         = "key";
    full.send_value        = "Enter";
    full.answer_selector   = ".markdown-body";
    full.done_kind         = "selector_gone";
    full.done_selector     = "button[aria-label*='停止']";
    full.cookie_names      = "kimi_session, kimi_token";
    full.token_expr        = "localStorage.getItem('token')";
    counter.check(siteTemplatePending(full).empty(), "模板：完整模板 → 待实测清单为空");
    counter.check(splitNames(full.cookie_names).size() == 2, "Cookie 名单：`a, b` 两项");
    counter.check(splitNames("a，b；c").size() == 3, "Cookie 名单：全角分隔符三项");
    counter.check(splitNames("a,a,b").size() == 2, "Cookie 名单：去重");

    SpecLoadReport      report;
    const ProviderSpecs specs = merge_provider_specs(
        {{"builtin", selftest_base_layer_json()}, {"user.d", siteFileJson(full)}}, &report);
    counter.check(report.errors.empty(), "合并：生成的文件零错误",
                  report.errors.empty() ? "" : report.errors.front());
    const ProviderSpec* loaded = specs.find("demo-site");
    counter.check(loaded != nullptr, "合并：条目已进入表");
    if (loaded != nullptr) {
        counter.check(loaded->kind == "web" && loaded->web.adapter == "dom",
                      "合并：kind=web / adapter=dom");
        counter.check(loaded->web.login_url == full.login_url, "合并：login_url 一致");
        counter.check(loaded->web.send_kind == "key" && loaded->web.send_value == "Enter",
                      "合并：web.send{} 嵌套形状被解析");
        counter.check(loaded->web.done_kind == "selector_gone" &&
                          !loaded->web.done_selector.empty(),
                      "合并：web.done_when{} 嵌套形状被解析");
        counter.check(loaded->web.cookie_names.size() == 2, "合并：cookie_names 数组被解析");
        counter.check(loaded->web.answer_poll_ms == full.answer_poll_ms,
                      "合并：answer_poll_ms 一致");
    }
    counter.check(!has_text(report.warnings, "未知字段"),
                  "合并：无「未知字段」告警（只写白名单字段）");

    // ---- ⑤ 回归护栏：缺信封 ⇒ 整层失效（R8 / §8 附录 D 实测表第一行）----
    std::string no_envelope = siteFileJson(full);
    const auto  envelope_at = no_envelope.find("  \"schema_version\"");
    if (envelope_at != std::string::npos) {
        const auto line_end = no_envelope.find('\n', envelope_at);
        no_envelope.erase(envelope_at, line_end - envelope_at + 1);
    }
    SpecLoadReport      report_missing;
    const ProviderSpecs specs_missing = merge_provider_specs(
        {{"builtin", selftest_base_layer_json()}, {"user.d", no_envelope}}, &report_missing);
    counter.check(!report_missing.errors.empty(), "缺信封：明确报错（整层失效）");
    counter.check(specs_missing.find("demo-site") == nullptr, "缺信封：条目未进入表");
    counter.check(has_text(report_missing.errors, "schema_version"),
                  "缺信封：错误文案点名 schema_version");

    // ---- ⑥ 转义 / 中文往返 ----
    WebSiteTemplate escaped = full;
    escaped.id              = "esc-site";
    escaped.display         = "带\"引号\"与\\反斜杠的站";
    escaped.input_selector  = "div[data-x=\"a\\b\"]";
    escaped.notes           = "中文备注：自建（未实测）";
    SpecLoadReport      report_esc;
    const ProviderSpecs specs_esc =
        merge_provider_specs({{"builtin", selftest_base_layer_json()},
                              {"user.d", siteFileJson(escaped)}},
                             &report_esc);
    const ProviderSpec* confirmed = specs_esc.find("esc-site");
    counter.check(confirmed != nullptr && confirmed->display == escaped.display,
                  "转义：显示名（引号 / 反斜杠 / 中文）往返一致");
    counter.check(confirmed != nullptr && confirmed->web.input_selector == escaped.input_selector,
                  "转义：选择器（引号 / 反斜杠）往返一致");

    // ---- ⑦ 落盘（**自建临时目录**；不碰用户数据）----
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / "aiwrite-site-template-selftest";
    std::filesystem::remove_all(dir, ec);
    std::string           err;
    std::filesystem::path written;
    counter.check(write_user_site_file(full, dir, false, &err, &written) &&
                      std::filesystem::exists(written),
                  "落盘：首次写成功（目录自动创建）", err);
    counter.check(!write_user_site_file(full, dir, false, &err, nullptr),
                  "落盘：同名文件默认拒绝覆盖");
    counter.check(err.find("已存在同名文件") != std::string::npos,
                  "落盘：拒绝文案可操作（点明如何覆盖）", err);
    WebSiteTemplate changed = full;
    changed.display         = "改名后";
    counter.check(write_user_site_file(changed, dir, true, &err, nullptr), "落盘：显式覆盖成功",
                  err);
    {
        std::ifstream in(written, std::ios::binary);
        std::string   body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
        counter.check(body.find("改名后") != std::string::npos, "落盘：覆盖后内容已更新");
        counter.check(body.find("\"schema_version\"") != std::string::npos, "落盘：文件带信封");
    }
    counter.check(!std::filesystem::exists(std::filesystem::path(written.string() + ".tmp")),
                  "落盘：无残留 .tmp");
    WebSiteTemplate bad = full;
    bad.id              = "../evil";
    counter.check(!write_user_site_file(bad, dir, true, &err, nullptr), "落盘：坏 id 不落盘");
    std::filesystem::remove_all(dir, ec);
    counter.check(!std::filesystem::exists(dir), "落盘：自检目录已清理（零残留）");

    // ---- ⑧ 照抄：内建站点只抄身份（选择器字段不抄，避免「不支持（已忽略）」告警）----
    ProviderSpec builtin_source;
    builtin_source.id                 = "deepseek-web";
    builtin_source.kind               = "web";
    builtin_source.display            = "DeepSeek（网页版）";
    builtin_source.web.adapter        = "builtin:deepseek";
    builtin_source.web.login_url      = "https://chat.deepseek.com";
    builtin_source.web.input_selector = "should-not-copy";
    builtin_source.web.cookie_names   = {"userToken"};
    const WebSiteTemplate copied = siteTemplateFromSpec(&builtin_source);
    counter.check(copied.id == "deepseek-web" && copied.login_url == "https://chat.deepseek.com",
                  "照抄：站点身份（id / 登录页）已抄");
    counter.check(copied.input_selector.empty(), "照抄：内建站点的选择器不抄");
    counter.check(copied.cookie_names == "userToken", "照抄：Cookie 名清单已抄");
    counter.check(!siteFileJson(copied).empty(), "照抄：可直接生成文件 JSON");

    if (passed_out != nullptr) {
        *passed_out = counter.passed;
    }
    std::printf("   站点模板自检结果：%d 通过 / %d 失败\n", counter.passed, counter.failed);
    return counter.failed;
}

} // namespace aiwrite::ai





