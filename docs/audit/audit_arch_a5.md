# audit_arch_a5：ARCH-A5 落地审计报告（全局变量断言 · `check_globals.ps1` + 棘轮基线）

> 批次：`ARCH-A5` · 日期：2026-10-23 · 计划：[refactor/ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §2.1 迁移路线阶段 3「应用级收口（ARCH-A）」A5
> 性质：**架构约束工具化（第二批）** —— 把 §1.1 规则 6「跨帧可变状态显式化」从**口头纪律**变成**离线可验证断言**：新增 [../../source/tools/check_globals.ps1](../../source/tools/check_globals.ps1) + 存量基线 [../../source/tools/check_globals_baseline.txt](../../source/tools/check_globals_baseline.txt)，接入 [../../source/tools/check_all.ps1](../../source/tools/check_all.ps1) 作为第 ⑦ 项（`check_all` **20 → 21 项**）。
> 判据：`check_all` **21/21 PASS**（新项 `check_globals（全局变量棘轮）` PASS）· 扫描 **201 文件 / 违规 87（R1 0 / R2 82 / R3 5）/ 基线 87 / 新增 0** · 本批同时**清零「跨 TU extern」（113 → 87，`R1 = 0`）** · 构建 0/0 · `--run-selftest` PASS · exec 713/0 · graph 151/0 · ce_repro 全帧 OK · check_links **1889/0**。

## 1. 背景与目标（承接 §1.1 规则 6 / §4 判据 2）

规则 6 原文：**「任何跨帧存活的可变状态必须是某个对象成员；文件级 `static` 与跨 TU `extern` 全局在重构结束后归零」**。A4 只把规则 1（依赖单向）工具化了，规则 6 仍是一句话 —— 而它恰恰是本次重构**性质最恶劣**的一类债：全局变量**编译从不报错**、`--run-selftest` 也常常照过，但它是「一次操作触发多次同步」「关闭会话后状态残留」「两画布行为漂移」的根因土壤。

A5 的目标 = **让新增的全局状态立刻变红**：

1. 静态扫描命名空间作用域的可变变量（跨 TU `extern` / 文件级 `static` / 匿名 namespace / 外部链接全局），**只读、零副作用、可离线跑**。
2. 违规 = 失败（退出码 1），并逐条打印 `文件:行  R?  声明`。
3. 接入一键自检；同时**先把最危险的「跨 TU extern」清零**（本批代码改动见 §4）。

> 与 A4 的差别：A4 是纯新增脚本（存量本来就 0）；A5 存量 **113 条**，一次归零不现实 ⇒ 引入**棘轮基线**（存量登记、新增即失败、只减不增），把「归零」拆成 A5（工具 + R1 清零）→ A6（ui 逐文件对象化）→ A7（web/utils + 收尾删基线）。

## 2. 规则定义（脚本 = 唯一执行者）

| 规则 | 内容 | 依据 |
|---|---|---|
| **R1** | 任何层 `*.h` 在命名空间作用域出现 `extern <类型> <名>;`（函数 / `extern "C"` 除外） | 规则 6「跨 TU `extern` 归零」 |
| **R2** | `*.cpp`/`*.h` 在命名空间作用域（含匿名 `namespace { … }` 内）定义的**可变**变量 | 匿名空间成员 = 内部链接，等价「文件级 `static`」 |
| **R3** | 命名空间作用域、非 `static`、非匿名空间、非 const 的可变定义（真正的外部链接全局） | 跨 TU 可见，最危险 |

**不计入**（规则 6 的合法形态，**无例外条款**）：类 / 结构体 / 联合体 / 枚举**成员**（`TextureCache`、`CeUiState` 这类对象成员天然合规）· `const` / `constexpr` / `consteval` / `constinit` 常量表 · 函数 / 类型 / 别名 / 模板 / `using` / `typedef` / 预处理行 / 前向声明。

**函数内 `static` 局部变量**：属于**函数作用域**（不是「文件级 / 跨 TU」），因此**只统计不判失败** —— 输出末尾固定一行 `参考：函数内 static 局部变量 N 处`（当前 **43**），供后续批次决定是否一并对象化。


**判定算法**（关键设计点）：C++ 里「一行是不是命名空间作用域的可变变量定义」无法用正则单独判死（函数体 / 初始化列表 / 类型体里的同名文本形态完全一样），脚本因此做了两步：

1. **词法预处理**（保持行号不变）：逐字符去掉 `//` 与 `/* */` 注释、普通字符串 / 字符字面量、**原始字符串 `R"delim( … )delim"`**；C++14 数字分隔符（`2'000`）**不当字符字面量**。
   - 这三个坑都是实测踩出来的：`2'000` 被当成 `'` 起始字符字面量 ⇒ 后续花括号配对全乱（`pydoll_channel.cpp` 因此把函数体内的局部变量误报成全局）；raw string 未识别 ⇒ JS/JSON 脚本正文里的 `;` 行被当成声明；多行字符串续行（`"...";`）被当成 `"";` 声明。
2. **作用域栈**：`ns`（命名空间）/ `anon`（匿名命名空间）/ `other`（函数体 / 类型体 / 初始化列表，内容跳过）三类；只在栈顶 ∈ {`ns`,`anon`} 时判定声明，逐字符结算 `{` `}`（同行多块也能正确配对）。
   - 判声明的附加约束：行尾 `;`、不含 `(` `)`（排除函数声明与函数指针）、不以类型 / 别名 / 预处理关键字开头。
   - **R2 vs R3** 由「是否 `static` / 祖先里是否有匿名空间」区分。

**已知限制**：带函数调用初始化的全局（`Foo g = make();`）**不在扫描范围**（含 `)`，与函数声明无法区分；此类写法本项目为 0）；跨行声明只认首行；`#if` 两侧**都**扫（比编译器更严格 —— 宁可误报不放过，本批实测 0 误报）。

## 3. 现状清单（重构前 113 条 · 本批后 87 条）

重构前实测（2026-10-23 · `check_globals.ps1` 首跑）：

| 层 | 文件 | R1 跨 TU extern | R2 文件级 static（含匿名空间） | R3 外部链接全局 | 合计 |
|---|---|---|---|---|---|
| app | 3 | 0 | 0 | 0 | 0 |
| **ui** | 68 | **12** | **56** | **18** | **86** |
| engine | 24 | 0 | 0 | 0 | 0 |
| nodes | 10 | 0 | 0 | 0 | 0 |
| ai | 44 | 0 | 0 | 0 | 0 |
| web | 19 | 0 | 15 | 0 | 15 |
| utils | 33 | 0 | 12 | 0 | 12 |
| **合计** | 201 | **12** | **83** | **18** | **113** |

本批后（`R1 = 0`）：

| 层 | 文件 | R1 | R2 | R3 | 合计 |
|---|---|---|---|---|---|
| ui | 68 | **0** | 55 | **5** | 60 |
| web | 19 | 0 | 15 | 0 | 15 |
| utils | 33 | 0 | 12 | 0 | 12 |
| 其余 4 层 | 81 | 0 | 0 | 0 | **0** |
| **合计** | 201 | **0** | **82** | **5** | **87** |

**存量分布（= A6/A7 待办清单的唯一权威口径，逐条内容见 [../../source/tools/check_globals_baseline.txt](../../source/tools/check_globals_baseline.txt)）**：

| 文件 | 条数 | 规则 | 典型内容 |
|---|---|---|---|
| `ui/src/settings_panel.cpp` | 19 | R2 | 设置面板草稿缓冲 / 校验结果 / 文件对话框状态 |
| `ui/src/texture_cache.cpp` | 12 | R2 | `g_cache` / `g_order` / `g_capacity` / `g_bytes` / `g_frame` / 七项统计 |
| `ui/src/ce_draw.cpp` | 7 | R2 | 新建 / 打包对话框状态（`g_new_*` / `g_pack_*`） |
| `ui/src/node_canvas.cpp` | 6 + 5 | R2 / R3 | 主画布吸附 / 小地图 / 诊断采样 |
| `ui/src/node_library.cpp` | 6 | R2 | 搜索缓冲 / 分类折叠 |
| `ui/src/console_panel.cpp` | 3 | R2 | 过滤级别 / 搜索缓冲 |
| `ui/src/output_panel.cpp` | 2 | R2 | 折叠 / 滚动锚点 |
| `web/src/webview_host.cpp` | 8 | R2 | 窗口表 / 环境创建锁 / 条件变量 |
| `web/src/pipe_client.cpp` | 4 | R2 | IPC 计数（atomic / thread_local） |
| `web/src/pydoll_channel.cpp` | 3 | R2 | 守护进程会话 |
| `utils/src/diagnostics.cpp` | 7 | R2 | 异常计数 atomics |
| `utils/src/config.cpp` · `credential.cpp` · `log.cpp` | 2 + 1 + 2 | R2 | 配置缓存 / 可用性表 / logger 单例 |

## 4. 本批代码改动：直接把「跨 TU extern」清零（113 → 87）

`ui/include/ce_internal.h` 原有 **12 条 `extern` 全局**（上级节点编辑器的画布上下文 / 改名缓冲 / 5 个开关 / 小地图快照 / 撤销栈），定义分散在 `composite_editor.cpp`（9 条）与 `ce_state.cpp`（2 条 undo/redo），被 `ce_draw.cpp` / `ce_session.cpp` / `composite_editor.cpp` 共 **60 余处**跨 TU 访问。

改法 = **收进一个状态对象**（规则 6 的「必须是某个对象成员」）：

```cpp
// ce_internal.h（唯一状态容器；定义于 ce_state.cpp）
struct CeUiState {
    ax::NodeEditor::EditorContext* context = nullptr;   // 画布域
    std::string rename_id;  std::array<char,128> rename_buf{};  char add_filter[64] = {};
    bool show_minimap = true, align_snap = true, grid_snap = true, show_grid = true, request_fit = false;
    MinimapState minimap;  std::unordered_map<std::string, ImVec2> prev_positions;
    bool window_focused = false;
    std::vector<CeUndoEntry> undo, redo;                // 撤销域
};
CeUiState& ceUiState();                                  // ce_state.cpp: static CeUiState state;
```

顺带把同域的两条 TU 内全局一并归入（同类债务，改动同处、风险同批承担）：

| 原符号 | 原位置 | 新归属 | 消除 |
|---|---|---|---|
| `g_ce_context` / `g_ce_rename_id` / `g_ce_rename_buf` / `g_ce_add_filter` / `g_ce_show_minimap` / `g_ce_align_snap` / `g_ce_grid_snap` / `g_ce_show_grid` / `g_ce_request_fit` / `g_ce_minimap` | `composite_editor.cpp` 定义 + `ce_internal.h` extern | `CeUiState` 成员 | **R1 ×10 + R3 ×10** |
| `g_ce_undo` / `g_ce_redo` | `ce_state.cpp` 定义 + extern | `CeUiState::undo/redo` | **R1 ×2 + R3 ×2** |
| `g_ce_prev_positions` | `composite_editor.cpp`（TU 内、外部链接） | `CeUiState::prev_positions` | **R3 ×1** |
| `g_ce_window_focused` | `ce_draw.cpp` 匿名 namespace | `CeUiState::window_focused` | **R2 ×1** |

净效果：`R1 12 → 0`、`R3 18 → 5`、`R2 83 → 82`，**合计 113 → 87**；`ceUiState()` 自身是**函数级 static**（对象成员持有状态）⇒ 只进「参考」行，不计违规。语义零变化：初始化值逐字对齐原定义（`context=nullptr` · 4 个开关 `true` · `request_fit=false` · `window_focused=false`），生命周期仍是进程级。

## 5. 脚本输出（判据可读）

```
层              文件     R1     R2     R3
--------   ------  -----  -----  -----
app             3      0      0      0
ui             68      0     55      5
engine         24      0      0      0
nodes          10      0      0      0
ai             44      0      0      0
web            19      0     15      0
utils          33      0     12      0

参考：函数内 static 局部变量 43 处（函数作用域 · 规则 6 字面范围外 · 不计失败）

checked files: 201   violations: 87   (R1 0 / R2 82 / R3 5)   ·   baseline: 87   new: 0   fixable: 0
结果: PASS（无新增跨帧可变状态；存量见基线文件，按 ARCH-A6/A7 逐步清偿）
```

- 结果行固定含 `checked files:` / `violations:` ⇒ `check_all` 抓这一行做摘要（与 `check_includes.ps1` / `check_links.py` 同款约定）。
- **棘轮模式**（`-Baseline <文件>`）：基线内 ⇒ PASS；**任何基线外新增 ⇒ 逐条打印 + FAIL + exit 1**（不是「有违规就红」，否则接不进 `check_all`）；基线条目被修掉时提示「基线可收缩 —— 已修复 N 条」。
- **无基线**（`-Baseline` 省略）= 严格零容忍（存量清零后 A7 收尾时改用它，并删基线文件）。
- 维护：`-Baseline <文件> -UpdateBaseline` 重写基线（**只许减**；新增违规必须在代码里修，不许写进基线）。

## 6. 反向验证（脚本真的会红）

| 注入 | 预期 | 实测 |
|---|---|---|
| `ui/include/ce_internal.h` 追加 `extern int g_probe_r1;` | R1 新增 1 条 | ✅ `R1 1` · `new: 3` 中列出 |
| `ui/src/texture_cache.cpp` 追加 `static int g_probe_r2 = 0;` | R2 新增 1 条 | ✅ `R2 83` |
| 同文件追加 `int g_probe_r3 = 0;` | R3 新增 1 条（外部链接） | ✅ `R3 6` |

三注入合并跑：`violations: 90` · **`new: 3`** · `结果: FAIL` · **exit 1**。`git checkout` 复原后复跑 ⇒ `violations: 87` · `new: 0` · PASS（无残留）。

## 7. 接入一键自检

- [../../source/tools/check_all.ps1](../../source/tools/check_all.ps1) 新增 **第 ⑦ 项**（在「⑥ 依赖单向」与「文档断链」之间，静态检查归堆），原 ⑦ 文档断链 → **⑧**、⑧ 打包 → **⑨**（脚本内注释 / 打印同步改号）。
- 判据：退出码 0 ⇒ PASS；摘要行取 `checked files: …`；基线文件缺失 ⇒ 脚本抛错 ⇒ 该项 FAIL（防止「基线丢了却显示通过」）。
- 项数：**20 → 21**（`=== check_all 汇总：共 21 项 · 失败 0 项 ===`）。

## 8. 验证结果

| 验证 | 结果 |
|---|---|
| `check_globals.ps1`（单跑 · 棘轮） | ✅ `checked files: 201   violations: 87   (R1 0 / R2 82 / R3 5) · baseline: 87 · new: 0` · exit 0 |
| 注入测试 3 种 | ✅ 全部被捕获（`new: 3` · exit 1） |
| `tools/check_all.ps1` | ✅ **21/21 全 PASS** · exit 0（新项 PASS） |
| `source/build.ps1` | ✅ 构建 **0 error / 0 warning** |
| `aiwrite.exe --run-selftest` | ✅ PASS（零断言增减） |
| `api_probe.exe --exec-selftest` / `--graph-selftest` | ✅ **713 / 0** · **151 / 0** |
| `ce_repro.exe 0` | ✅ `ALL FRAMES OK (mode=0)` |
| `check_includes.ps1` | ✅ 201 文件 / 656 include 行 / 违规 0 |
| `check_links.py` | ✅ **1889 / 0** |

## 9. 验收判据对照（§4 判据 2）

| 判据 | 状态 | 取证 |
|---|---|---|
| **2. 全局变量归零** | 🟡 **部分满足（工具已强制、存量已锁）** | ✅ **跨 TU `extern` = 0**（A5 本批清零，`R1 0`）· 🟡 **文件级 static 可变状态 = 87**（ui 60 / web 15 / utils 12，全部登记在基线文件里，**新增即 FAIL**）· 终值归零 = **ARCH-A6**（ui 60 条逐文件对象化）→ **ARCH-A7**（web/utils 27 条 + 删基线、切严格零容忍） |

## 10. 已知限制

- **语义级扫描，不是编译器**：带函数调用初始化的全局变量不在扫描范围（见 §2 已知限制）；模板实例化 / 宏展开出的静态不解析。
- **函数内 `static` 只统计不判失败**：规则 6 字面范围是「文件级 / 跨 TU」，函数内 static 属函数作用域；当前 43 处，A6/A7 可一并对象化（届时把规则收紧即可）。
- **棘轮基线是过渡形态**：基线条目 = 已登记债，不是「允许」。A7 收尾必须删掉基线文件并把判据改回「严格 0」，否则棘轮会变成永久豁免 —— 这是本批**唯一**需要下一批盯住的收口点。
- **不扫 `source/tools/`（自检程序）**：测试端允许持有全局状态（与 A4 同口径）。

## 11. 回滚点

删除 [../../source/tools/check_globals.ps1](../../source/tools/check_globals.ps1) + [../../source/tools/check_globals_baseline.txt](../../source/tools/check_globals_baseline.txt) + `git checkout -- source/tools/check_all.ps1` 即回滚到 A4 形态（`check_all` 回到 20 项）；`CeUiState` 收口改动可单独 `git checkout -- source/ui/include/ce_internal.h source/ui/src/{ce_state,ce_draw,ce_session,composite_editor}.cpp` 回滚（会退回 12 条 extern）。本审计文档可留作登记。

