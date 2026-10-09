#pragma once

// ============================================================================
//  每站上传单元的**入口声明**（M8-14）
//
//  * 每个站点 = 一个 `.cpp`（`ai/upload/sites/site_upload_<站>.cpp`），只导出**一个**入口
//  * 新增站点三步：① 新建该 .cpp ② 在本文件加一行声明 ③ 在 `upload_registry.cpp` 的
//    `site_uploads()` 里挂上（并把 `web.upload_adapter` 写进 providers.json 的该条目）
//  * 本文件**不含任何站点知识**（站点常量在各自的 .cpp 里 —— `I14`）
// ============================================================================

#include "ai/upload/upload_contract.h"

namespace aiwrite::ai {

// 豆包（doubao-web）：B1/B2 实测已拿到入口形态与上传链物证（`M8-` 记录见 docs）
const SiteUpload* site_upload_doubao();
// Kimi（kimi-web）：第二站，用于验证「模板只加一个站点单元即可通」
const SiteUpload* site_upload_kimi();

} // namespace aiwrite::ai
