# 审计报告 · audit_codebase（20261023）—— 重构完成度复审 + 代码风险与重复度量化

> 日期：2026-10-23 · 触发：用户「现在重构还有没做的部分吗？如果全部完成……再审计一遍代码风险和重复度」
> 范围：`source/` 全部自有代码（`ui/` `app/` `engine/` `nodes/` `ai/` `web/` `utils/` `tools/`），排除 `third_party/`（61 文件 / 76,007 行）与 `source/python/`
> 方法：① 进度核对（[refactor/ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §3 / §4 逐条对照）② `check_all` 21 项门禁实测 ③ **克隆检测**（自写窗口哈希扫描器，K=6 有意义行 / 最大延伸 / 禁止自重叠，`28976` 有意义行 / `28971` 窗口）④ 风险面 grep 取证（静态存储期 / 内存 / 并发 / 危险 API / 文件体量）
> 结论一句话：**重构 18/19 批（A6 / A7 已于同日落地，见 [audit_arch_a6.md](audit_arch_a6.md) · [audit_arch_a7.md](audit_arch_a7.md)；Z2 已落地，见 [audit_arch_z2.md](audit_arch_z2.md)），验收 8 条判据 **8 ✅**（判据 7 已由 Z2 补齐）；代码重复度 **3.7 %**（可接受，但 ai/web 有结构性重复）；P0 风险只剩一类，挂在未完成的 Z1 上。**

---

## §1 重构完成度：还剩 1 批（Z1 · 第 1、2 组已落地 · 余 10 文件）

| 阶段 | 批次 | 状态 | 说明 |
|---|---|---|---|
| 1 运行侧事件化 | R1 · R2 · R3 · R4 | ✅ 4/4 | RunSession · RunEvent 订阅 · ReadOnlyView · 命令路由 |
| 2 编辑侧事件化 | E1 · E2 · E3 · E4 · E5 · E6 | ✅ 6/6 | EventBus · Undo/autosave 订阅 · WorkflowModel · SelectionState · CanvasInteractor · 打包下沉 |
| 3 应用级收口 | A1 · A2 · A3 · A4 · A5 · **A6 · A7** | ✅ 7/7 | AppContext · AutosaveService · `editor()` 退场 · 依赖单向 · 全局变量断言 · **ui 60 条状态对象化** · **web/utils 27 条状态对象化 + 删基线切严格 0** |
| 4 收尾 | ~~**Z2**~~ ✅ · **Z1** 🟡（10/15 文件仍 >800） | 🟡 **1/2**（Z2 已落地 2026-10-23；Z1 第 1 组同日落地：`provider_spec` 1743 → 5 TU · 第 2 组同日落地：`webview_host` / `dom_web_client` / `pydoll_channel` → 15 TU） | 见 §2 |

**验收判据 8 条**（§4）实测状态：

| # | 判据 | 状态 | 证据 |
|---|---|---|---|
| 1 | 行为等价（断言数 ≥ 现状 · selftest 0 失败） | ✅ | exec **713/0** · graph **151/0** · composite **112/0** · upload **72/0** · `--run-selftest` PASS · check_links **1889**（门槛 1730） |
| 2 | 全局变量归零 | ✅ | **跨 TU `extern` = 0 · 文件级 static = 0 · R3 = 0（全库）** —— A5 清 `extern` 12 条 · A6 清 ui 60 条 · A7 清 web 15 + utils 12 条 ⇒ 存量 **113 → 0**；`check_all` 第 ⑦ 项改**严格零容忍**（基线文件已删除）+ 函数内 static 棘轮（只许减；**Z2 后 51 → 50**），实测 `206 文件 / 违规 0 / fn_static 50·50`（[audit_arch_a7.md](audit_arch_a7.md) · [audit_arch_z2.md](audit_arch_z2.md)） |
| 3 | 双画布单套交互 | ✅ | 关键谓词只在 `interactor.cpp`（E5） |
| 4 | 打包-校验同源 | ✅ | `ce_pack.cpp` 零直写（E6） |
| 5 | 依赖单向 | ✅ | `check_includes` 201 文件 / 656 行 / 违规 **0** |
| 6 | `editor()` 退场 | ✅ | `rg` 全库 = 0（A3） |
| 7 | ReadOnlyView 落地 | ✅ | **主画布侧 + ce 侧均完成**（Z2：`drawCompositeEditor(name, open, const CeReadOnlyView&, editor)` · `CompositeEditorState` 单例 → 5 对象 · 详 §3.2 / [audit_arch_z2.md](audit_arch_z2.md)） |
| 8 | `AppContext` 运行时不可访问 | ✅ | UI 面板 0 处代码引用（A3） |

---

## §2 剩余工作量化（不是「感觉」，是数字）

| 批次 | 内容 | 量化 | 关键文件（条数） |
|---|---|---|---|
| ~~**ARCH-A6**~~ ✅ | ui 层文件级可变状态**逐文件对象化** | ~~60 条 / 7 文件~~ → **0** | 已落地 2026-10-23：`settings_panel` 19 → `SettingsPanelState` · `texture_cache` 12 → `class TextureCache` · `node_canvas` 11 → `NodeCanvasState` · `ce_draw` 7 → `CompositeDialogsState` · `node_library` 6 · `console_panel` 3 · `output_panel` 2（[audit_arch_a6.md](audit_arch_a6.md)） |
| ~~**ARCH-Z2**~~ ✅ | `CeSession` 对称拆分（`CeModel` / `CeFile` / `CeSelection` / `CeSpec`）+ ce 渲染 ReadOnlyView 化 | ~~ce 侧 **6 文件 / 1,514 行**~~ → 已落地 2026-10-23 | `ce_draw` 846（读 `view` / 写 `session`）· `ce_run` 223 · `ce_session` 182 + 新增 `ce_model` 74 / `ce_spec` 191 / `ce_file` 98 / `ce_selection` 33 · `composite_editor` 162 · `ce_state` 57（[audit_arch_z2.md](audit_arch_z2.md)） |
| ~~**ARCH-A7**~~ ✅ | web / utils 层 27 条 + **删基线、切严格零容忍** | ~~27 条 / 7 文件~~ → **0** | 已落地 2026-10-23：`webview_host` 8 → `WindowRegistry` + `EnvCache` + `tlsWindow()` · `diagnostics` 7 → `ExceptionCounter` · `pipe_client` 4 → `IpcCounters` + `IpcThreadCounters` · `pydoll_channel` 3 → `SessionState` · `log` 2 → `Loggers` · `config` 2 → `ConfigStore` · `credential` 1 → `AvailabilityCache`；基线文件删除 + 函数内 static 棘轮 41 → 51（[audit_arch_a7.md](audit_arch_a7.md)） |
| **ARCH-Z1** | 超长文件瘦身（目标 <800 行） | **14 目标 → 已达标 5**（`provider_spec.cpp` 1743 → **5 TU / 最大 679** · `webview_host.cpp` 3514 → **8 TU / 最大 607** · `dom_web_client.cpp` 2397 → **4 TU + 2 脚本头 / 最大 583** · `pydoll_channel.cpp` 1282 → **3 TU / 最大 527** · `image_decode.cpp` **758**）；**余 10 个生产文件仍 >800** | 余队列（按批）：`app.cpp` **1454** · `local_nodes.cpp` **1242** · `composite.cpp` **1171** · `node_canvas.cpp` **1126** · `settings_panel.cpp` **944** · `canvas_view.cpp` **938** · `node_registry.cpp` **899** · `executor.cpp` **886** · `property_panel.cpp` **855** · `ce_draw.cpp` **846**（[audit_arch_z1.md](audit_arch_z1.md) §8 · 第 2 组详版 [audit_arch_z1_web.md](audit_arch_z1_web.md) §9） |

> 附带说明（**A7 收口时更正**）：本文初版曾把基线 27 条写成「函数内 `static`」—— 口径错了，那 27 条**全是文件级** `static` / 匿名 `namespace` 内可变变量（web 15 / utils 12）；函数内 `static` 另有 **41 处**（`check_globals` 只统计、不判失败）。A7 因此 = 27 条对象化 + **补函数内 static 棘轮（41 → 51，本批新增 10 个访问器）** + 删基线切严格零容忍 —— 既关掉文件级存量，也关掉「文件级归零、函数内继续长」的绕行口子。

---

## §3 风险清单（按「真会出事」排序，不按字母排序）

### 3.1 P0-① 静态可变状态 =「一次操作触发多次同步 / 关会话残留 / 两画布漂移」的土壤（**A6/A7 后全库已清零**）

| 危害形态 | 证据（**改造前**的形态 · 现均已对象化） | 为什么当时没暴雷 |
|---|---|---|
| 单实例假设 | `settings_panel.cpp` 19 条：`g_edit` / `g_baseline` 是**整份 Config 副本**，`g_off_tpl` / `g_site_tpl` 是模板对象副本 | 应用只开一个设置面板；一旦开「多窗口设置」或改成复用即错乱 |
| 语义漂移 | `node_canvas.cpp` **5 条 R3**（`g_show_minimap` / `g_snap_enabled` / `g_snap_distance` / `g_minimap` / `g_prev_positions`）是**外部链接**全局，`node_library.cpp` / `ce_draw.cpp` 可写 | 双画布已由 E5 收敛交互，但**视图状态仍各写各的全局** |
| 跨会话残留 | `ce_draw.cpp` 7 条（`g_new_dialog_open` / `g_pack_name` / `g_new_name` …）是对话框与输入缓冲 | `ce_repro` 全帧 OK 只验证渲染路径，不验证「关会话后重开」的干净度 |
| 缓存双轨 | `texture_cache.cpp` 12 条（含 `std::` 容器） | `TextureCache` 类成员与文件级缓存并存 |

**判断**：这不是「洁癖债」。规则 6 的机器判据已能拦住**新增**（严格 0 + 棘轮超限 ⇒ exit 1）；**A6 已把 ui 层 60 条全部对象化、A7 已把 web/utils 27 条全部对象化（87 → 0）**，全库再无文件级可变状态，规则 6 在这里是**闭环**，不是口号。

### 3.2 P0-② 判据 7 只覆盖一半：ce 侧渲染仍直读可变对象（**Z2 后已闭环 ✅**）

- 已 `ReadOnlyView&` 化：`drawNodeCanvas` / `drawPropertyPanel` / `drawOutputPanel` / `app.cpp` 的 4 个文本函数。
- ~~未覆盖：`ce_draw.cpp`（865 行）、`canvas_view.cpp`（844 行）、`settings_panel.cpp`（893 行）、`node_library.cpp`（334 行）—— ce 侧渲染直接读 `CompositeEditorState` / `CeUiState` / `Config` 的可变引用。~~
- **Z2 已落地（2026-10-23）**：`drawCompositeEditor(name, open, const CeReadOnlyView&, editor)` —— ce 绘制只经视图读（内部图 / 选中 / 运行快照 / 配置 / 规格 id·名称·dirty），
  写路径仍走 `ceSession()`；`CompositeEditorState` 单例拆成 **5 对象**（`CeModel` / `CeFile` / `CeSelection` / `CeSpec` / `CeSession` · [refactor/ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §1.8.15），
  `rg "compositeEditorState\(" source/` 代码行 = **0**。等价证据 = `ce_repro` **ALL FRAMES OK（mode 0）** + 自检断言零升降（[audit_arch_z2.md](audit_arch_z2.md)）。
- **残留（不属判据 7）**：`ce_draw.cpp` 仍 846 行（体量问题 → Z1）；`ceUiState()` / `Config` 仍是「画布瞬态 / 配置」的可变对象，绘制层经视图或会话读，不经 raw 引用。

### 3.3 P1 中等风险

| 项 | 量化 | 评价 |
|---|---|---|
| 超长文件 | 15 个 >700 行（最大 3195） | 单文件承载 WebView2 + 登录 + PoW + 上传桥接（`webview_host`）—— Z1 首选 |
| provider 骨架重复 | `anthropic↔gemini` 29 行 · `doubao↔kimi` 26 行 · `official↔site_template` 20 行 · provider 头 ↔ `inference_provider.h` 41 行 | 典型「模板方法没抽」：图片编码循环 + 请求骨架 + HTTP 结果处理在 2~4 处各写一遍 |
| 同文件双分支重复 | `workflow_io.cpp:163↔200` 18 行（composite 分支与普通节点分支重复 id/title/params 恢复） | 抽 `restoreNodeFields()` 可一劳永逸（也是「暴露节点保存」bug 的孪生区） |
| 线程面 | `std::mutex` **155** 处 · `thread_local` **6** · `catch (...)` **2**（`dom_web_client.cpp:1569` / `composite.cpp:1159`） | 并发密集但都有锁；两处 `catch(...)` 建议改具体异常 + 必记日志，否则「静默失败」无法诊断 |
| 所有权 | C++ `new` 仅 **1** 处（`webview_host.cpp:2144` `LoginWindow` 进程级单例，有意不释放）· `delete` **0** · `= delete` 20 · `unique_ptr` 3 / `shared_ptr` 56（`webview_host` 34） | 无 `new/delete` 配对泄漏风险；但 `shared_ptr` 偏重，需防环（当前无环证据） |

### 3.4 P2 低风险 / 观察项

| 项 | 量化 | 评价 |
|---|---|---|
| 危险 C API | `strcpy` / `strcat` / `strncpy` / `sprintf` / `gets` / `alloca` / `system()` = **0** | 干净；`ShellExecuteW` 4 处均为「打开 URL / 资源管理器」，参数非拼接命令 |
| 子进程 | `CreateProcessW` 2 处（`pydoll_channel.cpp:167` / `cli_diag.cpp:288`） | 命令行由内部构造，但**未做参数转义单测** —— 建议 A7 顺带补一条断言 |
| 类型转换 | `reinterpret_cast` 31 · `const_cast` 10 | 集中在 WebView2 COM 交互，属 Win32 必要噪声 |
| 代码内 TODO | **0**（债务全在 [DEBT.todo](DEBT.todo) 451 行） | 好处：代码无噪声；坏处：**读代码看不出债务**，只能靠文档。建议 Z1 瘦身时在文件头补一行「本文件已知缺口 → 文档锚点」 |
| 跨层偶然重复 | `cli_diag↔pydoll_channel` 15 行 · `official_template↔autosave` 10 行 | 多为错误处理样板，可忽略 |

---

## §4 重复度审计（首次量化）

**口径**：预处理去注释 / raw string，压空白；丢弃纯括号、预处理、`namespace`、访问修饰符等结构性行；**连续 6 个有意义行完全相同**即记一个克隆窗口，向两侧延伸到最大，同一行只计一次，禁止自重叠。

| 指标 | 数值 |
|---|---|
| 有意义行 | **28,976**（窗口 28,971） |
| 克隆块 | **91** |
| 被覆盖行数 | **1,093** |
| **重复率** | **3.8 %** |

| 层 | 有意义行 | 重复行 | 重复率 | 判读 |
|---|---|---|---|---|
| ui | 8,173 | 86 | **1.1 %** | 最好 —— R/E 阶段的对象化与 interactor 收敛见效 |
| app | 1,702 | 50 | 2.9 % | CLI 诊断样板 |
| utils | 2,786 | 83 | 3.0 % | 可接受 |
| engine | 3,853 | 147 | 3.8 % | `workflow_io` 双分支 + `executor` 校验块 |
| web | 4,399 | 242 | **5.5 %** | `pydoll_channel` 守护进程逻辑两处、`webview_host` `ExecuteScript` 两处 |
| ai | 6,420 | 370 | **5.8 %** | provider 家族骨架重复（最大来源） |
| nodes | 1,643 | 115 | **7.0 %** | `local_nodes` 两个节点函数结构同构 + `text_process_common.h` 与实现重复 |

> **复测（2026-10-23 · ARCH-Z1 第 2 组后）**：有意义行 **29,243** · 克隆块 **90** · 重复行 **1,096** · 重复率 **3.7 %**
> （ai **5.6 %**（原 5.8 %，`provider_spec` 拆分 −1 块）· ui **1.2 %**（原 1.1 %，Z2 新增 5 个 TU 的分母效应）· web 5.5 % · nodes 7.0 % · engine 3.8 % · utils 3.0 % · app 2.9 %）。
> 上表保持审计时点快照不动，复测口径同为 `python source\tools\dup_scan.py`。

**Top 集群**（原始行数）：`anthropic↔gemini` 29 · `doubao↔kimi site_upload` 26 · `official_template↔site_template` 20 · `provider 头↔inference_provider.h` 17+12+12 · `provider_spec↔site_template` 15 · `cli_diag↔pydoll_channel` 15 · 同文件内：`pydoll_channel` 23 · `workflow_io` 18 · `image_decode` 15 · `webview_host` 14 · `executor` 14。

**结论**：

1. **3.8 % 对 3 万行 C++ 属健康区间**（同类项目常见 5~15 %）。重构没有把重复「搬家」，ui 层反而降到 1.1 %。
2. 重复**不是随机噪声，是三簇结构性问题**：① provider 骨架（应模板方法化）② 同函数内「两条分支同一套恢复/校验逻辑」（`workflow_io` / `executor` / `image_decode`）③ 平台样板（守护进程启动 / `ExecuteScript` 包装）。
3. 建议**只治前两簇**（净减约 150~250 行、消除 3 处「改一处漏一处」），第三簇留给 Z1 顺带处理。**不建议**为降指标做机械抽取 —— 当前指标不是问题。

---

## §5 建议的下一批顺序（先解锁判据，再降体量，最后扩覆盖）

| 顺序 | 批次 | 为什么这个顺序 |
|---|---|---|
| 1 | ~~**ARCH-A6**~~ ✅ 已落地（2026-10-23） | 60 条占存量 69 %，且 `node_canvas` 11 条 / `ce_draw` 7 条是 Z2 的直接依赖（先有对象、再拆分）—— 已按此顺序做完，Z2 的 `NodeCanvasState` / `CompositeDialogsState` 前置条件就位 |
| 2 | ~~**ARCH-A7**~~ ✅ 已落地（2026-10-23） | 27 条 + **删基线切严格 0** ⇒ **判据 2 已由 🟡 转 ✅**；`check_globals` 现为零容忍 + 函数内 static 棘轮，规则 6 闭环（比原计划提前：A7 与 Z2 相互独立，先做短的立刻拿下一个 ✅） |
| 3 | ~~**ARCH-Z2**~~ ✅ 已落地（2026-10-23） | 同时补齐**判据 7**（ce 渲染 ReadOnlyView 化）—— 已按「与拆分同一改动面」落地：`CeReadOnlyView` + 5 对象拆分，**判据 7 由 🟡 转 ✅**，`ce_repro` 全帧 OK 作等价证据（[audit_arch_z2.md](audit_arch_z2.md)） |
| 4 | **ARCH-Z1** | 15 个超长文件瘦身（先 `webview_host` / `dom_web_client` / `provider_spec`），顺带治 provider 骨架与平台样板重复 —— 🟡 **第 1 组已落地（2026-10-23）**：`provider_spec` 1743 → 5 TU；下一组 `webview_host`（3514）|
| 5 | 复审 | 重跑本文 §1 / §4 两表 + `check_all` 21 项；重复率目标 **≤ 3.0 %**、判据 **8/8 ✅** |

---

## §6 复现命令

```powershell
# 门禁（21 项 · 含构建）
powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_all.ps1

# 全局变量（A7 后 = 严格零容忍 · 文件级 0 / 函数内 static 棘轮 **50**——Z2 从 51 下调）
powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_globals.ps1
# 函数内 static 按文件分布（棘轮参考清单）
powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_globals.ps1 |
    Select-String -Pattern '^\s+\[' | ForEach-Object { ($_ -split '\] ')[1] -replace ':\d+.*','' } |
    Group-Object | Sort-Object Count -Descending
# 依赖单向
powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_includes.ps1

# 体量
Get-ChildItem source -Recurse -Include *.cpp,*.h -File |
    Where-Object { $_.FullName -notmatch 'third_party|selftest|_test\.' } |
    ForEach-Object { [pscustomobject]@{ L=(Get-Content $_.FullName | Measure-Object -Line).Lines; P=$_.FullName } } |
    Where-Object { $_.L -gt 700 } | Sort-Object L -Descending

# 重复度（克隆检测 · 只报不判 · 未入 check_all）
python source\tools\dup_scan.py
```

> **已入库的可复现载体** = [../../source/tools/dup_scan.py](../../source/tools/dup_scan.py)（`--k` 窗口行数 / `--top` 打印条数 / `--layers` 只扫指定层）；本次实测即该脚本输出，与报告 §4 数字逐项一致。
> **待定**：是否把它接入 `check_all`（第 ⑩ 项，棘轮基线同 A5 形态）。建议**等 Z1 做完再入闸** —— 否则基线一次性写 91 条、随后大批失效，维护成本高于收益；当前形态下它是「审计工具」而非「门禁」。

---

## §7 变更记录

| 日期 | 变更 |
|---|---|
| 2026-10-23 | 首版：重构完成度复审（15/19 批 · 判据 6 ✅ / 2 🟡）+ 风险清单（P0×2 / P1×5 / P2×5）+ 重复度首次量化（3.8 % · 91 块）+ 下批顺序建议；同步 [ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §4 |
| 2026-10-23 | **A7 回填：17/19 批 · 判据 7 条 ✅ + 1 🟡**（文件级 static 87 → 0 · 基线删除 · 严格零容忍 + 函数内 static 棘轮 51 · 判据 2 转 ✅）；更正初版 §2 的「27 条函数内 static」口径（实为 web/utils 的**文件级** static；函数内 static 另有 41 处）；§5 顺序表按「A7 已提前落地」重排（Z2 → Z1 → 复审）；§6 复现命令改严格模式（[audit_arch_a7.md](audit_arch_a7.md)） |
| 2026-10-23 | **Z1 回填（第 1 组）：§1 第 4 行标 🟡 1/13 文件 · §2 Z1 行改用「15 目标 → 已达标 2」口径并列出剩余 13 个文件的**实测**行数（原表头是审计时点快照，`provider_spec` 1609 → 实测 1743 等）· §4 增「复测」段（克隆块 90 / 重复率 **3.7 %**）· §5 第 4 序标第 1 组已落地（[audit_arch_z1.md](audit_arch_z1.md)） |
| 2026-10-23 | **Z1 回填（第 2 组）：§1 第 4 行改 🟡 10/15 文件 · §2 Z1 行改「14 目标 → 已达标 5」并重列剩余 10 个文件的实测行数 · §4 复测标第 2 组后 · §5 第 4 序补第 2 组已落地；新详版审计 [audit_arch_z1_web.md](audit_arch_z1_web.md)（web/DOM 三文件 7193 → 15 TU · 契约头 3 个 + 脚本头 2 个 · 夹具抽取口修正 · 40 块逐字 0 差异）** |





