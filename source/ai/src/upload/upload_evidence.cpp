#include "ai/upload/upload_evidence.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <string>
#include <utility>

namespace aiwrite::ai {
namespace {

using json = nlohmann::json;

// 原始结果片段的可读摘要（诊断用；过长截断）
std::string snippet(const std::string& text, std::size_t limit = 120)
{
    if (text.size() <= limit) {
        return text;
    }
    return text.substr(0, limit) + "…";
}

} // namespace

std::string NetRecord::line() const
{
    return (method.empty() ? std::string("?") : method) + " " +
           (status > 0 ? std::to_string(status) : std::string("-")) + " " +
           (url.empty() ? std::string("(空 URL)") : url);
}

bool UploadNetwork::url_matches_any(const std::string& url, const std::vector<std::string>& needles)
{
    if (url.empty() || needles.empty()) {
        return false;
    }
    for (const std::string& needle : needles) {
        if (!needle.empty() && url.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string UploadNetwork::evidence_for(const UploadExpectations& expect) const
{
    if (expect.net_url_contains.empty()) {
        return {};
    }
    std::size_t hits  = 0;
    std::string first;
    for (const NetRecord& record : records) {
        if (url_matches_any(record.url, expect.net_url_contains)) {
            ++hits;
            if (first.empty()) {
                first = record.line();
            }
        }
    }
    if (hits == 0) {
        return {};
    }
    return first + "（命中 " + std::to_string(hits) + " 条）";
}

// M8-37：**站点无关**的网络自动判据（声明特征未命中时的兜底；不新增任何开关）
//  * 判据 = 交件后出现的 **POST / PUT** 请求，且 URL 含通用上传特征词
//    （`upload` / `/files` / `file/` / `attach` / `media` / `oss` / `tos` / `storage` / `blob`）
//  * 为什么要求 POST/PUT：上传必然是写请求 —— 用 GET 命中特征词（页面 / 接口文档）会误判
//  * 返回空串 = 未取到自动证据（调用方据此判「缺网络回执」）
std::string UploadNetwork::evidence_for_auto() const
{
    static const std::vector<std::string> kGeneric = {"upload", "/files", "file/",  "attach", "media",
                                                      "oss",    "tos",    "storage", "blob"};
    std::size_t hits  = 0;
    std::string first;
    for (const NetRecord& record : records) {
        const bool is_write = record.method == "POST" || record.method == "PUT";
        if (!is_write || !url_matches_any(record.url, kGeneric)) {
            continue;
        }
        ++hits;
        if (first.empty()) {
            first = record.line();
        }
    }
    if (hits == 0) {
        return {};
    }
    return "自动判据：" + first + "（命中 " + std::to_string(hits) + " 条写请求）";
}

std::string UploadNetwork::summary() const
{
    if (records.empty()) {
        return "0 条（未采集到任何网络回执）";
    }
    std::string       out   = std::to_string(records.size()) + " 条：";
    const std::size_t shown = records.size() > 5 ? 5 : records.size();
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0) {
            out += " | ";
        }
        out += records[i].line();
    }
    if (records.size() > shown) {
        out += " | …";
    }
    return out;
}

namespace {

// M8-37：站点无关的自动判据 —— 附件候选池（只读采样）
constexpr const char* kAttachPoolSelector =
    "[class*=\"attach\"], [class*=\"upload\"], [class*=\"file\"]";

std::string page_state_script_impl(const std::vector<std::string>& selectors,
                                   const std::vector<std::string>& file_names)
{
    json array = json::array();
    for (const std::string& selector : selectors) {
        array.push_back(selector);
    }
    json names = json::array();
    for (const std::string& name : file_names) {
        names.push_back(name);
    }
    std::string script;
    script += "(function () {\n";
    script += "  const sels = " + array.dump() + ";\n";
    script += "  const names = " + names.dump() + ";\n";
    script += "  const pool = " + json(kAttachPoolSelector).dump() + ";\n";
    script += "  const out = { url: location.href, title: document.title, counts: [],\n";
    script += "                file_hits: [], attach_pool: 0, blob_images: 0 };\n";
    script += "  for (const s of sels) {\n";
    script += "    let n = 0;\n";
    script += "    try { n = document.querySelectorAll(s).length; } catch (e) { n = -1; }\n";
    script += "    out.counts.push([s, n]);\n";
    script += "  }\n";
    // M8-37：交付文件名是否出现在页面（附件卡片常见呈现）—— 站点无关的「页面已挂上」判据
    script += "  let text = '';\n";
    script += "  try { text = document.body ? (document.body.innerText || '') : ''; } catch (e) { text = ''; }\n";
    script += "  for (const nm of names) {\n";
    script += "    if (!nm) { continue; }\n";
    script += "    let n = 0;\n";
    script += "    if (text.indexOf(nm) >= 0) { n = 1; }\n";
    script += "    if (n === 0) { // 缩略图 / alt / title / aria 里出现文件名也算\n";
    script += "      try {\n";
    script += "        document.querySelectorAll('img[alt], [title], [aria-label]').forEach(function (el) {\n";
    script += "          if (n > 0) { return; }\n";
    script += "          const s = (el.getAttribute('alt') || '') + (el.getAttribute('title') || '') +\n";
    script += "                    (el.getAttribute('aria-label') || '');\n";
    script += "          if (s && s.indexOf(nm) >= 0) { n = 1; }\n";
    script += "        });\n";
    script += "      } catch (e) { }\n";
    script += "    }\n";
    script += "    out.file_hits.push([nm, n]);\n";
    script += "  }\n";
    script += "  try { out.attach_pool = document.querySelectorAll(pool).length; } catch (e) { out.attach_pool = -1; }\n";
    script += "  try {\n";
    script += "    out.blob_images = document.querySelectorAll('img[src^=\"blob:\"], img[src^=\"data:\"]').length;\n";
    script += "  } catch (e) { out.blob_images = 0; }\n";
    script += "  return out;\n";
    script += "})();";
    return script;
}

std::vector<std::pair<std::string, int>> parse_count_pairs(const json& parsed, const char* key)
{
    std::vector<std::pair<std::string, int>> out;
    const auto                               items = parsed.find(key);
    if (items == parsed.end() || !items->is_array()) {
        return out;
    }
    for (const auto& item : *items) {
        if (!item.is_array() || item.size() < 2 || !item[0].is_string() || !item[1].is_number_integer()) {
            continue; // 形态不符的条目跳过（不因此让整次采样失败）
        }
        out.emplace_back(item[0].get<std::string>(), item[1].get<int>());
    }
    return out;
}

} // namespace

std::string page_state_script(const std::vector<std::string>& selectors)
{
    return page_state_script_impl(selectors, {});
}

std::string page_state_script(const std::vector<std::string>& selectors,
                              const std::vector<std::string>& file_names)
{
    return page_state_script_impl(selectors, file_names);
}

bool parse_page_state(const std::string& json_text, PageState* out, std::string* error)
{
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "内部错误：PageState 输出为空";
        }
        return false;
    }
    *out = PageState();
    json parsed;
    try {
        parsed = json::parse(json_text);
    }
    catch (const std::exception&) {
        if (error != nullptr) {
            *error = "页面状态解析失败（脚本未回传 JSON；原始结果：" + snippet(json_text) + "）";
        }
        return false;
    }
    if (!parsed.is_object()) {
        if (error != nullptr) {
            *error = "页面状态形态不符（期望对象，实际：" + snippet(json_text) + "）";
        }
        return false;
    }
    out->url   = parsed.value("url", std::string());
    out->title = parsed.value("title", std::string());
    out->counts      = parse_count_pairs(parsed, "counts");
    // M8-37：站点无关自动判据的采样（缺这些字段 → 默认 0 / 空，不影响既有调用方）
    out->file_hits   = parse_count_pairs(parsed, "file_hits");
    out->attach_pool = parsed.value("attach_pool", 0);
    out->blob_images = parsed.value("blob_images", 0);
    return true;
}

