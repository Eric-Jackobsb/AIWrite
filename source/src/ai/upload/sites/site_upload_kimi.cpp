#include "ai/upload/site_uploads.h"

// ============================================================================
//  Kimi（kimi-web）站点上传单元 —— M8-14（**模板性验证站**）
//
//  目的：验证「模板 = 公共契约 + 每站独立实现」是否成立 —— 第二站应当**只加一个 .cpp**
//  就能复用全部基础设施（CDP 注入 / 格式转换 / 双证据判定）。
//
//  ⚠️ 当前状态：**入口侦察（B1/B2）尚未做**（需要人工登录一次 Kimi，见
//     `source/python/_probe/` 的 `m7b28_*` 同款探针；侦察结论应补写进
//     docs/网页版协议实测记录.md §10）。因此本文件的**选择器与网络特征都是待确认占位**：
//       * 占位一律**窄匹配**（`input[type=file]` / `[class*=...attachment...]`）+ 网络侧要求
//         真实 API 请求 → 认错时会**如实超时报错**（`I18` 不产生假成功）
//       * 首次真机跑通后，把确认值写进 `providers.json` 的 `kimi-web` 条目
//         （`web.attach_selector` / `web.upload_accept`）—— **不必改本文件**
//
//  实现上与本模板的豆包单元**逐字段对齐**（同一套 prepare / deliver / expect 契约）。
// ============================================================================

#include "ai/upload/image_convert.h"
#include "web/site_ref.h"
#include "web/webview_host.h"

#include <string>
#include <vector>

namespace aiwrite::ai {
namespace {

// 待侦察确认：Kimi 网页版的文件输入（多数站点是隐藏 input；表字段可覆盖）
constexpr const char* kAttachSelector = "input[type=file]";
// 待侦察确认：上传后的附件卡片容器（窄匹配，避免误命中模板节点）
constexpr const char* kAttachmentSelector = "[class*=\"attachment-item\"]";
// 待侦察确认：Kimi 上传走自家 API（域名级子串；真机按实际请求 URL 校正）
constexpr const char* kUploadUrlNeedles[] = {"kimi.com/api", "moonshot"};
constexpr int         kDeliverTimeoutMs   = 20000;

// 待侦察确认：Kimi 网页版接受的图片格式（先按主流取值；真机按 accept 串校正）
std::vector<std::string> image_accept()
{
    return {"png", "jpg", "jpeg", "webp", "gif"};
}

bool kimi_prepare(const std::string& local_path, const UploadPrepareOptions& options,
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

bool kimi_deliver(const UploadPlan& plan, const std::vector<std::string>& local_paths,
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
            *error = "把文件交给页面失败：" + why +
                     "（Kimi 的选择器仍是**待侦察占位** —— 请先跑一次入口侦察，把确认值填进"
                     " providers.json 的 kimi-web 条目）";
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

UploadExpectations kimi_expect(const ProviderWebSpec& /*site*/)
{
    UploadExpectations expect;
    expect.page_selector = kAttachmentSelector; // 待侦察确认
    expect.page_note     = "Kimi 输入框附件区出现节点（**待侦察确认**）";
    expect.net_url_contains = {kUploadUrlNeedles[0], kUploadUrlNeedles[1]}; // 待侦察确认
    expect.net_note = "Kimi 站点上传请求（**待侦察确认**）";
    return expect;
}

UploadPrepareOptions kimi_prepare_options(const ProviderWebSpec& /*site*/)
{
    UploadPrepareOptions options;
    options.accept     = image_accept();
    options.target_ext = "png";
    options.max_edge   = 4096;
    return options;
}

} // namespace

const SiteUpload* site_upload_kimi()
{
    static const SiteUpload unit = {"builtin:kimi",   "Kimi 网页版（kimi.com）",
                                    kimi_prepare,     kimi_deliver,
                                    kimi_expect,      kimi_prepare_options};
    return &unit;
}

} // namespace aiwrite::ai
