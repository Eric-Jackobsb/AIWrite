# audit_cleanup_14：CLN-14 落地审计报告（main.cpp 拆分）

> 批次：`CLN-14` · 日期：2026-10-20 · 计划：docs/actionPlan/M_code_cleanup.md §3.4
> 性质：**超大文件拆分（可读性）** —— 不改变任何行为，仅分文件。
> 判据：不改变行为；单文件 < 60 KB；构建 0/0 + selftest 全绿。

## 1. 拆分对象与方案

| 项 | 值 |
|---|---|
| 源文件 | `source/app/src/main.cpp`（2359 行 / ≈122 KB） |
| 拆分方向 | **按 CLI 功能区拆**（2026-10-20 审计修正：主循环/窗口已承载于 `platform_glfw.cpp` / `platform_win32.cpp` / `webview_host.cpp`，不在 main.cpp；main.cpp 主体 = CLI / 自检分派） |
| 落地文件 | `cli_selftest.cpp`（自检域）· `cli_diag.cpp`（诊断/杂项域）· `cli_internal.h`（三 TU 内部共享声明） |

### 拆分后文件

| 文件 | 行数 | 体积 | 内容 |
|---|---|---|---|
| `source/app/src/main.cpp` | 661 | ≈33 KB | 入口 `main`（CLI 参数分派 + 启动/退出收尾），保留原 include 全量 + `cli_internal.h` |
| `source/app/src/cli_selftest.cpp` | 1201 | ≈57 KB | 自检域：`run_selftest`（原 700 行最大块）+ `official_generate_selftest` / `web_chat_selftest` / `web_session_selftest` / `vlm_selftest` / `provider_selftest` + `join_names`（定义）+ `utf8_length` / `utf8_prefix` / `kVa07ProbeChars` |
| `source/app/src/cli_diag.cpp` | 564 | ≈27 KB | 诊断/杂项域：`print_usage` / `pipe_selftest` / `web_session_list` / `print_spec_list` / `provider_dump` + `report_timestamp` / `read_text_file` / `write_text_file` |
| `source/ui/include/cli_internal.h` | 55 | ≈2 KB | 三 TU 内部共享声明：7 个通用辅助 + 10 个 CLI 分派函数（全局命名空间） |

## 2. 依赖分析与共享接口

- **命名空间结构**：原 main.cpp 整段 L54-1755 均在匿名命名空间（L53 `namespace {` … L1756 `} // namespace`），main 在全局。
- **跨 TU 提升**：拆分后匿名块函数提升为**全局命名空间**（`::`）—— `report_timestamp` / `read_text_file` / `write_text_file` / `utf8_length` / `utf8_prefix` / `join_names` / `print_usage` + 10 个 CLI 分派函数，全部经 `cli_internal.h` 声明共享（**不进公共头**）。
- **冲突排查（无）**：`join_names` 在 site_template.cpp / provider_spec.cpp（`aiwrite::ai`）、provider_resolve.cpp（`aiwrite::engine`）、pydoll_channel.cpp（匿名）各有定义 —— 与 main.cpp 的提升版（`::join_names`）**命名空间不同，不冲突**；`print_usage` 在 api_probe.cpp / webview2_login.cpp 有定义但属**独立 exe 目标**（api_probe.exe / webview2_login.exe），不与 aiwrite.exe 链接。
- **不跨 TU（随域内联）**：`kVa07ProbeChars` 常量随 utf8_* 放 cli_selftest.cpp。

## 3. 落地方式与等价性保证

- **纯物理搬移**：新文件内容由原 main.cpp 按行区间提取（PowerShell 数组切片，逐字节保留），未改动任何函数体 / 注释。
- **最小结构调整**：① 函数由匿名提升为全局命名空间（链接属性 internal → external，语义不变）；② 三个文件统一使用原 main.cpp 全量 include（L1-51）+ `cli_internal.h`；③ `join_names` 前向声明（原 L235）随 official_generate_selftest 区一并保留在 cli_selftest.cpp。
- **CMakeLists**：`aiwrite` 目标新增 `app/src/cli_selftest.cpp`、`app/src/cli_diag.cpp`。

## 4. 验证结果

| 验证 | 结果 |
|---|---|
| `build.ps1`（默认全目标） | ✅ **0 error / 0 warning**（aiwrite / api_probe / ce_repro / webview2_login 4 目标） |
| `api_probe --exec-selftest` | ✅ **682 通过 / 0 失败**（与基准一致） |
| `api_probe --graph-selftest` | ✅ **151 通过 / 0 失败**（与基准一致） |
| `api_probe --selftest` | ✅ **十三组全 PASS** |
| `aiwrite.exe --help` | ✅ 正常打印帮助文本（CLI 分派 + `print_usage` 跨 TU 调用路径完好） |
| 单文件体积 | main.cpp ≈122 KB → **33 KB**（661 行）；最大新文件 cli_selftest.cpp ≈57 KB（< 60 KB 目标） |

## 5. 回滚点

- 新文件均可删除 + `git checkout -- source/app/src/main.cpp source/CMakeLists.txt` 即回滚到拆分前单文件形态；函数名 / 语义零变化，无数据风险。
- ⚠️ 本次为纯拆分，**未改任何功能行为**；后续批（CLN-15 + 驼峰化）同法推进。

## 6. 备注

- `cli_selftest.cpp`（1201 行 / ≈57 KB）为**自检域全集**，接近 60 KB 上限；`run_selftest`（700 行）仍为最大单函数 —— 审计已标「低优先」，如打磨期需要可再拆（如 `cli_runspec_selftest.cpp`）。
- main.cpp 重建时首轮脚本路径笔误（`Join-Path $src 'main.cpp'` 把源文件当目录）→ 修正为直接写回后一次成功；行提取本身无错。
- 下一步：`CLN-15`（api_probe.cpp，先跑基线存档 → 分节拆独立文件 → 逐字节不变判据）→ 驼峰化（`CLN-06` 扩项）。
