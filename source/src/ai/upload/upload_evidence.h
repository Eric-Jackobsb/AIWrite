#pragma once

// ============================================================================
//  上传证据层（`I18` 无证据不发送）—— M8-14
//
//  交付成功必须**两侧都成立**（缺任何一侧 → 不发送提示词、如实报错）：
//    * 页面证据：附件区渲染出的节点真的出现了（页面侧「挂上了」）
//    * 网络回执：站点自己的上传请求真的发出去了（数据侧「走出去了」）
//
//  为什么必须两侧（事故背景：M8 §3.1 + 网页版协议实测记录 §9）：
//    只注入文件（DOM.setFileInputFiles 成功）**不等于**上传被站点接受 —— 站点可能
//    在 change 事件里再校验一次（格式 / 大小 / 数量），失败时静默丢弃；只有网络回执
//    证明「站点确实发了上传请求」，页面证据证明「界面确实反映了这次上传」。
//
//  本层**全部是纯函数**（脚本生成 + 结果解析 + 判定），可离线断言：
//    * `page_state_script` 生成只读采样脚本（不改页面、不发送内容，只读 URL/标题/命中数）
//    * `parse_page_state` 解析 `web::run_script_sync` 回传的 JSON
//    * `UploadNetwork` 承载 `web::` 采到的网络记录，`evidence_for` 给出回执文案
//    * `upload_evidence_ready` 做最终组合判定
// ============================================================================

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "ai/upload/upload_contract.h"

namespace aiwrite::ai {

// ------------------------------------------------------------------ 网络回执 --

// 一条网络记录（由 `web::` 的 WebResourceResponseReceived 采集，站点无关）
struct NetRecord {
    std::string url;
    std::string method;   // GET / POST / …
    int         status = 0; // HTTP 状态码（0 = 未知）
    std::string line() const; // "POST 200 https://…"（报告 / 日志用）
};

// 网络记录集合（一次上传期间采集）
struct UploadNetwork {
    std::vector<NetRecord> records;

    std::size_t size() const { return records.size(); }
    void        clear() { records.clear(); }
    // URL 是否含任一子串（needles 空 → false）
    static bool url_matches_any(const std::string& url, const std::vector<std::string>& needles);
    // 命中期望里的任一子串 → 证据文案（"POST 200 tos-hl-x…（命中 1/3 条）"）；未命中 → 空
    std::string evidence_for(const UploadExpectations& expect) const;
    // M8-37：**站点无关**的自动判据 —— 交件后的 POST/PUT 且 URL 含通用上传特征词（未命中 → 空）
    //  * 供「站点单元未声明网络特征 / 声明特征未命中」时兜底（不新增开关；口径见 .cpp）
    std::string evidence_for_auto() const;
    // 全部记录的摘要（进报告：把「采集到几条」讲清楚，0 条也要讲出来）
    std::string summary() const;
};

// ------------------------------------------------------------------ 页面证据 --

// 页面状态（`page_state_script` 的解析结果）
struct PageState {
    std::string                              url;
    std::string                              title;
    std::vector<std::pair<std::string, int>> counts; // 选择器 → 命中数（-1 = 选择器非法）
    // ---- M8-37：站点无关自动判据的采样（只读）----
    std::vector<std::pair<std::string, int>> file_hits; // 交付文件名 → 页面出现次数（0/1）
    int                                      attach_pool = 0; // 附件候选池命中数（[class*=attach|upload|file]）
    int                                      blob_images = 0; // img[src^=blob:|data:] 计数（缩略图）
};

// 生成**只读**采样脚本：返回 `{url, title, counts, file_hits, attach_pool, blob_images}`
//  * 只读 document（不点击、不输入、不改 DOM）；`ExecuteScript` 会把返回值序列化为 JSON
//  * 选择器非法（`querySelectorAll` 抛错）→ 该条计数记 -1（如实反映，不静默当 0）
//  * M8-37：`file_names` 非空时一并采样「文件名是否出现在页面」（站点无关的页面证据兜底）
std::string page_state_script(const std::vector<std::string>& selectors);
std::string page_state_script(const std::vector<std::string>& selectors,
                              const std::vector<std::string>& file_names);

// 解析脚本结果（`json` = `web::run_script_sync` 回传的原始 JSON）
//  * 形态不符 → false + `*error` 含原因与原始片段（便于人工核对）
bool parse_page_state(const std::string& json, PageState* out, std::string* error);

// 某选择器的命中数（没采到过 → 0）
int page_count(const PageState& state, const std::string& selector);

// 命中页面期望 → 证据文案（"命中 2 个（<selector>）"）；未命中 → 空
std::string match_page_evidence(const PageState& state, const UploadExpectations& expect);

// M8-37：**站点无关**的页面自动判据（声明值未命中时的兜底；不新增开关）
//  * ① 交付文件名出现在页面 ② 附件候选池节点数 > 交件前基线 ③ blob/data 缩略图数 > 基线
//  * `baseline == nullptr` → 只保留 ①（不臆造「新增」）；未取到 → 空串（调用方判「缺页面证据」）
std::string match_page_evidence_auto(const PageState& state, const PageState* baseline);

// ------------------------------------------------------------------ 组合判定 --

// `I18`：两侧齐备才 true；否则 false + `*error` 写明**缺哪一侧**（可操作文案）
bool upload_evidence_ready(const std::string& page_evidence, const std::string& net_evidence,
                           std::string* error);

// 离线自检（返回失败项数；`passed_out` 回传通过项数）
int upload_evidence_selftest(int* passed_out = nullptr);

} // namespace aiwrite::ai
