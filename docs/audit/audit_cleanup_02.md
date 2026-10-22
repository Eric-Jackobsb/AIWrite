# 审计报告 · audit_cleanup_02 —— CLN-02 冗余 / 未用 include

> 批次：`CLN-02`（对应 [../actionPlan/M_code_cleanup.md](../actionPlan/M_code_cleanup.md) §3.2）· 日期：2026-10-20
> 性质：**删除前置审计闸门（§3.0）**的产物 —— 本批扫描结论 = **项目头未用 include 0 项**；唯一可删项为 `tools/api_probe.cpp` 的 **2 行重复 include**（零行为风险）。
> 范围：`source/**`（**225 个 .cpp/.h**，全量），排除 `third_party/**`（vendored）与 `source/python/**`（Pydoll 休眠栈，硬边界 §6）。
> 方法：自研静态扫描脚本（临时工具，不入库，见 §5）+ 合成**阳性对照**验证扫描器可信 + 关键文件人工核验。

---

## ① 对象清点（删除候选）

| 分类 | 扫描方式 | 候选数 | 结论 |
|---|---|---|---|
| A. 未用项目头 include | 文件体标识符 ∩ 头文件「自声明符号集」（符号级） | **0** | 无删除项 |
| B. 同文件内重复 include | 文件内 `#include` 同名计数 > 1 | **2**（均属 `tools/api_probe.cpp`） | 可删（去重，保留首处） |
| C. 外部头（std / 系统 / vendored） | 无静态符号表可取证 | 687 处 | 编译驱动验证；本批无可疑对象，不判删 |

## ② 引用取证（逐项）

- **A 类**：扫描器对全部项目头做符号级判定——文件剥注释/字符串/本 include 行后取标识符集 `F`；头文件自声明符号集 `E` = 头文件标识符 − C++ 关键字/近通用 std 名 − **其直接包含的项目头标识符**（避免把传递符号算在它头上）+ `#define` 宏名。`F ∩ E = ∅` 才判未用。**全库 0 命中**。
  - **可信度证据**：合成阳性对照（§5）证明扫描器**能**抓出未用 include；抽查本次重构波及文件（`node_canvas.cpp`（-657 行）、`composite_editor.cpp`（+579 行）、`canvas_view.cpp`、`app.cpp`），每个 include 均带用途注释且与现存代码相符。
- **B 类 `api_probe.cpp`**（重复 include，逐行确认）：
  - **L24 与 L39 均含 `#include "paths.h"`**（中间隔着 graph/autosave/node_registry 等 14 个头）——真重复。
  - **L46 与 L52 均含 `#include "webview_host.h"`**（L47-51 之间有 cancel_token/retry_policy 等）——真重复。
  - `#pragma once` 下重复 include 无符号/副作用，但属冗余；**删除第二处、保留第一处** 零风险。
- **C 类**：外部头（`<string>`、`<imgui.h>`、`<nlohmann/json.hpp>`、`<windows.h>` 等 687 处）无法静态取证（无符号表）；本批**未发现可疑对象**，不做盲删（符合 §3.0「不靠碰运气」）。

## ③ 影响面 / 回滚点

- 待删项 = `api_probe.cpp` 2 行重复 include：**零行为影响**（`#pragma once` 幂等，去重不改变符号可见性）。
- 回滚点：单文件 `git checkout source/tools/api_probe.cpp` 即可。

## ④ 行动计划（交人工审核）

1. 执行删除（待批准）：`api_probe.cpp` 删除 **L39 `#include "paths.h"`** 与 **L52 `#include "webview_host.h"`**（保留 L24 / L46 首处）。
2. 验证（CLN-02 判据 = 「删除后必须能干净重编译，靠构建日志」）：
   - 全量构建 **0 error / 0 warning**（MSVC `/W4`）；
   - `check_links`（文档链路基线 1597 / broken 0）；
   - 兜底：`--selftest` / `--exec-selftest` / `--graph-selftest` 全绿（§0.3 第 2 条，本次有代码改动 ⇒ 建议执行，可与选项 D 合并）。
3. 勾账：`M_code_cleanup.md` §3.2 `CLN-02` 标记 ✅（删除 2 行 · 构建 0/0 验证通过）。

## ⑤ 审核闸门

- **待用户批准**：执行 ④-1 的 2 行删除。
- 附加确认：是否一并执行 ④-2 的 selftest 全量回归（= 上一轮候选方向 D，可与本批合并执行）。

## ⑥ 落地与登记

- **已执行（2026-10-20 · 人工已批准）**：`tools/api_probe.cpp` 删除第 2 处 `#include "paths.h"`（原 L39）与第 2 处 `#include "webview_host.h"`（原 L52）——保留 L24 / L45 首处。
- **验证（CLN-02 判据 = 靠构建日志）**：全量构建 **0 error / 0 warning**（`source/build.ps1`，2026-10-20）· `check_links` **1598 / broken 0** · **selftest 全量回归**（`--selftest` 13 组 PASS · `--exec-selftest` 682/0 · `--graph-selftest` 151/0）。
- **勾账**：`M_code_cleanup.md` §3.1 `CLN-02` 已标记 ✅（v1.4）。
- 证据存档：构建输出 `build/tmp/cln02_build_out.txt` · selftest 输出 `build/tmp/cln02_st_*.txt` · 扫描器与夹具 `build/tmp/cln02_scan.py` / `cln02_selftest.py` / `tst/`（临时，不入库）。

---

## §5 方法学与已知局限（如实登记）

- **扫描脚本**（临时，不入库）：`build/tmp/cln02_scan.py`（符号级判定）+ `build/tmp/cln02_selftest.py`（阳性对照驱动）。`build/tmp/tst/` 为合成夹具（h1.h / h2.h / use1.cpp / use2.cpp）。
- **扫描器 bug 修复 3 个**（均为审计过程中发现并修复，否则 0 候选结果不可信）：
  1. **误用 Python 关键字表（`keyword.kwlist`）当 C++ 关键字集** → `int`/`string`/`return` 等混入头文件导出集，几乎任何文件都命中 ⇒ 全库假阴性 0 候选。已换用 CLN-01 验证过的 C++ 关键字 + 公共 std 名集合。
  2. **头文件自带 `#include`/`#pragma` 行标识符污染导出集**（`include`/`string`/`pragma`/`once`）。已剥离全部预处理行（`#define` 宏名另行单独保留）。
  3. **重写剥注释函数时数字分隔符判定移位**（`i += 1` 后再判 `prev_ok`）→ `30'000` 吞块回归（与 CLN-01 同款 bug 的二次引入）。已整体替换为 CLN-01 验证过的状态机版。
- **阳性对照（可信度证据）**：`use1.cpp` 包含 h1/h2 但只用 h2 符号 → 扫描器正确判 **h1 未用**；`use2.cpp` 只用 h1 符号 → 正确判 **已用**。
- **残留局限**：
  - 外部头（std / 系统 / vendored）无静态符号表 ⇒ 只能靠编译验证（本批无候选，不盲删）。
  - 「方便头」场景（文件只包含 A 头但用 B 头符号）不会判 A 冗余——属**正确保守**（删 A 会编译失败）。
  - 宏拼接 / 运算符覆写 / 友元等极端引用场景依赖编译兜底（§3.0 第 6 条「不靠碰运气」的最终闸门是构建 0/0）。

**文档结束。**
