#include "ai/upload/upload_registry.h"

#include "ai/upload/image_convert.h"
#include "ai/upload/site_uploads.h"
#include "ai/upload/upload_evidence.h"
#include "utils/asset_store.h"
#include "utils/log.h"
#include "web/site_ref.h"
#include "web/webview_host.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

namespace aiwrite::ai {
namespace {

// M8-14 诊断 / M8-37 自动识别：**只读**枚举页面里的 `input[type=file]`
//  * 单一实现：复用 `web::resolve_file_input_selector`（枚举 + 取值策略 + 可回填清单）
//  * 只在已建立会话 / 已打开页面的前提下调用；失败一律吞掉（绝不因诊断再抛错）
std::string file_input_diagnosis(const web::LoginRequest& site_request)
{
    std::string selector;
    std::string detail;
    std::string why;
    if (!web::resolve_file_input_selector(site_request, 10000, &selector, &detail, &why)) {
        return detail; // 取不到候选时 detail 仍是清单（含「共 0 个」），没有则空串
    }
    return detail;
}

constexpr int kEvidencePollMs = 700;      // 证据轮询间隔
constexpr int kMinTimeoutMs   = 20000;    // 单次上传最少给 20 秒（站点直传可能很慢）
constexpr int kMaxTimeoutMs   = 600000;   // 上限 10 分钟（R13：有硬上限）
constexpr int kScriptPollMs   = 15000;    // 单次页面采样脚本超时

int clamp_timeout(int timeout_ms)
{
    if (timeout_ms < kMinTimeoutMs) {
        return kMinTimeoutMs;
    }
    return timeout_ms > kMaxTimeoutMs ? kMaxTimeoutMs : timeout_ms;
}

std::string join_id_list(const std::vector<std::string>& ids)
{
    std::string out;
    for (const std::string& id : ids) {
        if (!out.empty()) {
            out += " / ";
        }
        out += id;
    }
    return out;
}

std::string join_values(const std::vector<std::string>& values, std::size_t limit = 3)
{
    if (values.empty()) {
        return "（空）";
    }
    std::string out;
    const std::size_t shown = values.size() > limit ? limit : values.size();
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0) {
            out += " / ";
        }
        out += values[i];
    }
    if (values.size() > shown) {
        out += " / …（共 " + std::to_string(values.size()) + " 项）";
    }
    return out;
}

std::string file_name_of(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}

} // namespace

const std::vector<SiteUpload>& site_uploads()
{
    static const std::vector<SiteUpload> units = [] {
        std::vector<SiteUpload> out;
        const SiteUpload* const registrars[] = {site_upload_doubao(), site_upload_kimi()};
        for (const SiteUpload* unit : registrars) {
            if (unit != nullptr && unit->id != nullptr) {
                out.push_back(*unit);
            }
        }
        return out;
    }();
    return units;
}

std::vector<std::string> implemented_site_uploads()
{
    std::vector<std::string> ids;
    for (const SiteUpload& unit : site_uploads()) {
        ids.push_back(unit.id);
    }
    return ids;
}

const SiteUpload* site_upload_for(const std::string& id)
{
    if (id.empty()) {
        return nullptr;
    }
    for (const SiteUpload& unit : site_uploads()) {
        if (id == unit.id) {
            return &unit;
        }
    }
    return nullptr;
}

std::string site_upload_error(const std::string& id)
{
    if (id.empty()) {
        return "该站点条目未声明 `web.upload_adapter`（图片上传需要它）—— 请在 assets/providers.json"
               " 的该条目 `web` 段加上 `upload_adapter`（已实现：" +
               join_id_list(implemented_site_uploads()) + "）";
    }
    if (site_upload_for(id) != nullptr) {
        return {};
    }
    return "web.upload_adapter=" + id + " 本版本尚未实现（已实现：" +
           join_id_list(implemented_site_uploads()) +
           "）—— 该站点暂不能自动上传图片：可先在浏览器里手动上传，或改用纯文本链路";
}

const SiteUpload* site_upload_of(const ProviderWebSpec& site)
{
    return site_upload_for(site.upload_adapter);
}

UploadPrepareOptions resolve_prepare_options(const SiteUpload& unit, const ProviderWebSpec& site,
                                             std::size_t max_bytes)
{
    UploadPrepareOptions options;
    if (unit.prepare_options != nullptr) {
        options = unit.prepare_options(site);
    }
    if (!site.upload_accept.empty()) {
        // 表字段 = **可覆盖项**（站点 accept 串随版本变，不必改代码）
        const std::vector<std::string> from_table = image_exts_from_accept(site.upload_accept);
        if (!from_table.empty()) {
            options.accept = from_table;
        }
    }
    if (max_bytes > 0) {
        options.max_bytes = max_bytes;
    }
    if (options.tmp_dir.empty()) {
        options.tmp_dir = default_upload_tmp_dir();
    }
    return options;
}

