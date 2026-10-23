# 审计报告 · audit_codebase（20261023）—— 重构完成度复审 + 代码风险与重复度量化

> 日期：2026-10-23 · 触发：用户「现在重构还有没做的部分吗？如果全部完成……再审计一遍代码风险和重复度」
> 范围：`source/` 全部自有代码（`ui/` `app/` `engine/` `nodes/` `ai/` `web/` `utils/` `tools/`），排除 `third_party/`（61 文件 / 76,007 行）与 `source/python/`
> 方法：① 进度核对（[refactor/ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §3 / §4 逐条对照）② `check_all` 21 项门禁实测 ③ **克隆检测**（自写窗口哈希扫描器，K=6 有意义行 / 最大延伸 / 禁止自重叠，`28976` 有意义行 / `28971` 窗口）④ 风险面 grep 取证（静态存储期 / 内存 / 并发 / 危险 API / 文件体量）
> 结论一句话：**重构 16/19 批（A6 已于同日落地，见 [audit_arch_a6.md](audit_arch_a6.md)），验收 8 条判据 6 ✅ + 2 🟡；代码重复度 3.8 %（可接受，但 ai/web 有结构性重复）；P0 风险只剩两类，都挂在未完成的 A7/Z2 上。**

---

## §1 重构完成度：还剩 4 批

| 阶段 | 批次 | 状态 | 说明 |
|---|---|---|---|
| 1 运行侧事件化 | R1 · R2 · R3 · R4 | ✅ 4/4 | RunSession · RunEvent 订阅 · ReadOnlyView · 命令路由 |
| 2 编辑侧事件化 | E1 · E2 · E3 · E4 · E5 · E6 | ✅ 6/6 | EventBus · Undo/autosave 订阅 · WorkflowModel · SelectionState · CanvasInteractor · 打包下沉 |
| 3 应用级收口 | A1 · A2 · A3 · A4 · A5 · **A6** | ✅ 6/6 | AppContext · AutosaveService · `editor()` 退场 · 依赖单向 · 全局变量棘轮 · **ui 60 条状态对象化** |
| 4 收尾 | **A7 · Z1 · Z2** | ⬜ **0/3**（原 A6 已从本阶段移出并落地） | 见 §2 |

**验收判据 8 条**（§4）实测状态：

| # | 判据 | 状态 | 证据 |
|---|---|---|---|
| 1 | 行为等价（断言数 ≥ 现状 · selftest 0 失败） | ✅ | exec **713/0** · graph **151/0** · composite **112/0** · upload **72/0** · `--run-selftest` PASS · check_links **1889**（门槛 1730） |
| 2 | 全局变量归零 | 🟡 | **跨 TU `extern` = 0 ✅**；**文件级 static = 0 ✅（ui 层，A6）**；基线剩 **27 条全是函数内 static**（web 15 / utils 12）⇒ 判据 2 的最后一门 = A7 |
| 3 | 双画布单套交互 | ✅ | 关键谓词只在 `interactor.cpp`（E5） |
| 4 | 打包-校验同源 | ✅ | `ce_pack.cpp` 零直写（E6） |
| 5 | 依赖单向 | ✅ | `check_includes` 201 文件 / 656 行 / 违规 **0** |
| 6 | `editor()` 退场 | ✅ | `rg` 全库 = 0（A3） |
| 7 | ReadOnlyView 落地 | 🟡 | **仅主画布面板完成**；ce 侧仍直读对象（详 §3.2） |
| 8 | `AppContext` 运行时不可访问 | ✅ | UI 面板 0 处代码引用（A3） |

---

## §2 剩余工作量化（不是「感觉」，是数字）

| 批次 | 内容 | 量化 | 关键文件（条数） |
|---|---|---|---|
| ~~**ARCH-A6**~~ ✅ | ui 层文件级可变状态**逐文件对象化** | ~~60 条 / 7 文件~~ → **0** | 已落地 2026-10-23：`settings_panel` 19 → `SettingsPanelState` · `texture_cache` 12 → `class TextureCache` · `node_canvas` 11 → `NodeCanvasState` · `ce_draw` 7 → `CompositeDialogsState` · `node_library` 6 · `console_panel` 3 · `output_panel` 2（[audit_arch_a6.md](audit_arch_a6.md)） |
| **ARCH-Z2** | `CeSession` 对称拆分（`CeModel` / `CeFile` / `CeSelection` / `CeSpec`）+ ce 渲染 ReadOnlyView 化 | ce 侧 **6 文件 / 1,514 行** | `ce_draw.cpp` 865 · `ce_run.cpp` 203 · `ce_session.cpp` 187 · `composite_editor.cpp` 139 · `ce_state.cpp` 77 · `ce_pack.cpp` 43 |
| **ARCH-A7** | web / utils 层 27 条 + **删基线、切严格零容忍** | **27 条 / 7 文件** | `webview_host.cpp` **8** · `diagnostics.cpp` **7** · `pipe_client.cpp` 4 · `pydoll_channel.cpp` 3 · `log.cpp` 2 · `config.cpp` 2 · `credential.cpp` 1 |
| **ARCH-Z1** | 超长文件瘦身（目标 <800 行） | **15 文件 >700 行**（其中 **14 >800**） | `webview_host.cpp` **3195** · `dom_web_client.cpp` **2301** · `provider_spec.cpp` **1609** · `app.cpp` **1321** · `pydoll_channel.cpp` 1188 · `local_nodes.cpp` 1175 · `composite.cpp` 1069 · `node_canvas.cpp` 1017 · `settings_panel.cpp` 893 · `ce_draw.cpp` 865 · `canvas_view.cpp` 844 · `node_registry.cpp` 841 · `property_panel.cpp` 807 · `executor.cpp` 805 · `image_decode.cpp` 705 |

> 附带说明：**函数内** `static` 现 **27 处**（A6 已顺带收掉 `node_canvas` 的 3 条）；`check_globals` 把它们也计入 R2 与基线，所以 A7 = 这 27 条对象化 + **删基线切严格零容忍**，不再留「文件级归零、函数内继续长」的绕行口子。

---

## §3 风险清单（按「真会出事」排序，不按字母排序）

### 3.1 P0-① 静态可变状态 =「一次操作触发多次同步 / 关会话残留 / 两画布漂移」的土壤（A6 后 ui 层已清零）

| 危害形态 | 证据 | 为什么现在没暴雷 |
|---|---|---|
| 单实例假设 | `settings_panel.cpp` 19 条：`g_edit` / `g_baseline` 是**整份 Config 副本**，`g_off_tpl` / `g_site_tpl` 是模板对象副本 | 应用只开一个设置面板；一旦开「多窗口设置」或改成复用即错乱 |
| 语义漂移 | `node_canvas.cpp` **5 条 R3**（`g_show_minimap` / `g_snap_enabled` / `g_snap_distance` / `g_minimap` / `g_prev_positions`）是**外部链接**全局，`node_library.cpp` / `ce_draw.cpp` 可写 | 双画布已由 E5 收敛交互，但**视图状态仍各写各的全局** |
| 跨会话残留 | `ce_draw.cpp` 7 条（`g_new_dialog_open` / `g_pack_name` / `g_new_name` …）是对话框与输入缓冲 | `ce_repro` 全帧 OK 只验证渲染路径，不验证「关会话后重开」的干净度 |
| 缓存双轨 | `texture_cache.cpp` 12 条（含 `std::` 容器） | `TextureCache` 类成员与文件级缓存并存 |

**判断**：这不是「洁癖债」。规则 6 的机器判据已能拦住**新增**（`new > 0 ⇒ exit 1`）；**A6 已把 ui 层 60 条全部对象化（87 → 27）**，剩余 27 条函数内单实例由 **ARCH-A7** 收口（`webview_host` / `diagnostics` / `pipe_client` 等）。

### 3.2 P0-② 判据 7 只覆盖一半：ce 侧渲染仍直读可变对象

- 已 `ReadOnlyView&` 化：`drawNodeCanvas` / `drawPropertyPanel` / `drawOutputPanel` / `app.cpp` 的 4 个文本函数。
- 未覆盖：`ce_draw.cpp`（865 行）、`canvas_view.cpp`（844 行）、`settings_panel.cpp`（893 行）、`node_library.cpp`（334 行）—— ce 侧渲染直接读 `CompositeEditorState` / `CeUiState` / `Config` 的可变引用。
- **后果**：ce 侧任一渲染改动仍可能顺手改状态（渲染与状态未解耦）；Z2 拆分 `CeSession` 前没有「只读视图」这层缓冲，拆分风险更高。
- **对策**：Z2 与「ce 渲染 ReadOnlyView 化」同批做（同一改动面，等价证据靠 `ce_repro` 全帧对比）。

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
| 2 | **ARCH-Z2** | 同时补齐**判据 7**（ce 渲染 ReadOnlyView 化）—— 与拆分同一改动面，`ce_repro` 全帧对比可作等价证据 |
| 3 | **ARCH-A7** | 27 条 + **删基线切严格 0** ⇒ 判据 2 由 🟡 转 ✅；此后 `check_globals` 变成零容忍，规则 6 彻底闭环 |
| 4 | **ARCH-Z1** | 15 个超长文件瘦身（先 `webview_host` / `dom_web_client` / `provider_spec`），顺带治 provider 骨架与平台样板重复 |
| 5 | 复审 | 重跑本文 §1 / §4 两表 + `check_all` 21 项；重复率目标 **≤ 3.0 %**、判据 **8/8 ✅** |

---

## §6 复现命令

```powershell
# 门禁（21 项 · 含构建）
powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_all.ps1

# 全局变量棘轮（A6 后 27 条 / 按文件分布）
powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_globals.ps1 `
    -Baseline .\source\tools\check_globals_baseline.txt -SummaryOnly
Select-String -Path .\source\tools\check_globals_baseline.txt -Pattern '^R[123]\|' |
    ForEach-Object { ($_ -split '\|')[1] } | Group-Object | Sort-Object Count -Descending

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
| 2026-10-23 | 首版：重构完成度复审（15/19 批 · 判据 6 ✅ / 2 🟡）+ 风险清单（P0×2 / P1×5 / P2×5）+ 重复度首次量化（3.8 % · 91 块）+ 下批顺序建议；同步 [ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §4 判据 7 标 🟡 |




