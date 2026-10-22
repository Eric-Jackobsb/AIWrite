# audit_cleanup_15：CLN-15 落地审计报告（api_probe.cpp 拆分）

> 批次：`CLN-15` · 日期：2026-10-20 · 计划：docs/actionPlan/M_code_cleanup.md §3.4
> 性质：**超大文件拆分（可读性）** —— 只拆结构不动断言，不改变任何行为。
> 判据：不改变行为/输出；**先跑基线存档再动**（拆前 `--selftest` 13 组 / `--exec-selftest` 682 / `--graph-selftest` 151 存档 `build/_cln15_baseline/`）；构建 0/0。

## 1. 拆分对象与方案

| 项 | 值 |
|---|---|
| 源文件 | `source/tools/api_probe.cpp`（6937 行 / ≈390 KB） |
| 拆分方向 | **按自检分节拆独立文件**（2026-10-20 审计确认：分节边界清晰；`execution_selftest` ≈3500 行单函数为最大拆分对象） |
| 落地文件 | 5 新 cpp + `api_internal.h`（跨 TU 内部共享声明） |

### 拆分后文件

| 文件 | 行数 | 体积 | 内容 |
|---|---|---|---|
| `source/tools/api_probe.cpp` | 321 | ≈14 KB | 入口 `main`（CLI 参数分派 + 启动/退出收尾），保留原 include 全量 + `api_internal.h` |
| `source/tools/api_probe_selftest.cpp` | 1866 | ≈101 KB | 协议/网络/CLI 单测域（原匿名块 A）：`Options`（移入头）· `kSha3*` · `has_header_named` / `chat_completion_body` / `loopback_request` · `test_nokey_request_shape` / `test_cancel_and_timeout` / `test_protocol_requests` / `test_factory_and_capability` / `test_network_and_credentials` / `test_protocol_loopback` / `print_usage` / `test_sha3` / `test_chat_request_shape` / `test_config_roundtrip` / `test_web_session_and_visibility` / `set_param`（定义）/ `test_workflow_files` / `test_http` / `test_chat` |
| `source/tools/api_graph_selftest.cpp` | 736 | ≈37 KB | 图模型自检段：`Check`（移入头）· `expect` / `expect_eq` / `add_node` · `graph_selftest` |
| `source/tools/api_executor_selftest.cpp` | 393 | ≈20 KB | 执行器自检段（P3-1）：`make_edge` / `contains_text` · `executor_selftest` |
| `source/tools/api_execution_selftest.cpp` | 3624 | ≈220 KB | 执行核心自检段（P3-2）：`execution_selftest`（≈3500 行单函数） |
| `source/tools/api_diag.cpp` | 482 | ≈24 KB | 诊断/断言域：`image_decode_report` · `test_diag_report` / `test_dep_check` / `test_settings_assert` / `test_site_assert` / `test_autosave_assert` |
| `source/tools/api_internal.h` | — | ≈4 KB | 跨 TU 内部共享声明：`Options` / `Check` + 断言辅助（`expect` / `expect_eq` / `add_node` / `contains_text` / `make_edge` / `set_param`）+ 13 个 `test_*` / `print_usage` / 3 个自检入口 / 6 个诊断函数 |

## 2. 依赖分析与共享接口

- **命名空间结构**：原 api_probe.cpp 含两个匿名块 —— 块 A（L102-1887：`Options` + `kSha3` + 嵌套块 B（L137-171：`has_header_named` / `chat_completion_body` / `loopback_request`）+ 协议/CLI 单测）+ 块 C（L1889-6378：`Check` / `expect*` / `add_node` / `make_edge` / `contains_text` / `graph_selftest` / `executor_selftest` / `execution_selftest` / `image_decode_report`）+ 全局段（L6380-6937：6 个诊断函数 + `main`）。
- **跨 TU 提升**：拆分后匿名块符号提升为**全局命名空间**（`::`）—— `Options`（结构体定义移入头）/ `Check` / `expect` / `expect_eq` / `add_node` / `contains_text` / `make_edge` / `set_param` / `print_usage` / 13 个 `test_*` / `graph_selftest` / `executor_selftest` / `execution_selftest` / `image_decode_report` / 6 个诊断函数，全部经 `api_internal.h` 声明共享（**不进公共头**）。
- **冲突排查（无）**：api_probe 为独立 exe 目标，不与 aiwrite.exe / webview2_login.exe 链接；提升符号（如 `print_usage` / `set_param` / `expect`）在 api_probe.exe 内唯一。`namespace engine = aiwrite::engine;` 别名随原头 L1-100 保留在各 TU（非头内，避免别名重定义）。
- **不跨 TU（随域内联）**：`kSha3Abc` / `kSha3Empty` 常量随 `test_sha3` 留在 api_probe_selftest.cpp。

