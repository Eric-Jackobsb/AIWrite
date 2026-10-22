# 审计报告 · audit_cleanup_05 —— CLN-05 重复的辅助函数

> 批次：`CLN-05`（对应 [../actionPlan/M_code_cleanup.md](../actionPlan/M_code_cleanup.md) §3.1）· 日期：2026-10-20
> 性质：**删除前置审计闸门（§3.0）**的产物 —— 本批清点出 **5 组真重复辅助函数**（12 文件 · 14 处重复定义），收敛方案 = **新建 `utils/string_util.{h,cpp}` 单一来源** + 各 TU 改调共享实现 + 删本地定义。
> 范围：`source/**`（163 个 `.cpp/.h`，非 `third_party` / `source/python`）。
> 方法：全库同名函数清点（脚本提取 → 人工逐行比对）+ **逐行一致判定**（Trim-归一化精确 diff）+ 调用点全量取证。

---

## ① 对象清点（重复定义候选 · 收敛对象）

全库扫描 `source/**`（排除 `third_party`/`python`），提取全部文件局部辅助函数定义（匿名命名空间 / 顶格定义），按「同名跨文件」归组，**逐行比对函数体**后分三类：

### A. 真重复（5 组 · 14 处定义 · 收敛对象）

| # | 函数 | 重复份数 | 定义位置 | 语义 | 函数体一致性 |
|---|---|---|---|---|---|
| A1 | `find_ci` / `ce_find_ci` | 2 | `ui/src/node_library.cpp:22` ↔ `ui/src/composite_editor.cpp:90` | 大小写不敏感子串查找（ASCII 折叠；中文原样字节比较） | ✅ 26 行（签名+函数体）仅函数名不同，其余逐行一致 |
| A2 | `trim` | 5 | `ai/src/deepseek/deepseek_web_client.cpp:101` · `ai/src/http_net.cpp:21` · `ai/src/provider_spec.cpp:25` · `ai/src/site_template.cpp:19` · `engine/src/graph.cpp:21` | 去首尾空白（`" \t\r\n"`，全空返 `{}`） | ✅ 5 处两两 IDENTICAL |
| A3 | `now_string` | 3 | `engine/src/autosave.cpp:30` · `engine/src/recent_files.cpp:19` · `web/src/session_store.cpp:12` | 本地时间戳 `%Y-%m-%d %H:%M:%S`（`localtime_s`+`strftime`） | ✅ 3 处两两 IDENTICAL |
| A4 | `lower` | 2 | `ai/src/http_net.cpp:13` ↔ `ai/src/provider_spec.cpp:35` | 整串转小写（`std::tolower(unsigned char)`） | ✅ 2 处逐行一致 |
| A5 | `narrow` | 2 | `utils/src/diagnostics_report.cpp:24` ↔ `utils/src/webview2_runtime.cpp:40` | `std::wstring` → UTF-8（`WideCharToMultiByte`，空/失败返 `{}`） | ✅ 2 处逐行一致（16 行含空行） |

### B. 同名但语义/签名/实现不同（**排除** · §0.3「勿动语义」不强行并）

| 函数 | 份数 | 差异证据 |
|---|---|---|
| `join_names` | 6 | 分隔符不同（`、` vs ` / `）、空项处理不同（provider_spec/site_template 用 `、`，main 跳空项，pydoll_channel 用 ` / ` 且不跳空） |
| `join_list` | 3 | 分隔符不同（image_convert 用 ` / `，provider_spec/connection_test 用 `、`）；connection_test 版空返 `（无）` |
| `base64_encode` | 2 | 实现不同（deepseek_official_provider 用 OpenSSL `EVP_EncodeBlock`；web_pow 手写查表） |
| `local_time_text` | 3 | 签名不同（text_export 版带 `format` 参数）；buffer 16 vs 32；`strftime` vs `snprintf`；diagnostics 版带 `(时钟不可用)` 分支 |
| `to_utf8` | 3 | pipe_client 版带 `size<=0` 防御分支，另两处无 |
| `human_bytes` | 2 | deepseek_official_provider 版 KB/MB 自适应 + `ostringstream precision(1)`；image_convert 版固定 `%.2f MB` |
| `megabytes_text` | 2 | deepseek 版返回 MB 纯数字（`ostringstream`）；diagnostics 版返回 GB 带单位（`%.1f GB`）—— 同名不同义 |
| `read_reg_string` | 2 | webview2_runtime 版 `KEY_READ|KEY_WOW64_64KEY|32KEY` + buffer 512；diagnostics 版 `KEY_READ` + buffer 256 |
| `extract_cookies` | 3 | webview2_login 是声明+包装（调 `extract_cookies_impl`）；webview_host 是真实实现（不同逻辑） |
| `add_node`/`check`/`main`/`expect` 等 | — | 同名不同职责（工具入口 / 断言宏 / 各模块私有），非辅助函数重复 |

