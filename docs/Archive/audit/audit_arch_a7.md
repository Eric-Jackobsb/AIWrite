# audit_arch_a7：ARCH-A7 落地审计报告（web/utils 27 条文件级可变状态对象化 · 删基线切严格零容忍）

> 日期：2026-10-23 · 批次：**阶段 3 第七批（ARCH-A7）** · 承接 [audit_arch_a6.md](audit_arch_a6.md)（ui 层清零 · 基线 27 条）
> 结论一句话：**文件级可变状态 87 → 0**（A6 清 ui 60 条 · 本批清 web 15 + utils 12），
> `check_globals` 改为**严格零容忍**（基线文件删除）+ 新增**函数内 static 棘轮 51**，
> 判据 2 从 🟡 **转 ✅**；构建 0/0、`check_all` **21/21** PASS、自检断言**零增减**。

---

## 1. 背景与目标（承接 §1.1 规则 6 / §4 判据 2）

A5 把规则 6 变成机器判据时留了口子：**存量登记在棘轮基线里**（113 → 87 → A6 后 27）。
A6 之后剩余的 27 条全部集中在两个「能力层」（web 15 / utils 12）：

* 它们**没有**像 `EditorController` 那样的所有者对象可以挂载（utils 是最底层，web 被 ui/app 单向依赖）；
* 但它们共同点是**进程级单实例设施**：配置缓存、日志器、凭据可用性缓存、异常计数、IPC 计数、
  守护会话句柄、窗口表 / 环境缓存。

本批目标 = 这 27 条**全部**从「文件作用域可变变量」变成「显式状态对象 + 单一定义点」，
然后把工具切成**严格 0**（删掉基线文件），并补上**函数内 static 棘轮**，
防止「文件级归零、函数内继续长」把刚关上的门重新打开。

## 2. 归属决策（为什么这里用「访问器单实例」而不是注入）

| 候选 | 结论 | 理由 |
|---|---|---|
| 注入（像 A6 那样由上层持有） | ❌ 不可行 | `utils` 不依赖任何层，`log::info()` / `appConfig()` 是**全库随处调用**的自由函数；改造 = 给每个调用点加参数，行为零收益、diff 巨大 |
| 保留文件级 `static` | ❌ 否 | 正是本批要清的对象（初始化顺序不可控、TU 内隐式内部链接、A7 判据不达成） |
| **`struct XState` + `X& xState()` 单一定义点** | ✅ 采用 | 状态**成组**且有名字；初始化改成 C++11 magic static（**首次使用才构造，线程安全**）；读写口只有一个，`rg xState` 即可盘点全部触碰点 |

> 与 A6 §2「拒绝函数内 static 单例」的口径差异：那一批**有**所有者对象（`EditorController`），
> 用函数内 static 只是「换个地方藏单例」；本批的应用场景是**进程级基础设施**，
> 它们按定义就该是单实例 —— 能改的只是「怎么表达这个单实例」。所以本批**同时**补了棘轮上限：
> 表达方式可以商量，「数量继续增长」不行。

## 3. 逐文件改动（27 条 / 7 文件）

| 文件 | 原状 | 落地形态（单一定义点） |
|---|---|---|
| `utils/src/config.cpp` | 2 条（`g_config_mutex` / `g_config`） | `struct ConfigStore { mutex; value; }` + `configStore()` |
| `utils/src/log.cpp` | 2 条（`g_logger` / `g_workflow_logger`） | `struct Loggers { app; workflow; }` + `loggers()` |
| `utils/src/credential.cpp` | 1 条（`g_availability`） | `struct AvailabilityCache { entries; }` + `availabilityCache()` |
| `utils/src/diagnostics.cpp` | 7 条（异常计数 6 × `atomic` + `installed`） | `struct ExceptionCounter` + `exceptionCounter()`（内存序仍全 `relaxed`） |
| `web/src/pipe_client.cpp` | 4 条（全局 2 × `atomic` + 2 × `thread_local`） | `IpcCounters` + `ipcCounters()` · `IpcThreadCounters` + `ipcThreadCounters()`（**`thread_local` 语义原样保留**） |
| `web/src/pydoll_channel.cpp` | 3 条（`g_session` / `g_session_pipe` / `g_cancel_polls`） | `struct SessionState { process; pipe_name; cancel_polls; }` + `sessionState()` |
| `web/src/webview_host.cpp` | 8 条（窗口表 3 + 环境缓存 4 + `t_cur`） | `WindowRegistry` + `windowRegistry()` · `EnvCache` + `envCache()` · `WindowCtx*& tlsWindow()`（`st()` 改为 `*tlsWindow()`） |

合计 **27 条 → 0**；命名一律「字段名去前缀」（`g_env_creating` → `envCache().creating`），
调用点按原语义就地替换（`webview_host.cpp` 58 行 / `diagnostics.cpp` 14 行 / `log.cpp` 15 行 / `pipe_client.cpp` 14 行 /
`pydoll_channel.cpp` 13 行 / `config.cpp` 4 行 / `credential.cpp` 3 行）。