int page_count(const PageState& state, const std::string& selector)
{
    for (const auto& entry : state.counts) {
        if (entry.first == selector) {
            return entry.second;
        }
    }
    return 0;
}

std::string match_page_evidence(const PageState& state, const UploadExpectations& expect)
{
    if (expect.page_selector.empty()) {
        return {}; // 站点单元没给页面期望 → 只能靠网络回执判定
    }
    const int count = page_count(state, expect.page_selector);
    if (count <= 0) {
        return {};
    }
    return "命中 " + std::to_string(count) + " 个（" + expect.page_selector + "）" +
           (expect.page_note.empty() ? std::string() : ("· " + expect.page_note));
}

// M8-37：**站点无关**的页面自动判据（声明值未命中时的兜底；不新增任何开关）
//  * 命中条件（任一）：① 交付文件名出现在页面正文 / alt / title / aria
//                      ② 附件候选池节点数 > 交件前基线（页面**新增**了附件区节点）
//                      ③ blob/data 缩略图数 > 交件前基线
//  * `baseline == nullptr`（没采到基线）→ 只保留 ①（不臆造「新增」）
//  * 返回空串 = 未取到自动证据（调用方据此判「缺页面证据」，绝不假装成功）
std::string match_page_evidence_auto(const PageState& state, const PageState* baseline)
{
    for (const auto& entry : state.file_hits) {
        if (entry.second > 0) {
            return "自动判据：页面出现交付文件名（" + entry.first + "）";
        }
    }
    if (baseline == nullptr) {
        return {};
    }
    if (state.attach_pool > baseline->attach_pool && state.attach_pool > 0) {
        return "自动判据：附件候选池节点 " + std::to_string(baseline->attach_pool) + " → " +
               std::to_string(state.attach_pool) + "（页面新增附件节点）";
    }
    if (state.blob_images > baseline->blob_images && state.blob_images > 0) {
        return "自动判据：缩略图 " + std::to_string(baseline->blob_images) + " → " +
               std::to_string(state.blob_images) + "（页面渲染出图片缩略图）";
    }
    return {};
}

