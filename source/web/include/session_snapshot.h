#pragma once

// ============================================================================
//  登录态快照（L2）的 **C++ 侧只读视图** —— `MB-D0-8` L2（M7B 批 2 · §6.2）
//
//  * **快照本体不在 C++**：DPAPI 加密 / 读写 / 回灌全在 Python
//    （`source/python/brain_ai_browser/session.py`；自检 `--session-selftest` · `VB2-39`）。
//    本组件只回答三件事：
//      ① **路径**（`~/.brain-ai/session/cookies.dat`，与 `paths::session_snapshot_dir()` 同源）
//      ② **触发时机**（口径常量，供 UI / 诊断显示：定时 5 s、登录即写、`shutdown` 关闭前再写）
//      ③ **状态展示**（存在 / 字节数 / 修改时间）—— **不解密、不读内容、不落日志**
//  * **红线**（`I23③`）：C++ 侧**永不**读取快照字节内容（明文与密文都不碰）——
//    只 `std::filesystem::exists` / `file_size` / `last_write_time`；「导出物零明文」在本层**天然成立**
//    （`VB2-39` 的 C++ 半：`summary()` 只有路径 / 大小 / 时间，**没有任何 Cookie 名或值**）
//  * **不喂 `SessionStore`**：快照与内存会话是两条线（`MB-D0-8`）；「是否已登录」仍走
//    `ai::web_session_state(...)`（判据 = 条目 `cookie_names` + 该 origin Cookie，`I15′`）
//  * 纯逻辑 + 只读元数据 ⇒ 可离线断言（`api_probe --exec-selftest`），**不启浏览器 / 不读密文**
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <string>

namespace aiwrite::web {

// 跨语言常量（与 Python `session.py` 同值；改一处必须改两处 —— 写进 §6.2 表）
inline constexpr const char* kSnapshotFileName      = "cookies.dat";  // `session.SNAPSHOT_NAME`
inline constexpr double      kSnapshotRefreshPeriod = 5.0;            // `session.REFRESH_INTERVAL_S`（< 10 s）
inline constexpr std::uintmax_t kSnapshotMaxBytes   = 1024u * 1024u;  // `session.MAX_SNAPSHOT_BYTES`（1 MiB）

// 快照路径（`~/.brain-ai/session/cookies.dat`；**只拼路径，不检查存在性**）
std::filesystem::path snapshot_file();

struct SnapshotInfo {
    bool          exists      = false;   // 文件是否存在
    std::uintmax_t bytes      = 0;       // 字节数（不存在 = 0）
    std::string   modified_at;           // "YYYY-MM-DD HH:MM:SS"（不存在 = 空）
    bool          in_size_limit = true;  // ≤ `kSnapshotMaxBytes`（超限 ⇒ Python 侧会当「损坏」；这里只展示）
};

// **只读元数据**（不打开文件内容）；任何异常 → `exists=false`（如实回报，不假装）
SnapshotInfo inspect_snapshot();

// 同上，但**指定路径** —— 供离线断言（可在临时目录造样本）与将来「多 profile」扩展
SnapshotInfo inspect_snapshot_at(const std::filesystem::path& path);

// 一行状态文案（UI / 诊断用）；**只含路径 / 大小 / 时间**（零 Cookie 信息）
std::string snapshot_summary(const SnapshotInfo& info);

// 触发时机口径（UI 帮助文本 / 文档同源；不参与运行逻辑）
std::string snapshot_trigger_note();

} // namespace aiwrite::web
