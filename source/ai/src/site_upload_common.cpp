// ARCH-Z1 去重专项（v1.14）：`site_upload_common.h` 的实现（豆包 / Kimi 共用骨架）
//  * 行为与改造前逐字一致：本文件只是把两站 `.cpp` 里相同的 28 行收在一处
#include "site_upload_common.h"

#include "image_convert.h" // `ConvertOutcome` / `convert_for_upload`
#include "webview_host.h"  // `web::interactive_login_request` / `web::set_file_input_files`

namespace aiwrite::ai::site_upload {

bool prepare_for_upload(const std::string& local_path, const UploadPrepareOptions& options,
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

bool deliver_files(const UploadPlan& plan, const std::vector<std::string>& local_paths,
                   const char* attach_selector, int timeout_ms, std::string* detail,
                   std::string* error, const std::string& why_hint)
{
    const std::string selector =
        plan.site.attach_selector.empty() ? std::string(attach_selector) : plan.site.attach_selector;
    const web::LoginRequest request = web::interactive_login_request(plan.site, plan.provider_id);

    std::string injected;
    std::string why;
    if (!web::set_file_input_files(request, selector, local_paths, timeout_ms, &injected, &why)) {
        if (error != nullptr) {
            *error = "把文件交给页面失败：" + why + why_hint;
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

}  // namespace aiwrite::ai::site_upload
