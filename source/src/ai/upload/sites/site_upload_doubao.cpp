#include "ai/upload/site_uploads.h"

// ============================================================================
//  豆包（doubao-web）站点上传单元 —— M8-14
//
//  站点知识**只**在本文件里（`I14`）；公共契约见 `upload_contract.h`，传输原语见 `web/webview_host.h`。
//
//  实测依据（`source/python/_probe/out/m7b28_doubao_recon*.json`；见 docs/网页版协议实测记录.md §9）：
//    * B1（入口侦察）：豆包上传入口是**唯一**命中的隐藏 file input（`input.hidden`）；其
//      `accept` 串同时列了文档 / 音频（`.pdf/.docx/.mp3/.wav` …）与图片（`.png/.jpeg/.jpg/.webp`）
//      ⇒ 本站点单元只取**图片子集**，绝不把文档 / 音频当图传
//    * B2（上传链物证，四步）：① `/alice/resource/prepare_upload` ② `/top/v1?Action=ApplyImageUpload`
//      ③ `tos-hl-x.snssdk.com/upload/v1/<bucket>/<hash>.png`（TOS 直传）④ `/top/v1?Action=CommitImageUpload`
//      ⇒ 网络回执取 ③ ④ 两条（③ 是「字节真的出网了」，④ 是「站点真的认了」）
//
//  ⚠️ 本站点**不做**原生复刻：`s=` 短票据由页面内模块 `lHg` 产出（未在 34 个 chunk 中找到），
//     原生复刻站点一变即废。上传仍由**站点自己的页面 SDK** 完成，C++ 只负责注入 + 取证据。
// ============================================================================

#include "ai/upload/image_convert.h"
#include "web/site_ref.h"
#include "web/webview_host.h"

#include <string>
#include <vector>

namespace aiwrite::ai {
namespace {

// B1 实测：唯一的隐藏 file input（表字段 `web.attach_selector` 可覆盖）
constexpr const char* kAttachSelector = "input.hidden";
// B2 实测：附件区渲染模块 `chat-input-attachment-area` / `s1-chat-input-attachment-mixed-area`
//  * 用 `class*=` 前缀匹配（类名带构建哈希后缀）—— 换站点改**表 / 本文件**，不动引擎
constexpr const char* kAttachmentSelector = "[class*=\"chat-input-attachment-area\"]";
// B2 实测：TOS 直传 + Commit（URL 子串；站点换域名时改这里）
constexpr const char* kUploadUrlNeedles[] = {"tos-hl-x.snssdk.com/upload/v1/",
                                             "Action=CommitImageUpload"};
// 注入超时（CDP 三步很快；给足 20 秒应对页面卡顿）
constexpr int kDeliverTimeoutMs = 20000;

// B1 实测 accept 串里的**图片子集**（文档 / 音频已剔除）
std::vector<std::string> image_accept()
{
    return {"png", "jpg", "jpeg", "webp"};
}

bool doubao_prepare(const std::string& local_path, const UploadPrepareOptions& options,
                    std::string* out_path, std::string* note, std::string* error)
{
    const ConvertOutcome outcome = convert_for_upload(local_path, options);
    if (!outcome.ok) {
        if (error != nullptr) {
            *error = outcome.error;
        }
        return false;
    }
    if (out_path != nullptr) {
        *out_path = outcome.path;
    }
    if (note != nullptr) {
        *note = outcome.note;
    }
    return true;
}

bool doubao_deliver(const UploadPlan& plan, const std::vector<std::string>& local_paths,
                    std::string* detail, std::string* error)
{
    const std::string selector =
        plan.site.attach_selector.empty() ? std::string(kAttachSelector) : plan.site.attach_selector;
    const web::LoginRequest request = web::interactive_login_request(plan.site, plan.provider_id);

    std::string injected;
    std::string why;
    if (!web::set_file_input_files(request, selector, local_paths, kDeliverTimeoutMs, &injected,
                                   &why)) {
        if (error != nullptr) {
            *error = "把文件交给页面失败：" + why;
        }
        if (detail != nullptr) {
            *detail = "交付（CDP DOM.setFileInputFiles）：失败｜选择器 " + selector;
        }
        return false;
    }
    if (detail != nullptr) {
        *detail = "交付（CDP DOM.setFileInputFiles）：" + injected + "｜选择器 " + selector;
    }
    return true;
}

UploadExpectations doubao_expect(const ProviderWebSpec& /*site*/)
{
    UploadExpectations expect;
    // 页面证据：附件区节点出现（**必须**，否则 `I18` 判定不放行）
    expect.page_selector = kAttachmentSelector;
    expect.page_note     = "豆包输入框附件区（chat-input-attachment-area）出现节点";
    // 网络证据：TOS 直传 或 CommitImageUpload（任一命中即可）
    expect.net_url_contains = {kUploadUrlNeedles[0], kUploadUrlNeedles[1]};
    expect.net_note = "站点上传请求（TOS 直传 /top/v1?Action=CommitImageUpload）";
    return expect;
}

UploadPrepareOptions doubao_prepare_options(const ProviderWebSpec& /*site*/)
{
    UploadPrepareOptions options;
    options.accept     = image_accept();
    options.target_ext = "png"; // 白名单外 / 超限时统一转 PNG（无损、站点接受）
    options.max_edge   = 4096;  // 保守：豆包对边长无公开硬限；4096 足以保证视觉任务可用
    return options;
}

} // namespace

const SiteUpload* site_upload_doubao()
{
    static const SiteUpload unit = {"builtin:doubao",   "豆包网页版（doubao.com）",
                                    doubao_prepare,     doubao_deliver,
                                    doubao_expect,      doubao_prepare_options};
    return &unit;
}

} // namespace aiwrite::ai