### C. 单处定义（**无重复** · 不入选）

`is_id_char`（site_template.cpp:29）、`max_of`（image_convert.cpp:143）、`truncate`（diagnostics_report.cpp:81）、`mask_value`、`extract_cookies_impl` 等。

## ② 引用取证（收敛点 · 全量）

| 函数 | 收敛到 | 调用点（改 `utils::xxx`） | 调用点计数 |
|---|---|---|---|
| `ce_find_ci` → `find_ci` | `utils::find_ci` | `ui/src/composite_editor.cpp:1421,1422` | 2 |
| `find_ci`（node_library） | `utils::find_ci` | `ui/src/node_library.cpp:73,139,161,162` | 4 |
| `trim` ×5 | `utils::trim` | `ai/src/deepseek/deepseek_web_client.cpp` ×2 · `ai/src/http_net.cpp` ×7 · `ai/src/provider_spec.cpp` ×16 · `ai/src/site_template.cpp` ×11 · `engine/src/graph.cpp` ×1 | 37 |
| `now_string` ×3 | `utils::now_string` | `engine/src/autosave.cpp:182` ×1 · `engine/src/recent_files.cpp:113` ×1 · `web/src/session_store.cpp:120,141,144` ×3 | 5 |
| `lower` ×2 | `utils::lower` | `ai/src/http_net.cpp` ×3 · `ai/src/provider_spec.cpp` ×1 | 4 |
| `narrow` ×2 | `utils::narrow` | `utils/src/diagnostics_report.cpp:57` · `utils/src/webview2_runtime.cpp:74,140` | 3 |

**合计**：14 处定义删除 · 55 处调用点改引 · 1 个新源文件 + 1 个新头 + CMakeLists 登记。

引用取证说明：
- 全部 55 处调用均为**同 TU 匿名命名空间内直接调用**，无跨 TU 符号引用（函数都藏在匿名命名空间，本就不可能被外部引用——这正是「重复只能靠复制、无法复用」的结构根因）。
- 无测试 / 文档 / 配置 / 诊断入口单独引用这些符号（它们只通过所在 TU 的公开函数被调用）。
- `check_links` 与文档引用不受影响（零文档改动）。

## ③ 影响面 / 回滚点

- **行为影响 = 零**：5 组收敛函数在语义上逐行等价（已精确 diff 验证）。共享实现从 14 处副本取一，行为唯一化后与每个副本的行为一致；仅函数名/命名空间变化（`ce_find_ci` → `utils::find_ci` 等），无参数/返回/空值/边界差异。
- **编译影响**：
  - `string_util.cpp` 加入 `aiwrite_core` 静态库（CMakeLists `utils/src/` 区）；`ui`（`aiwrite` 可执行）通过 `target_link_libraries(aiwrite PRIVATE aiwrite_core)` 已可见 ⇒ 无需改链接图。
  - 12 个改动文件均已在 include 区含 `log.h`/`paths.h` 等 utils 头，新增 `#include "string_util.h"` 无依赖环。
- **契约影响**：不涉及 `I*` 不变量 / JSON 规格 / CLI 契约 / 端口语义 / Pydoll 休眠栈（§6 硬边界不碰）。
- **回滚点**：收敛为**可逆小步**。单组回滚 = `git checkout -- <该组文件>` + 还原 CMakeLists 一行；整体回滚 = `git checkout -- source/utils/include/string_util.h source/utils/src/string_util.cpp source/CMakeLists.txt` + 12 文件。全部改动在本批独立 commit 前即逐组可还原。

## ④ 行动计划（交人工审核）

1. **新建单一来源**：
   - `source/utils/include/string_util.h`（`aiwrite::utils` 命名空间）：声明 `find_ci` / `trim` / `now_string` / `lower` / `narrow`，注释标注「单一来源 · 各模块共用，勿复制」。
   - `source/utils/src/string_util.cpp`：实现（取自副本逐行迁移，语义不动）。
   - `source/CMakeLists.txt`：`utils/src/string_util.cpp` 加入 `aiwrite_core` 源列表（`utils/src/paths.cpp` 行附近）。