bool upload_evidence_ready(const std::string& page_evidence, const std::string& net_evidence,
                           std::string* error)
{
    if (!page_evidence.empty() && !net_evidence.empty()) {
        return true;
    }
    if (error != nullptr) {
        if (page_evidence.empty() && net_evidence.empty()) {
            *error = "上传未取得任何证据（页面附件区没变化，也没有站点上传请求）—— 文件可能没被站点"
                     "接受：请确认站点格式 / 体积 / 张数限额，或先在浏览器里手动上传一次，确认该"
                     "账号当前能上传图片";
        }
        else if (page_evidence.empty()) {
            *error = "上传只有网络回执、**页面证据缺失**（附件区节点未出现）—— 页面可能尚未渲染完，"
                     "或 `web.attach_selector` 已变：请跑 --upload-selftest 复核该选择器";
        }
        else {
            *error = "上传只有页面证据、**网络回执缺失**（站点未发出上传请求）—— 文件很可能被站点"
                     "拒绝：请确认格式 / 体积 / 张数限额，并确认站点是否要求先点击上传按钮";
        }
    }
    return false;
}

int upload_evidence_selftest(int* passed_out)
{
    int passed = 0;
    int failed = 0;
    const auto check = [&passed, &failed](bool ok, const std::string& what) {
        if (ok) {
            ++passed;
            std::printf("  ✓ %s\n", what.c_str());
        }
        else {
            ++failed;
            std::printf("  ✗ %s\n", what.c_str());
        }
    };

    // ① URL 匹配
    check(UploadNetwork::url_matches_any("https://tos-hl-x.snssdk.com/upload/v1/b/x.png",
                                         {"tos-hl-x.snssdk.com/upload/v1/"}),
          "URL 含期望子串 → 命中");
    check(!UploadNetwork::url_matches_any("https://example.com/x", {"tos-hl-x.snssdk.com/"}),
          "URL 不含期望子串 → 不命中");
    check(!UploadNetwork::url_matches_any("https://example.com/x", {}), "无期望子串 → 不命中");

    // ② 网络证据
    {
        UploadNetwork network;
        network.records.push_back(NetRecord{"https://a/img.png", "GET", 200});
        network.records.push_back(NetRecord{"https://tos-hl-x.snssdk.com/upload/v1/b/x.png", "POST", 200});
        UploadExpectations expect;
        expect.net_url_contains = {"tos-hl-x.snssdk.com/upload/v1/"};
        const std::string evidence = network.evidence_for(expect);
        check(evidence.find("POST") != std::string::npos && evidence.find("200") != std::string::npos &&
                  evidence.find("命中 1 条") != std::string::npos,
              "网络证据：命中并给出方法 / 状态码 / 命中数");
        UploadExpectations other;
        other.net_url_contains = {"never.example.com"};
        check(network.evidence_for(other).empty(), "网络证据：未命中 → 空串");
        UploadExpectations none;
        check(network.evidence_for(none).empty(), "网络期望为空 → 空串（不由本层判定）");
        check(!network.summary().empty() && network.summary().find("2 条") != std::string::npos,
              "网络摘要含条数");
        UploadNetwork empty;
        check(empty.summary().find("0 条") != std::string::npos, "空网络采集也要讲清楚（0 条）");
        check(empty.evidence_for(expect).empty(), "空采集 → 无证据");
    }

    // ③ 页面证据（脚本生成 + 解析 + 判定）
    {
        const std::string script = page_state_script({".a", "input[type=file]"});
        check(script.find("querySelectorAll") != std::string::npos &&
                  script.find("input[type=file]") != std::string::npos,
              "页面状态脚本：选择器安全内联（JSON 数组）+ 只读采样");

        PageState   state;
        std::string error;
        const bool  parsed = parse_page_state(
            R"({"url":"https://a/b","title":"t","counts":[[".a",2],["input[type=file]",1],[".bad",-1]]})",
            &state, &error);
        check(parsed && error.empty() && state.url == "https://a/b" && state.counts.size() == 3,
              "页面状态解析：URL / 标题 / 计数全部取到");
        check(page_count(state, ".a") == 2 && page_count(state, ".absent") == 0 &&
                  page_count(state, ".bad") == -1,
              "页面计数：命中 / 未采集（0）/ 非法选择器（-1）");

        UploadExpectations expect;
        expect.page_selector = ".a";
        expect.page_note     = "附件区";
        check(match_page_evidence(state, expect).find("命中 2 个") != std::string::npos,
              "页面证据：命中 → 文案含命中数");
        expect.page_selector = ".absent";
        check(match_page_evidence(state, expect).empty(), "页面证据：未命中 → 空串");
        expect.page_selector.clear();
        check(match_page_evidence(state, expect).empty(), "页面期望为空 → 空串");

        PageState bad;
        check(!parse_page_state("not json at all", &bad, &error) && !error.empty(),
              "页面状态解析：非法 JSON → 失败 + 可读原因");
        error.clear();
        check(!parse_page_state("\"just a string\"", &bad, &error) && !error.empty(),
              "页面状态解析：形态不符 → 失败 + 可读原因");
        error.clear();
        check(parse_page_state("{}", &bad, &error) && bad.counts.empty(),
              "页面状态解析：缺 counts 也通过（计数为空）");
    }

    // ④ 组合判定（`I18`）
    {
        std::string error;
        check(upload_evidence_ready("命中 1 个", "POST 200 x", &error) && error.empty(),
              "双证据齐备 → 就绪");
        check(!upload_evidence_ready("", "POST 200 x", &error) &&
                  error.find("页面证据缺失") != std::string::npos,
              "缺页面证据 → 不就绪 + 指明缺哪侧");
        check(!upload_evidence_ready("命中 1 个", "", &error) &&
                  error.find("网络回执缺失") != std::string::npos,
              "缺网络回执 → 不就绪 + 指明缺哪侧");
        check(!upload_evidence_ready("", "", &error) && !error.empty(),
              "两侧都缺 → 不就绪 + 可操作文案");
    }

    // ⑤ M8-37（零开关）：**站点无关自动判据**（声明值未命中时的兜底；两侧都判，I18 不放宽）
    {
        // 页面侧：交付文件名出现 → 命中
        PageState with_name;
        with_name.file_hits.emplace_back("样例图.png", 1);
        check(match_page_evidence_auto(with_name, nullptr).find("文件名") != std::string::npos,
              "自动判据（页面）：交付文件名出现在页面 → 命中");

        // 页面侧：附件池 / 缩略图相对**交件前基线**增加 → 命中
        PageState base;
        base.attach_pool = 0;
        base.blob_images = 2;
        PageState after;
        after.attach_pool = 1;
        after.blob_images = 2;
        check(match_page_evidence_auto(after, &base).find("附件候选池") != std::string::npos,
              "自动判据（页面）：附件候选池节点增加 → 命中");
        after.attach_pool = 0;
        after.blob_images = 3;
        check(match_page_evidence_auto(after, &base).find("缩略图") != std::string::npos,
              "自动判据（页面）：缩略图增加 → 命中");
        PageState same;
        same.attach_pool = 0;
        same.blob_images = 2;
        check(match_page_evidence_auto(same, &base).empty(),
              "自动判据（页面）：与基线一致 → 空串（不假装成功）");
        check(match_page_evidence_auto(base, nullptr).empty(),
              "自动判据（页面）：无基线且无文件名 → 空串（不臆造「新增」）");

        // 网络侧：**写请求** + 通用特征词 → 命中；GET 一律不算
        UploadNetwork auto_net;
        auto_net.records.push_back(NetRecord{"https://x/files", "GET", 200});
        auto_net.records.push_back(NetRecord{"https://x/api/upload/v1?b=1", "POST", 200});
        check(auto_net.evidence_for_auto().find("写请求") != std::string::npos,
              "自动判据（网络）：POST + 通用上传特征词 → 命中（含「写请求」字样）");
        UploadNetwork only_get;
        only_get.records.push_back(NetRecord{"https://x/upload/doc.html", "GET", 200});
        check(only_get.evidence_for_auto().empty(),
              "自动判据（网络）：GET 命中特征词 → **不**算（避免页面请求误判）");
        UploadNetwork no_hint;
        no_hint.records.push_back(NetRecord{"https://x/api/chat", "POST", 200});
        check(no_hint.evidence_for_auto().empty(), "自动判据（网络）：无通用特征词 → 空串");

        // 采样脚本与解析的 M8-37 扩展字段
        const std::string script2 = page_state_script({".a"}, {"样例图.png"});
        check(script2.find("file_hits") != std::string::npos &&
                  script2.find("attach_pool") != std::string::npos &&
                  script2.find("样例图.png") != std::string::npos,
              "页面状态脚本：文件名与附件池采样已内联（JSON 安全转义）");
        PageState   ext;
        std::string ext_error;
        const bool  ext_ok = parse_page_state(
            R"({"url":"https://a/b","title":"t","counts":[[".a",1]],"file_hits":[["样例图.png",1]],"attach_pool":2,"blob_images":3})",
            &ext, &ext_error);
        check(ext_ok && ext.file_hits.size() == 1 && ext.attach_pool == 2 && ext.blob_images == 3,
              "页面状态解析：M8-37 采样字段（file_hits / attach_pool / blob_images）");
    }

    if (passed_out != nullptr) {
        *passed_out = passed;
    }
    std::printf("[上传证据自检] 通过 %d / 失败 %d\n", passed, failed);
    return failed;
}


} // namespace aiwrite::ai