UploadResult upload_images(const UploadPlan& plan)
{
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&start] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
            .count();
    };

    UploadResult       result;
    const SiteUpload* unit = site_upload_of(plan.site);
    if (unit == nullptr) {
        result.error = site_upload_error(plan.site.upload_adapter);
        result.steps = "未找到站点上传单元（web.upload_adapter=" + plan.site.upload_adapter + "）";
        return result;
    }
    if (unit->deliver == nullptr) {
        result.error = std::string("站点上传单元 ") + unit->id +
                       " 未实现「交件」步骤 —— 本版本不能自动上传图片";
        result.steps = "交件步骤缺失（SiteUpload::deliver = nullptr）";
        return result;
    }

    // ① 图片值 → 本地路径（`aiwrite-asset:` 令牌 + 旧绝对路径，`I19`）
    std::vector<std::string> problems;
    const std::vector<std::string> values =
        aiwrite::asset::to_local_paths(plan.image_values, &problems);
    if (values.empty()) {
        result.error = "没有可用的图片值（" + join_values(plan.image_values) +
                       "）—— 请在图片节点选择图片，或在面板里把旧路径迁移到资源目录";
        result.steps = "图片值解析为空";
        return result;
    }

    // ② 逐张准备：格式识别 → 白名单 / 限额 → 必要时缩放 + 转码
    const UploadPrepareOptions options = resolve_prepare_options(*unit, plan.site, plan.max_bytes);
    std::vector<std::string>   delivered;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i].empty()) {
            result.error = "第 " + std::to_string(i + 1) + " 张图片取不到文件" +
                           (problems.empty() ? std::string()
                                             : ("（" + join_values(problems, 2) + "）")) +
                           " —— 令牌可能已失效：见 ~/.brain-ai/assets/images";
            result.steps = "图片值解析失败";
            return result;
        }
        const ConvertOutcome converted = convert_for_upload(values[i], options);
        if (!converted.ok) {
            result.error = "第 " + std::to_string(i + 1) + " 张图片准备失败：" + converted.error;
            result.steps = "准备阶段失败";
            return result;
        }
        if (converted.converted && !converted.note.empty()) {
            result.conversions.push_back(file_name_of(values[i]) + "：" + converted.note);
        }
        delivered.push_back(converted.path);
    }
    result.delivered_paths = delivered;

    // ③ 证据期望（站点单元给出）；**无页面期望 → 直接失败**（`I18` 要求两侧都可判）
    UploadExpectations expect;
    if (unit->expect != nullptr) {
        expect = unit->expect(plan.site);
    }
    if (expect.page_selector.empty()) {
        result.error = std::string("站点上传单元 ") + unit->id +
                       " 未声明**页面证据选择器**（附件区）—— 不符合「无证据不发送」（`I18`）："
                       "请在站点单元里补上 `page_selector`";
        result.steps = "期望缺失（page_selector 为空）";
        return result;
    }
    if (expect.net_url_contains.empty()) {
        log::warn(std::string("[图片上传] 站点单元未声明网络回执特征 —— 只能靠页面证据判定（") +
                  unit->id + "）");
    }

    // ④ 开始采集网络回执（**交件前**采一次页面基线）→ 交件
    //  * M8-37：声明值未命中 → **只读自动识别** attach_selector 后**只重试一次**
    //    （口径①：只打印不落盘 —— 识别结果进日志 / steps，不改 providers.json）
    const web::LoginRequest site_request = web::with_owner(
        web::interactive_login_request(plan.site, plan.provider_id), plan.owner);
    std::vector<std::string> file_names;
    for (const std::string& path : delivered) {
        file_names.push_back(file_name_of(path));
    }
    web::begin_resource_capture();

    // 交件前的页面基线（供「页面新增」自动判据；采样失败静默 —— 诊断绝不干扰上传）
    PageState baseline_page;
    bool      have_baseline = false;
    const std::string probe_script = page_state_script({expect.page_selector}, file_names);
    {
        std::string base_json;
        std::string base_error;
        if (web::run_script_sync(site_request, probe_script, kScriptPollMs, &base_json, &base_error)) {
            std::string parse_error;
            have_baseline = parse_page_state(base_json, &baseline_page, &parse_error);
        }
    }

    std::string detail;
    std::string deliver_error;
    bool        delivered_ok = unit->deliver(plan, delivered, &detail, &deliver_error);
    if (!delivered_ok) {
        const bool missed = deliver_error.find("未命中") != std::string::npos ||
                            deliver_error.find("无匹配") != std::string::npos;
        std::string auto_selector;
        std::string auto_detail;
        std::string auto_error;
        if (missed && web::resolve_file_input_selector(site_request, kScriptPollMs, &auto_selector,
                                                       &auto_detail, &auto_error)) {
            log::info("[图片上传] attach_selector 未命中 → 自动识别选择器（本次运行生效）：\n" +
                      auto_detail);
            UploadPlan retry              = plan;
            retry.site.attach_selector    = auto_selector;
            std::string retry_detail;
            std::string retry_error;
            if (unit->deliver(retry, delivered, &retry_detail, &retry_error)) {
                delivered_ok = true;
                detail       = retry_detail + "｜自动识别 attach_selector：" + auto_selector;
            }
            else {
                deliver_error = retry_error + "（自动识别为 " + auto_selector + " 后仍失败）";
            }
        }
        else if (missed && !auto_error.empty()) {
            deliver_error += "；自动识别 attach_selector 也不成：" + auto_error;
        }
    }
    if (!delivered_ok) {
        result.error = "把图片交给页面失败：" + deliver_error;
        // M8-14 诊断：选择器没命中时，把页面里**实际的 file input** 枚举出来（只读；失败静默）
        //  * 目的：新站点接入时不必靠人肉 F12 —— 报错里直接给出「建议选择器」
        if (deliver_error.find("未命中页面元素") != std::string::npos ||
            deliver_error.find("未命中") != std::string::npos) {
            const std::string diagnosis = file_input_diagnosis(site_request);
            if (!diagnosis.empty()) {
                result.error += "\n  —— 诊断：\n    " + diagnosis +
                                "\n  修法：把命中的选择器填进 providers.json 该条目的 web.attach_selector"
                                "（不必改 C++；不填则每次运行都会自动识别），再跑 --upload-selftest "
                                "--provider " + plan.provider_id + " 复核";
            }
        }
        result.steps = detail;
        return result;
    }

    // ⑤ 等**双证据**（页面附件区 + 站点上传请求）
    //  * M8-37：声明判据之外补**站点无关自动判据**（文件名出现 / 页面新增附件节点 / 新增缩略图；
    //    网络侧 = 交件后的 POST/PUT 且 URL 含通用上传特征词）—— 两侧仍缺一不放行（`I18` 不变）
    const int timeout_ms = clamp_timeout(plan.timeout_ms);

    UploadNetwork network;
    std::string   page_evidence;
    std::string   net_evidence;
    std::string   last_issue;
    int           polls = 0;

    while (true) {
        ++polls;
        // 网络回执（增量取走；清空缓冲由 `begin_resource_capture` 负责）
        for (const web::WebResourceRecord& record : web::take_resource_capture()) {
            network.records.push_back(NetRecord{record.url, record.method, record.status});
        }
        net_evidence = network.evidence_for(expect);
        if (net_evidence.empty()) {
            net_evidence = network.evidence_for_auto(); // 站点无关兜底（写请求 + 通用特征词）
        }

        // 页面证据（只读采样；失败只记录，不中断 —— 页面可能刚好在重渲染）
        std::string state_json;
        std::string script_error;
        if (web::run_script_sync(site_request, probe_script, kScriptPollMs, &state_json,
                                 &script_error)) {
            PageState   state;
            std::string parse_error;
            if (parse_page_state(state_json, &state, &parse_error)) {
                page_evidence = match_page_evidence(state, expect);
                if (page_evidence.empty()) {
                    page_evidence =
                        match_page_evidence_auto(state, have_baseline ? &baseline_page : nullptr);
                }
                last_issue.clear();
            }
            else {
                last_issue = parse_error;
            }
        }
        else {
            last_issue = script_error;
        }

        std::string ready_error;
        if (upload_evidence_ready(page_evidence, net_evidence, &ready_error)) {
            break;
        }
        if (elapsed() >= timeout_ms) {
            result.ok             = false;
            result.page_evidence  = page_evidence;
            result.net_evidence   = net_evidence;
            result.error          = ready_error + "（已等 " + std::to_string(timeout_ms / 1000) +
                           " 秒 / " + std::to_string(polls) + " 次轮询）" +
                           (last_issue.empty() ? std::string()
                                               : ("；最近一次页面采样问题：" + last_issue)) +
                           "｜网络采集：" + network.summary();
            result.steps = detail + "｜超时于 " + std::to_string(elapsed()) + " ms";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kEvidencePollMs));
    }

    result.ok            = true;
    result.page_evidence = page_evidence;
    result.net_evidence  = net_evidence;
    result.steps = detail + "｜证据轮询 " + std::to_string(polls) + " 次 / 耗时 " +
                   std::to_string(elapsed()) + " ms｜本页网络采集：" + network.summary();
    log::info(std::string("[图片上传] 成功：") + unit->id + "｜" + result.page_evidence + "｜" +
              result.net_evidence);
    return result;
}