2. **逐文件收敛**（12 文件：删本地定义 → 加 `#include "string_util.h"` → 调用点改 `utils::xxx`）：
   - `ui/src/composite_editor.cpp`（删 L90-114 `ce_find_ci`；改 L1421-1422）
   - `ui/src/node_library.cpp`（删 L22-45 `find_ci`；改 L73/139/161/162）
   - `ai/src/deepseek/deepseek_web_client.cpp`（删 trim；改 2 处）
   - `ai/src/http_net.cpp`（删 trim + lower；改 7+3 处）
   - `ai/src/provider_spec.cpp`（删 trim + lower；改 16+1 处）
   - `ai/src/site_template.cpp`（删 trim；改 11 处）
   - `engine/src/graph.cpp`（删 trim；改 1 处）
   - `engine/src/autosave.cpp`（删 now_string；改 1 处）
   - `engine/src/recent_files.cpp`（删 now_string；改 1 处）
   - `web/src/session_store.cpp`（删 now_string；改 3 处）
   - `utils/src/diagnostics_report.cpp`（删 narrow；改 1 处）
   - `utils/src/webview2_runtime.cpp`（删 narrow；改 2 处）
3. **收敛后核验**：
   - 每个改动文件 `git diff` 复查：**删除行**只含被收敛的函数定义与注释；**新增行**只含 include + `utils::` 前缀调用；无意外变更。
   - 断言：`find_ci|trim|now_string|lower|narrow` 的**裸定义**（`^\s*<类型> <名字>(`）在改动后仅 `string_util.cpp` 各 1 处。
4. **验证（CLN-05 判据 = 构建 0/0 · 相关 selftest 升）**：
   - 全量构建：`source/build.ps1`（默认 all 目标）**0 error / 0 warning**（MSVC `/W4`）。
   - selftest 全量回归：`--selftest` / `--exec-selftest` / `--graph-selftest` 全绿（§0.3 第 5 条「只升不降」，本次为纯语义等价收敛，应全量不降）。
   - `check_links`：broken 0（文档零改动，预期基线不变）。
5. **勾账**：`M_code_cleanup.md` §3.1 `CLN-05` 标记 ✅（5 组收敛 · 14 定义删 · 55 调用改 · 构建 0/0 验证通过）。

## ⑤ 审核闸门

- **待用户批准**：④ 的收敛计划（新建 2 文件 + 改 12 文件 + 删 14 处定义 + 改 55 处调用）。
- **审核通过前不删**（§3.0 硬性）。

## ⑥ 落地与登记

- **已执行（2026-10-20 · 人工批准，含「驼峰命名」指示）**：
  - 新建 `source/utils/include/string_util.h` + `source/utils/src/string_util.cpp`（`aiwrite::utils` 命名空间，单一来源；**命名按用户指示用驼峰**：`findCi` / `trim` / `nowString` / `toLower` / `narrow` —— 全库驼峰化先导样板）。
  - `source/CMakeLists.txt`：`utils/src/string_util.cpp` 登记入 `aiwrite_core`。
  - 收敛 12 文件：删 14 处本地定义 + 加 `#include "string_util.h"` + 55 处调用点改 `utils::xxx`（调用点分布见 §②）。
  - **验证**：构建 `build/logs/cln05_build.log` **0 error / 0 warning**（MSVC /W4）· `api_probe --selftest` 13 组 PASS（exit 0）· `--exec-selftest` **682/0**（exit 0）· `--graph-selftest` **151/0**（exit 0）· `check_links` **1610 / 0 broken**（exit 0）。
  - 勾账：`M_code_cleanup.md` §3.1 `CLN-05` ✅。
- **全库驼峰化（用户新指示 2026-10-20）**：存量 **1457 处 snake_case 函数定义 / 992 个去重名字**（扫描基线）。已登记为新一批（`M_code_cleanup.md` 暂记 `CLN-06` 扩项），本批 5 函数作先导样板。

---

## §5 方法学与已知局限

- **清点工具**：临时 PowerShell 脚本（不入库）——正则提取 `^\s*<返回类型> <名字>(` 定义 + 同名跨文件归组 + Trim-归一化 diff。**合成对照**：`join_names`（分隔符差异）与 `megabytes_text`（同名不同义）两例被正确判为「排除」⇒ 证明判定器能区分「真重复」与「语义分叉」。
- **局限**：正则按「顶格 / 匿名 ns 内」提取定义；`static` 文件级函数与头文件 `inline` 函数不在扫描面（本批聚焦辅助函数复制模式，头文件 inline 重复属 CLN-06 头一致性范畴，不越界）。
- **`find_ci` 注释原文**：「与节点库 find_ci 同语义」—— 是作者**主动复制的自认证据**，本批收敛即消灭此复制。
