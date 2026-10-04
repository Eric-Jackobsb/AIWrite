#pragma once

// ============================================================================
//  管道协议 v1 帧（`docs/actionPlan/M7B.md` §6.1）—— C++ 侧**纯函数**实现
//
//  * 无 IO、无第三方状态 ⇒ 可**离线断言**（`VB2-29` / `VB2-32`）
//  * 词表**唯一来源** = §6.1；Python 侧同构实现见 `source/python/brain_ai_browser/protocol.py`
//  * 帧 = UTF-8 JSON 一行一帧；必带 `v` / `id` / `kind`（cmd | evt | err）
//  * **非法行**（JSON 不合法 / 缺 `v` 或 `kind` / `v` 不识别 / 超上限）：`parse_frame` 置
//    `bad = true` —— 调用方须**丢弃 + 记日志 + 回 `err {bad_frame}`**，
//    **不得**让守护进程退出
//  * 容量写死：单帧 ≤ 64 KiB、`images[]` ≤ 8（**图片一律传路径**，不传 base64）
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

namespace aiwrite::web::channel {

inline constexpr int         kProtoVersion      = 1;
inline constexpr std::size_t kMaxFrameBytes     = 64 * 1024;
inline constexpr std::size_t kMaxImages         = 8;
inline constexpr int         kTimeoutInjectMs   = 30000;   // 注入 / 上传
inline constexpr int         kTimeoutEvidenceMs = 60000;   // 上传完成证据（P7b-11）
inline constexpr int         kTimeoutAnswerMs   = 120000;  // 回答
inline constexpr int         kShutdownGraceMs   = 5000;    // I23①：close → 等进程退出

struct Frame {
    bool        bad = true;    // true = 非法行
    std::string reason;        // bad = true 时的可操作原因（日志 / err.hint）
    int         v = 0;         // 协议版本
    std::string kind;          // cmd | evt | err
    std::string id;            // 请求-响应配对；事件帧 "-"
    std::string name;          // 命令名 / 事件名 / 错误码
    std::string payload_json;  // 其余字段（JSON 对象文本；便于原样转发）
};

// 解析一行（含上限校验；**不抛异常**）
Frame parse_frame(const std::string& line);

// 生成（`fields_json` 必须是 JSON 对象文本；空串 = `{}`）
std::string make_command(const std::string& name, const std::string& id,
                         const std::string& fields_json = "{}");
std::string make_event(const std::string& name, const std::string& id,
                       const std::string& fields_json = "{}");
std::string make_error(const std::string& code, const std::string& id,
                       const std::string& hint = "");

// 词表（§6.1）
const std::vector<std::string>& known_commands();   // 7 条
const std::vector<std::string>& known_events();     // 6 条
std::vector<std::string>        required_fields_of(const std::string& command);
bool                            is_known_command(const std::string& name);

// 命令帧校验：通过 → true；否则 false 且 *reason 为**可操作原因**
//（「未知命令 …」/「缺少字段 …」—— 文案与 Python 侧同构，便于对照断言）
bool validate_command(const Frame& frame, std::string* reason);

// 单帧字节上限（UTF-8 计）
bool frame_within_limit(const std::string& line);

// `delta` 事件载荷 → 文本（形态 A：`text`；形态 B：`delta.text`）；
// 无可用增量 → 空串 + *explicit_non_stream = true
//（`I22`：流式降级**必须显式标注**「非流式（轮询）」，不许安静改变行为）
std::string delta_text_of_frame(const std::string& payload_json, bool* explicit_non_stream);

const char* error_code_bad_frame();   // "bad_frame"

} // namespace aiwrite::web::channel
