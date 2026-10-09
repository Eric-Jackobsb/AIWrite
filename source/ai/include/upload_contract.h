#pragma once

// ============================================================================
//  站点上传模板 —— 公共契约（M8-14 · 方案 E「每站独立上传函数」的骨架）
//
//  背景（docs/actionPlan/M8.md §3.1 + docs/网页版协议实测记录.md §9）：
//    * 豆包上传链 = ①prepare_upload ②ApplyImageUpload(`s=` 短票据) ③TOS 直传 ④Commit；
//      `s=` 由页面内模块 `lHg` 产出（**未在 34 个 chunk 中找到**）⇒ 原生复刻代价高、站点
//      一变即废（`R1` 反面教训）。
//    * 因此本模板**不做原生复刻**：C++ 只负责「选文件 / 交件 / 要证据」，
//      真正的上传仍由**站点自己的页面 SDK** 完成（WebView2 会话内），C++ 只做**注入与判定**。
//
//  分层（只有这三层，职责不重叠）：
//    ① 公共契约（本文件）：**站点无关**的数据结构与每站实现的函数指针签名
//    ② 每站独立实现（`ai/upload/sites/site_upload_*.cpp`）：选择器 / 白名单 / 判据 / 转换策略
//       —— **站点知识只允许出现在这里**（`I14`：站点知识不散落到引擎）
//    ③ 传输原语（`web/webview_host.*`）：CDP 文件注入 + 网络回执采集（站点无关）
//
//  不变量（与 M7/M8 同一套）：
//    * `I18` **无证据不发送**：页面证据 与 网络回执 **两者齐备**才算交付成功；
//      只有其一 → 不发送提示词，如实报错（绝不静默降级、绝不假装成功）
//    * `I21` 失败文案可操作：每条 error 必须含「谁、卡在哪、下一步做什么」
//    * `I19` 图片值兼容：`aiwrite-asset:` 令牌与旧绝对路径**都**要能取到图（解析在 `utils/asset_store`）
//    * `R2` 回退仍在：本模板是**独立步骤**，失败不影响文本链路与既有 `adapter=dom` 能力
//
//  离线可断言（`api_probe --upload-selftest`）：本文件只有数据结构与签名，无 IO、无站点常量。
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

#include "ai/provider_spec.h" // ProviderWebSpec
#include "web/web_owner.h"    // M_patchC（PW-01）：归属（纯数据；无 Win32 依赖）

namespace aiwrite::ai {

// ---------------------------------------------------------------- 期望（判据） --

// 站点交付期望：**每站单元**依据实测物证填写（页面证据 + 网络回执）
struct UploadExpectations {
    // 页面证据：附件区渲染节点选择器（命中数 > 0 = 页面已挂上文件）
    std::string              page_selector;
    std::string              page_note;         // 人话（进报告：这个选择器代表什么）
    // 网络证据：URL 含任一子串即算命中（例：tos-hl-x.snssdk.com/upload/v1/）
    std::vector<std::string> net_url_contains;
    std::string              net_note;          // 人话（进报告：这一步是哪一环）
};

// ---------------------------------------------------------------- 准备（转换） --

// 准备阶段参数：站点白名单 / 限额（由 `SiteUpload::prepare_options` 或表字段给出）
struct UploadPrepareOptions {
    std::vector<std::string> accept;       // 图片扩展名白名单（小写、无点；空 = 不限制）
    std::size_t              max_bytes = 0; // 站点体积上限（0 = 不限；超限先缩放再转码）
    int                      max_edge  = 0; // 长边像素上限（0 = 不缩放）
    std::string              tmp_dir;       // 空 = %TEMP%\aiwrite-upload
    std::string              target_ext = "png"; // 需要转码时的目标格式（png | jpg）
};

// ---------------------------------------------------------------- 计划 / 结果 --

// 一次上传计划：调用方（引擎 / CLI）**只**给站点无关的东西
struct UploadPlan {
    ProviderWebSpec          site;              // 表字段（选择器 / done_when / 登录页 …）
    std::string              provider_id;       // 条目 id（日志 / 站点键归属）
    std::vector<std::string> image_values;      // 图片值：`aiwrite-asset:<摘要>` 或绝对路径
    std::size_t              max_bytes  = 0;    // 表 / 条目限额（0 = 用站点单元默认）
    int                      timeout_ms = 180000;
    // ---- M_patchC（`PW-01` · 不变量 `I26`）：**归属**（证据 / 上传槽按 owner 归属）----
    web::WebOwner            owner;
};

// 一次上传结果（**双证据**；`I18`）
struct UploadResult {
    bool                     ok = false;        // true 仅当 页面证据 && 网络回执 都成立
    std::string              page_evidence;     // 例：命中 2 个（.chat-input-attachment-area）
    std::string              net_evidence;      // 例：tos-hl-x.snssdk.com/upload/v1/（status 200）
    std::vector<std::string> delivered_paths;   // 实际交付给页面的**本地路径**（可能是转码后的临时文件）
    std::vector<std::string> conversions;       // 每个转换一行（"webp→png（站点白名单不含 webp）"）
    std::string              steps;             // 诊断摘要（交付方式 / 证据轮询次数 / 耗时）
    std::string              error;             // ok=false 时的**可操作**原因（`I21`）
};

// ---------------------------------------------------------------- 每站实现单元 --

// 站点上传单元：**每站一个**（照 `builtin:deepseek` 适配器的注册表先例）
//  * 三个函数指针都可为空 —— 空表示「该站不支持这一步」（聚合器会给出可操作文案，不崩）
struct SiteUpload {
    const char* id      = nullptr; // "builtin:doubao"
    const char* display = nullptr; // "豆包网页版"

    // ① 准备：格式识别 / 转换（站点白名单 + 限额）。未转换时 *out_path = 入参
    bool (*prepare)(const std::string& local_path, const UploadPrepareOptions& options,
                    std::string* out_path, std::string* note, std::string* error) = nullptr;

    // ② 交件：把文件交给页面（站点专属选择器 / 是否需要先点「上传」按钮）
    bool (*deliver)(const UploadPlan& plan, const std::vector<std::string>& local_paths,
                    std::string* detail, std::string* error) = nullptr;

    // ③ 判据：本轮的页面 / 网络期望（来自实测物证）
    UploadExpectations (*expect)(const ProviderWebSpec& site) = nullptr;

    // ④ 站点默认准备参数（代码常量；表字段可覆盖 —— `upload_accept` / 限额）
    UploadPrepareOptions (*prepare_options)(const ProviderWebSpec& site) = nullptr;
};

} // namespace aiwrite::ai