## 3. 落地方式与等价性保证

- **纯物理搬移**：新文件内容由原 api_probe.cpp 按行区间提取（PowerShell 数组切片，逐字节保留），未改动任何函数体 / 断言 / 注释文本。
- **最小结构调整**：① 匿名块函数提升为全局命名空间（链接属性 internal → external，语义不变）；② 各 TU 统一使用原头 L1-100（含 `namespace engine` 别名）+ `api_internal.h`；③ `Options` / `Check` 结构体定义移入 `api_internal.h`（跨 TU 单一定义）；④ `expect` 默认参数从定义移到头声明（`detail = {}`）；⑤ `set_param` 第 4 参 `nlohmann::json`（可同时接受字符串与 bool 实参）。
- **CMakeLists**：`api_probe` 目标新增 `api_probe_selftest.cpp` / `api_graph_selftest.cpp` / `api_executor_selftest.cpp` / `api_execution_selftest.cpp` / `api_diag.cpp`（`composite_selftest.cpp` 保留）。

## 4. 验证结果

| 验证 | 结果 |
|---|---|
| `build.ps1`（默认全目标） | ✅ **0 error / 0 warning**（aiwrite / api_probe / ce_repro / webview2_login 4 目标） |
| `api_probe --exec-selftest` | ✅ **682 通过 / 0 失败**（与拆前基线一致 · 输出 874 行，行数逐行相等） |
| `api_probe --graph-selftest` | ✅ **151 通过 / 0 失败**（与拆前基线一致 · 输出 153 行，行数逐行相等） |
| `api_probe --selftest` | ✅ **十三组全 PASS**（与拆前基线一致 · 输出 924 行，行数逐行相等） |
| 输出逐字节对比 | ✅ 基线（`build/_cln15_baseline/` 拆前存档）vs 拆后 —— `Compare-Object` 差异行**全部为日志时间戳**（`19:46:49` vs `20:03:57` 等运行时刻漂移，`2>&1` 合并流中的 `[INFO]`/`[WARN]` 日志），断言文本 / PASS / FAIL 逐字一致 |
| `check_links.py` | ✅ 1679 / 0 |
| `aiwrite.exe --help` | ✅ 正常打印帮助文本（CLI 目标未受 api_probe 拆分影响） |
| 单文件体积 | api_probe.cpp ≈390 KB → **14 KB**（321 行）；最大新文件 api_execution_selftest.cpp ≈220 KB（`execution_selftest` 单函数，审计已标「需再细分」低优先） |

## 5. 回滚点

- 删除 5 个新 cpp + `api_internal.h`，`git checkout -- source/tools/api_probe.cpp source/CMakeLists.txt` 即回滚到拆分前单文件形态（git index 保留拆分前 6937 行版本）；函数名 / 语义零变化，无数据风险。
- ⚠️ 本次为纯拆分，**未改任何功能行为**；后续批（驼峰化 = CLN-06 扩项）同法推进。

## 6. 备注

- `api_execution_selftest.cpp`（3624 行 / ≈220 KB）仍为**最大单文件**（`execution_selftest` ≈3500 行单函数）—— 审计 §⑤-c 已标「需再细分」，因是单函数拆分收益低、风险高，留待打磨期（M_polish）处理。
- **脚本/头声明踩坑记录（均属工具层，函数体零改动）**：① PowerShell 数组参数逗号前缀（`,$body`）导致 $path 被数组化 → 产生垃圾文件名，修正为直接传 List；② 脚本重跑读到已被覆盖的源文件 → 增加只读备份（`build/_api_probe_orig.cpp`）来源，重跑不受影响；③ `namespace engine` 别名在 L100（非 L98）→ 各 TU 头取 L1-100；④ `set_param` 第 4 参原始签名是 `nlohmann::json`（非 `std::string`）→ 头声明修正（`set_param(graph, m, "dedup", true)` 依赖 json 隐式转换）。最终构建 0/0 + 输出零差异。
- 下一步：**驼峰化（`CLN-06` 扩项 · §3.4 全部完成后）** —— M_code_cleanup.md 已登记。