## 4. 工具改造（严格零容忍 + 函数内 static 棘轮 + 删基线）

1. **删基线**：`source/tools/check_globals_baseline.txt` 随存量归零一并删除；
   文档里 6 处指向它的链接改为文字标注（`check_links` 1910 / **0**）。
2. **`check_all.ps1` 第 ⑦ 项**：调用从 `-Baseline <file>` 改成**裸调用**（严格模式），
   项名 `check_globals（全局变量棘轮）` → `check_globals（全局变量零容忍）`；项数仍 **21**（只是判据变严）。
3. **`check_globals.ps1`**：
   * 新增 `-MaxFunctionStatics <n>` + `$script:FnStaticCap = 51`：函数内 static **只许减**，超上限 ⇒ exit 1
     （原来只打印「参考」行，等于**没门**）；
   * 汇总行加 `fn_static: 51 / 51`，`check_all` 详情里直接可见；
   * `-Baseline` 保留为**遗留能力**（将来大批量迁移可再用），文档标注「现用形态 = 严格 0」。

> 为什么上限是 **51 而不是 41**：本批新增的 10 个访问器本身各含 1 个函数内 `static`（配置 / 日志 / 凭据 / 异常计数 /
> IPC 全局 / IPC 线程 / 会话 / 窗口表 / 环境缓存 / 线程当前窗口），
> 41 + 10 = 51。这是**显式的一次性交换**：文件级 27 条消失，函数内 +10 条被棘轮钉死。

## 5. 验证结果（本批实测）

```
构建                     exit=0 · error=0 · warning=0
check_globals            201 文件 · violations 0（R1 0 / R2 0 / R3 0）· fn_static 51 / 51  ← 27 → 0
check_includes           201 文件 / 666 行 / violations 0
check_links              1910 links / broken 0
check_all                共 21 项 · 失败 0 项
自检                     exec 713/0 · graph 151/0 · --run-selftest PASS · ce_repro 全帧 OK
                         settings-assert 越界 15 例全拦 · site 50/50 · official 40/40 · autosave 32/32 · upload 72/0
```

**反向验证（三条，用完即复原）**：

| 注入 | 期望 | 实测 |
|---|---|---|
| 在 `config.cpp` 命名空间作用域加 `static int g_probe_scope = 0;` | R2 违规 ⇒ exit 1 | `violations: 1 (R2 1)` · `结果: FAIL` · exit **1** ✅ |
| `-MaxFunctionStatics 50`（低于实际 51） | 棘轮超限 ⇒ exit 1 | `fn_static: 51 / 50` · `结果: FAIL（函数内 static 超出棘轮上限 50…）` · exit **1** ✅ |
| 复原后 | PASS · exit 0 | `violations: 0 · fn_static 51 / 51` · `结果: PASS` · exit **0** ✅ |

## 6. 验收判据对照（§4 判据 2）

| 判据 2 的分项 | 本批前 | 本批后 |
|---|---|---|
| 跨 TU `extern` 变量 | 0（A5） | **0** |
| 文件级 `static` / 匿名 `namespace` 可变状态 | 27（web 15 / utils 12） | **0** |
| 外部链接全局（R3） | 0 | **0** |
| 门禁形态 | 棘轮基线（新增即失败，存量放行） | **严格零容忍**（无基线文件） |
| 函数内 `static` | 41 处，**只统计不判失败** | **51 处，棘轮上限（超 1 条即失败）** |

⇒ **判据 2「全局变量归零」= ✅ 已满足**（`check_globals.ps1` 是该判据的唯一执行者）。

## 7. 已知限制

1. **常量表仍在文件作用域**（`constexpr` / `const`）—— 规则 6 明确不计入（只读），本批不动。
2. **单实例仍是进程级**：`configStore()` / `loggers()` / `windowRegistry()` 等无法在同一进程内做「多份」。
   若将来要「多工作区 / 多配置域」，入口已收敛到 10 个访问器，替换成注入式即可（不必再全库搜变量）。
3. **棘轮数字是硬编码常量**（`$script:FnStaticCap = 51`），减少后需手动下调；
   A5 的基线文件形态更适合大批量迁移，本批不再使用（脚本保留能力）。
4. **退出期行为**：magic static 的析构发生在 `main` 返回后（与原全局对象同阶段），
   `log::shutdown()` 仍显式 `reset` 工作流 logger / `spdlog::shutdown()`，语义未变。

## 8. 回滚点

* 回滚到本批之前：`git revert <A7 提交>`（或 `git reset --hard b8f290f` = A6 提交，恢复 27 条存量 + 基线文件 + 棘轮模式）。
* 只回滚工具：`git checkout b8f290f -- source/tools/check_globals.ps1 source/tools/check_all.ps1 source/tools/check_globals_baseline.txt`
  —— **注意**：此时代码侧已是 0 条，基线模式会显示 `fixable: 27`（不是失败，但需要再 `-UpdateBaseline` 一次才干净）。
* 本审计文档可留作登记。
