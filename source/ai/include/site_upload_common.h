// ARCH-Z1 去重专项（v1.14）：站点上传单元的**共用骨架**（doubao / kimi 两站逐字重复部分）
//  * 只放「与站点无关」的步骤：待上传文件预处理（`convert_for_upload` 三件套）+ CDP 交付
//  * 站点专属知识（选择器 / 证据 / 文案）仍留在各自 `site_upload_<站>.cpp`（`I14` 不破）
//  * 只允许 `ai/src/<站>/site_upload_<站>.cpp` include；**不是**公开 API
#pragma once

#include "upload_contract.h" // `UploadPlan` / `UploadPrepareOptions`

#include <string>
#include <vector>

namespace aiwrite::ai::site_upload {

// 待上传文件预处理：`convert_for_upload` 的 ok / path / note → 三件套出参
//  * 失败 ⇒ 填 `error`（可操作原因）并返回 false（`out_path` / `note` 不动）
bool prepare_for_upload(const std::string& local_path, const UploadPrepareOptions& options,
                        std::string* out_path, std::string* note, std::string* error);

// CDP 交付（`DOM.setFileInputFiles`）：选择器与超时由站点传入
//  * 失败 ⇒ `error = "把文件交给页面失败：" + why + why_hint`，`detail` 记「失败｜选择器 …」
//  * 成功 ⇒ `detail = "交付（CDP DOM.setFileInputFiles）：<injected>｜选择器 <sel>"`
bool deliver_files(const UploadPlan& plan, const std::vector<std::string>& local_paths,
                   const char* attach_selector, int timeout_ms, std::string* detail,
                   std::string* error, const std::string& why_hint = std::string());

}  // namespace aiwrite::ai::site_upload
