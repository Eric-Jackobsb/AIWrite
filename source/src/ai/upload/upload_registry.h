#pragma once

// ============================================================================
//  站点上传模板 —— 注册表 + 执行入口（M8-14）
//
//  注册表形态照既有先例：`ai/provider_spec.cpp` 的 `implemented_web_adapters()`
//  （表里有、程序没实现 → 运行期**明确报错**，不静默降级，`R12`）。
//
//  执行入口 `upload_images()` 是**唯一**对外入口，内部顺序（`I18`）：
//    ① 图片值 → 本地路径（`utils/asset_store`：`aiwrite-asset:` 令牌 + 旧绝对路径，`I19`）
//    ② 逐张**准备**：格式识别 → 按站点白名单/限额转码或缩放（`image_convert`）
//    ③ 开始采集网络回执（`web::begin_resource_capture`）
//    ④ **交件**：站点单元把文件交给页面（`web::set_file_input_files`）
//    ⑤ 轮询**双证据**：页面附件区节点出现 **且** 站点上传请求出现 → 才算成功
//       （两侧缺任一 → 返回失败 + 可操作原因；**绝不**在无证据时继续发送提示词）
//
//  离线可断言部分（`upload_registry_selftest`）：注册表 / 未实现文案 / 参数合并 / 契约完整性
//  / 无站点与空图片时的**早期失败路径**（都不碰网络与窗口）。
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

#include "ai/upload/upload_contract.h"

namespace aiwrite::ai {

// 本版本已实现的站点上传单元（例：{"builtin:doubao", "builtin:kimi"}）
std::vector<std::string> implemented_site_uploads();

// 站点单元表（注册点；每站一个入口见 `ai/upload/site_uploads.h`）
const std::vector<SiteUpload>& site_uploads();

// 按 id 取单元（空 / 未实现 → nullptr）
const SiteUpload* site_upload_for(const std::string& id);

// 未实现（或未声明）时的**可操作**文案；已实现 → 空串
//  * 空 id：条目没写 `web.upload_adapter`
//  * 有 id 但没实现：写明「已实现清单」+ 下一步（先在浏览器手动上传 / 改用文本链路）
std::string site_upload_error(const std::string& id);

// 表字段 `web.upload_adapter` → 单元（空 / 未实现 → nullptr；原因用 `site_upload_error`）
const SiteUpload* site_upload_of(const ProviderWebSpec& site);

// 生效的准备参数：站点单元常量 → 表字段覆盖（`web.upload_accept`）→ 调用方限额（max_bytes）
//  * `tmp_dir` 为空时补 `default_upload_tmp_dir()`
UploadPrepareOptions resolve_prepare_options(const SiteUpload& unit, const ProviderWebSpec& site,
                                             std::size_t max_bytes);

// 执行一次上传（**唯一入口**；顺序与铁律见文件头）
UploadResult upload_images(const UploadPlan& plan);

// 离线自检（返回失败项数；`passed_out` 回传通过项数）
int upload_registry_selftest(int* passed_out = nullptr);

} // namespace aiwrite::ai