int upload_registry_selftest(int* passed_out)
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

    // ① 注册表
    const std::vector<std::string> ids = implemented_site_uploads();
    bool has_doubao = false;
    bool has_kimi   = false;
    for (const std::string& id : ids) {
        has_doubao = has_doubao || id == "builtin:doubao";
        has_kimi   = has_kimi || id == "builtin:kimi";
    }
    check(has_doubao && has_kimi, "注册表含 builtin:doubao 与 builtin:kimi");
    check(site_upload_for(std::string("builtin:doubao")) != nullptr, "按 id 取单元（doubao）");
    check(site_upload_for(std::string()) == nullptr, "空 id → nullptr");
    check(site_upload_for(std::string("builtin:nope")) == nullptr, "未知 id → nullptr");

    // ② 未实现 / 未声明 → 可操作文案
    check(site_upload_error(std::string("builtin:doubao")).empty(), "已实现 → 无错误文案");
    check(site_upload_error(std::string()).find("upload_adapter") != std::string::npos,
          "未声明 upload_adapter → 文案指向该字段");
    const std::string unknown_error = site_upload_error(std::string("builtin:nope"));
    check(unknown_error.find("尚未实现") != std::string::npos &&
              unknown_error.find("已实现") != std::string::npos &&
              unknown_error.find("手动上传") != std::string::npos,
          "未实现 → 文案含「尚未实现」+ 已实现清单 + 下一步");

    // ③ 契约完整性（每个单元四件事都要有；页面/网络期望都要能判）
    ProviderWebSpec      blank_site;
    const SiteUpload* const units[] = {site_upload_for(std::string("builtin:doubao")),
                                       site_upload_for(std::string("builtin:kimi"))};
    for (const SiteUpload* unit : units) {
        if (unit == nullptr) {
            check(false, "单元缺失（内部错误）");
            continue;
        }
        const std::string tag = std::string("单元 ") + unit->id;
        check(unit->display != nullptr && *unit->display != '\0', tag + "：有显示名");
        check(unit->prepare != nullptr && unit->deliver != nullptr && unit->expect != nullptr &&
                  unit->prepare_options != nullptr,
              tag + "：四个函数指针齐全");
        const UploadExpectations expect = unit->expect(blank_site);
        check(!expect.page_selector.empty(), tag + "：声明了页面证据选择器（I18 可判）");
        check(!expect.net_url_contains.empty(), tag + "：声明了网络回执特征");
    }

    // ④ 准备参数合并（表覆盖 → 调用方限额 → 临时目录兜底）
    if (const SiteUpload* doubao = site_upload_for(std::string("builtin:doubao"))) {
        ProviderWebSpec site;
        const UploadPrepareOptions defaults = resolve_prepare_options(*doubao, site, 0);
        check(!defaults.accept.empty() && !defaults.tmp_dir.empty(),
              "站点单元给出默认白名单 + 临时目录");
        site.upload_accept = ".jpg, .WEBP, .pdf, .mp3"; // 非图片混入 → 只留图片
        const UploadPrepareOptions merged = resolve_prepare_options(*doubao, site, 1234);
        check(merged.accept == std::vector<std::string>({"jpg", "webp"}),
              "表字段覆盖白名单（非图片被丢弃）");
        check(merged.max_bytes == 1234, "调用方限额覆盖站点默认");
    }

    // ⑤ 早期失败路径（**不碰网络 / 不碰窗口**）
    {
        UploadPlan plan;
        plan.image_values = {"aiwrite-asset:deadbeef"};
        const UploadResult no_adapter = upload_images(plan);
        check(!no_adapter.ok && no_adapter.error.find("upload_adapter") != std::string::npos,
              "未声明 adapter → 早期失败 + 指名该字段");

        plan.site.upload_adapter = "builtin:nope";
        const UploadResult unknown = upload_images(plan);
        check(!unknown.ok && unknown.error.find("尚未实现") != std::string::npos,
              "未实现 adapter → 早期失败 + 可操作文案");

        plan.site.upload_adapter = "builtin:doubao";
        plan.image_values.clear();
        const UploadResult no_image = upload_images(plan);
        check(!no_image.ok && no_image.error.find("没有可用的图片值") != std::string::npos,
              "无图片值 → 早期失败（不起窗口、不注入）");
    }

    if (passed_out != nullptr) {
        *passed_out = passed;
    }
    std::printf("[站点上传模板自检] 通过 %d / 失败 %d\n", passed, failed);
    return failed;
}



} // namespace aiwrite::ai
