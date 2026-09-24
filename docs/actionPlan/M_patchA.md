# M_patchA：地基补丁系列 —— 结果回流与可观测性

> 类型：跨里程碑「地基补丁」（不占用 M1–M6 编号，与 `M1.md … M6.md` 平级互链）
> 依据：2026-09-23 全库底层逻辑审计（见 §1）
> 版本目标：v0.2.x（在已收口的 M2 之上补体验地基）
> 预计工期：全系列 5–6 天（全职）/ 12–15 天（业余）；**Patch A 单批 ≈1.2 天**
> 实施状态：🟡 进行中 —— **A1 已完成**（PA-01/PA-04/PA-07，2026-09-24）；**A2 进行中**：**PA-02 输出面板 ✅**（2026-09-24），其余（PA-03/05/06/08/09）待做

### 实施进度（2026-09-24）

| 阶段 | 任务 | 状态 | 实测 / 证据 |
|---|---|---|---|
| A1 | PA-01 执行器只读运行信息 | ✅ | `NodeRunInfo` + `Executor::runInfos()`；`api_probe --exec-selftest` **62 通过 / 0 失败**（+6 项断言） |
| A1 | PA-04 逐节点耗时写入 `workflow.log` | ✅ | `[运行明细] n1(TextInput)=done/0ms … n3(LLMGenerate)=done/7167ms …`；节点行带耗时（完成/失败/跳过） |
| A1 | PA-07 配置访问器地基 | ✅ | `app_config()/set_app_config()`（mutex 保护）；app 启动与保存后同步；api_probe 启动载入；`--selftest` 断言通过 |
| A2 | PA-02 输出面板（全文 / 复制 / 导出） | ✅ | 新增 `ui/output_panel.{h,cpp}`（`node_output_text` / `run_output_text` 供面板、导出与自检共用）；视图菜单「输出」+ 接线 `config.ui.show_output_window`（切换即持久化）；停靠于 Console 同区；只读消费 `runInfos()`+`outputs()` |
| A2 | PA-02 无界面验证 | ✅ | `--run-selftest --web`：输出面板文本 **501 字符**、预览 `[n1] TextInput · done · 0 ms`、**VA-07 工作流 JSON 不含运行结果 = OK**、PASS（5.51s）；离线模式 613 字符（含错误/跳过段）、VA-07 OK、PASS |
| A2 | **PA-03 结果呈现（节点摘要 + 参数面板结果区）** | ✅ | 新增 `ui/text_view.{h,cpp}`（只读文本/复制共用，输出面板与参数面板同源）；`node_canvas` 节点内结果摘要（状态着色 + 首 160 字符 + 字符数，签名缓存避免每帧重算）；`property_panel` 新增「运行结果」折叠区（状态/耗时/错误 + 只读全文 + 复制 + 归档路径） |
| A2 | **VA-01 三处一致断言** | ✅ | `--run-selftest` 断言「输出面板全文 ⊇ 每个节点全文」：离线 **3 节点 / 613 字符**、网页版 **4 节点 / 2397 字符**，均 OK |
| A2 | **P1-a 生效提供商可见性** | ✅ | 新增 `engine/provider_resolve.{h,cpp}`（provider 输入优先的单点实现）；参数面板显示 `生效：official（来自 提供商配置 n5）` + 红字「必定失败」+ 「把 提供商配置 n5 改为 web」一键按钮（先压快照→可撤销）；节点体显示 `生效 official ← n5` |
| A2 | **P1-b 运行前提示（拦截）** | ✅ | `validateBeforeRun` **追加** warning（保持"不阻断"，保住既有断言）；工具栏「▶ 运行」预检 → 确认弹窗（受影响清单 + 「切换为网页版并运行」/「仍要运行」/「取消」）。回归不变：七组 PASS / 95-0 / 72-0 / 3-of-5 / 5-of-5 / 构建 0-0 |
| A2 | PA-05 Console 增强 / PA-06 错误条 / PA-08 未接配置治理 / PA-09 文档收尾 | ⬜ 待做 | — |
| A2 | 回归基线（PA-02 后） | ✅ | `--selftest` 七组 PASS；`--graph-selftest` 95/0；`--exec-selftest` 62/0；`--run-selftest` PASS（3/5 预期）；`--run-selftest --web` 5/5；构建 0 error / 0 warning |
| C | PC-05 输出归档（提前落地） | ✅ | 新增 `utils/output_archive.{h,cpp}`：运行结束写 `outputs/<yyyyMMdd-HHmmss>-<工作流名>/`（每节点 `.txt` 含元信息头 + `run.json`）；同秒多次运行自动加序号且**本次归档永不删除**；工作流名非法字符清洗；不可写路径返回错误不抛异常。`editor_state` 运行结束自动归档 + `last_archive_dir`；输出面板显示归档路径并可「复制归档路径」 |
| C | PC-06 归档保留策略（部分） | ✅ | **接线 `config.output.{archive_dir, keep_history, max_history, ttl_days}`**：`keep_history=false` 只留 1 份、`true` 按 `max_history`；`ttl_days>0` 清理超期目录（只清理符合命名规则的目录，先统计后删除并写日志）。`auto_open_on_complete` 仍留 M5 |
| C | PC-05/06 验证 | ✅ | `--exec-selftest` **72 通过 / 0 失败**（+10 项归档断言：run.json、节点 .txt、无输出不写 txt、名字清洗、元信息头、run.json 明细、保留份数、不可写路径、ttl 清理）；`--run-selftest --web` → **PC-05 归档 OK**（4 个节点文件 + run.json，含生成文档=是）；自检用临时目录，**不污染用户 outputs** |
| B | PB-xx 待做 | ⬜ | 执行线程化 / 流式 / Provider 统一（含官方 API）/ 凭据管理器 / 会话失效引导 |
> 当前基线（全绿）：`api_probe --selftest` 七组 PASS、`--graph-selftest` 95/0、`--exec-selftest` 56/0、
> `aiwrite --run-selftest` PASS、`--run-selftest --web` **5/5 ≈10s**、`--web-probe` PASS、`--web-chat` PASS

---

## §0 文档元信息与约定

### 0.1 定位

M1 已收口、M2 全绿、M3/M4 大部分落地，但审计发现**一批"地基"性质的逻辑尚未实施**（结果不回显、执行不可观测、配置大半未接、无自动保存等）。
这些缺口不属于任何单一里程碑，却是后续所有体验优化的前提，因此以「补丁系列」独立推进，**不改变里程碑编号**，完成后再回到 M4/M5/M6 主线。

### 0.2 编号规则

| 前缀 | 含义 | 示例 |
|---|---|---|
| `PA-` / `PB-` / `PC-` / `PD-` / `PM-` | 任务（补丁 A / B / C / D / 并行小项） | `PA-01` |
| `VA-` / `VB-` / `VC-` / `VD-` / `VM-` | 技术验证项 | `VA-03` |
| `AA-` / `AB-` / `AC-` / `AD-` / `AM-` | 验收标准 | `AA-02` |

### 0.3 执行原则（贯穿全系列）

1. **运行态值 vs 文档值**：节点执行结果（`Executor::outputs()`）**永不**进入 `Graph`、撤销快照或工作流 JSON。
2. **只读暴露**：UI 通过只读 API 读取执行产物；一切写入仍走 `EditorState` 的快照事务（撤销栈一致）。
3. **脱敏**：Cookie / userToken / API Key 只驻留内存，日志与导出文件一律脱敏（沿用现有 `mask_value` 风格）。
4. **一次提交一个补丁**：每个补丁独立可验证、可回滚；提交信息附实测数据。
5. **不引入新依赖**：已有 spdlog / nlohmann / httplib / OpenSSL / ImGui / WebView2 足够。

### 0.4 回归基线（每次提交必须重跑并贴结果）

| 命令 | 期望 |
|---|---|
| `api_probe.exe --selftest` | 七组全 PASS（含新增 V-11/V-12） |
| `api_probe.exe --graph-selftest` | 95 通过 / 0 失败（随补丁增长） |
| `api_probe.exe --exec-selftest` | 56 通过 / 0 失败（随补丁增长） |
| `aiwrite.exe --run-selftest` | PASS（离线：LLMGenerate 走 official 占位，3/5 为预期） |
| `aiwrite.exe --run-selftest --web` | **5/5，失败 0，跳过 0**（真实网页版生成） |
| `aiwrite.exe --web-probe` / `--web-chat "<提示词>"` | PASS |
| 构建 | 0 error / 0 warning |

---

## §1 审计总表（缺口 → 任务映射）

审计方式：全库检索占位标记、`Config` 字段消费点、UI 入口接线情况、执行器/持久化/错误路径的调用链。

### 1.1 A 类 —— 结果回流与可观测性（最痛）

| 缺口 | 证据 | 影响 |
|---|---|---|
| 运行结果零消费者 | `Executor::outputs()` 在 `src/ui/**` **无任何引用**（仅自检使用） | 生成正文只在 Console 留一行；节点内不显示、无"上次结果"、重启即丢、M5 Output 窗口无数据源 |
| Console 无法取出文本 | `ui/console_panel.cpp` 仅 `LevelFilter` + 搜索框（L14/16/17） | 长文只能截图/手抄 |
| 无节点耗时 | `engine/executor.cpp` 无 `log::workflow` 调用，仅 `summary()` 总耗时（L254/L261） | 慢在哪一步不可知（M4-13 登记项） |
| 无错误呈现 | `app.cpp` 无 `OpenPopup/Toast` | 失败只落 Console/状态栏/节点红点，易忽略 |

### 1.2 B 类 —— 执行层基础

| 缺口 | 证据 | 影响 |
|---|---|---|
| 同步阻塞 UI | `engine/executor.h` L9-10 自陈；`tick()` 在 UI 帧内执行节点 | 生成 4–10 s 界面卡死，长文更久 |
| `cancel()` 不中断当前节点 | 同上 | 「■ 停止」只在节点边界生效 |
| 无流式/进度回调 | `ExecutionContext` 仅 `outputs/cancelled/on_console/on_node_state`（L62-75） | 流式显示（M4-11）与官方 SSE（M4-10）缺接口 |
| 无节点超时/重试 | executor 无相关逻辑；`config.timeout/error` 未被任何代码读取 | 卡住的请求只能等到底 |
| 无运行快照 | `paths::snapshots_dir()` 已建目录但零引用 | 崩溃/重启后结果与上下文全丢 |

### 1.3 C 类 —— 配置层

| 配置项 | 现状 | 影响 |
|---|---|---|
| `ui.grid_size / console_height / running_animation / show_output_window` | 仅 `config.cpp` 读写；UI 只用 `show_grid/show_console/show_node_library/show_property_panel` | 改了无效果 |
| `general.language / startup` | 零引用 | 无 i18n、无启动引导 |
| `output.*`（archive_dir/ttl_days/auto_open_on_complete/keep_history/max_history） | 零引用 | 输出不归档（M5 §11 未做） |
| `timeout.* / error.*` | 零引用 | provider 超时/重试未生效 |
| `advanced.log_dir / log_ttl_days` | 零引用（旋转已有界，影响低） | 语义未实现 |
| `deepseek.*` | 与节点参数默认值**两套来源**，config 段未被读取 | 易踩"改了 config 怎么没用" |
| 无进程级配置访问器 | `Config` 仅存在于 `app.cpp` 局部变量（L328-329） | 非 UI 代码（provider/工具）无法读取配置 |
| 配置落盘 | 仅 `[ui]` 可见性会 `save_config`（app.cpp L498-511） | 其它项改动不持久 |

### 1.4 D 类 —— 会话与凭据

| 缺口 | 证据 | 影响 |
|---|---|---|
| `api_key_ref` 无实现 | `node_registry.cpp` L311 定义，无消费方 | §8.4 凭据管理器未落地（M4-07） |
| Key 仅内存/环境变量 | ProviderConfig secret 参数 + `DEEPSEEK_API_KEY` | 重启需重输 |
| 会话失效无检测与引导 | 只有通用错误串，无 `40002/401` 特判 | Token 过期时提示"未解析出文本"，用户不知要重登（违背 §8.5） |
| 无代理/证书策略 | httplib 默认；仅 `api_probe --insecure` 有此选项 | 企业网络/自签环境无法使用 |

### 1.5 E 类 —— 交互基础

| 缺口 | 证据 | 影响 |
|---|---|---|
| 快捷键几乎全无 | `node_canvas.cpp` L237 `ed::EnableShortcuts(false)`；全库无 `ImGuiKey_*` 处理 | 设计 §6.7 未实现 |
| 复制/粘贴暂停 | `toolbar.cpp` L44、`app.cpp` L476、`node_canvas.cpp` L691/703 三处禁用；根因=粘贴后画布位置同步+视图跟随产生异常坐标 | 高频操作缺失 |
| 窗口几何不持久化 | 无 `glfwGet/SetWindowSize`；config 无对应字段 | 每次启动固定"九成屏" |
| 参数面板无重置/清空/批量 | property_panel 无入口 | 参数多时逐个改 |
| 无"重跑"能力 | 失败后只能整图重跑 | 调试迭代效率低 |

### 1.6 F 类 —— 数据与文件

| 缺口 | 影响 |
|---|---|
| 无自动保存/崩溃恢复（`snapshots_dir` 空置） | 意外退出丢工作流（当前最大数据风险） |
| 最近列表不校验存在性 | 打开失效条目报错（非致命） |
| 工作流 `version` 只 warn 不迁移（`workflow_io.cpp` L104-107） | 跨版本兼容无保障 |
| `config_version` 无迁移 | 配置结构演进无路径 |
| 输出不归档、无 TTL | M5 §11 未做；输出无法追溯 |

### 1.7 G 类 —— 占位与远期

`VLMGenerate`（M5-02）、`ImagePreview` 仅 Console（M5）、Output 窗口（M5）、i18n、M6-02 WebView2 Runtime 引导、M6-08 打包、无 CI / 无构建脚本一键化。

### 1.8 明确不做（Out of Scope）

多工作流标签页 / 协同编辑 / 云同步 / 自动更新 / 插件系统 —— 与 MVP 定位无关，不纳入补丁系列。

---

## §2 补丁系列总览（按顺序执行）

| 补丁 | 名称 | 覆盖缺口 | 依赖 | 工作量（全职） | 提交时验证 |
|---|---|---|---|---|---|
| **A**（本文件详述） | 结果回流与可观测性 + 配置访问器地基 | A、C（地基） | — | ≈1.2 天 | VA-01…VA-08 |
| **B** | 执行异步化 + 流式 + Provider 统一（含官方 Provider、凭据管理器、会话失效、网络策略） | B、D | A | 1.5–2 天 | VB-01…VB-09 |
| **C** | 数据安全（autosave/恢复、最近列表、版本迁移、输出归档最小集） | F | A | ≈1 天 | VC-01…VC-06 |
| **D** | 交互打磨与配置接线（快捷键、复制粘贴重做、设置面板、窗口几何、重跑、i18n 决策） | E、C（全量） | B、C | ≈1.5 天 | VD-01…VD-08 |
| 并行 | 小项（M6-02 WebView2 检测、文档索引治理、构建脚本、诊断信息） | G | — | 0.5–1 天 | VM-01…VM-04 |

**顺序理由**：A 建立"结果数据通道 + 界面呈现 + 配置访问器"，是 B 的流式呈现与 C 的归档、D 的设置面板共同的底座；B 提升执行体验上限；C 保障数据不丢；D 做手感与配置闭环。

---

## §3 M_patchA（本补丁详述）—— 结果回流与可观测性

### 3.1 目标与用户可见收益

| 目标 | 用户可见收益 | 验证点 |
|---|---|---|
| 结果**可见** | 跑完立刻在节点内看到正文摘要，选中节点可看全文 | VA-01 |
| 结果**可用** | 输出面板一键复制全文 / 导出为文件 | VA-02 |
| 过程**可观测** | `workflow.log` 逐节点耗时；Console 可复制/导出/自动滚动 | VA-03 / VA-05 |
| 错误**可见** | 失败出现可关闭错误条，点击定位到失败节点 | VA-04 |
| 地基**可用** | 进程内统一配置访问器；未接配置字段不再"改了没反应" | VA-06 |

### 3.2 任务清单

#### PA-01 执行器运行信息只读暴露 ✅（2026-09-24 · A1）

| 项目 | 内容 |
|---|---|
| 目标 | UI 能读「每节点状态 / 耗时 / 错误」，且不污染 `Graph` 与撤销快照 |
| 步骤 | 1. 定义 `struct NodeRunInfo { std::string node_id, type; NodeState state; double duration_ms; std::string error; std::size_t delta_bytes; }`（`delta_bytes` 为 Patch B 预留，只增不改）<br>2. `Executor` 增 `const std::vector<NodeRunInfo>& runInfos() const`（与 `outputs()` 同风格，只读）<br>3. `executeNode()` 前后 `steady_clock` 打点；完成/失败/跳过三条路径都写入<br>4. `reset()`/`start()` 清空列表，保证一次运行一份记录 |
| 验证 | 自检断言：记录数 = 计划节点数；耗时 ≥0；状态与 `Graph` 一致；失败节点含 `error`；跳过节点有记录 |
| 产出 | `src/engine/executor.{h,cpp}` |

#### PA-02 输出面板（Output 窗口最小可用版）✅（2026-09-24 · A2）

| 项目 | 内容 |
|---|---|
| 目标 | 运行结果有专门落点，可停靠、可复制、可导出 |
| 步骤 | 1. 新增 `src/ui/output_panel.{h,cpp}`：`draw_output_panel(EditorState&)`（默认隐藏、可停靠）<br>2. 数据源：`executor.runInfos()` + `executor.outputs()`（按拓扑序分段：`节点名 · 类型 · 状态 · 耗时` + 正文）<br>3. 正文用 `ImGuiListClipper` 虚拟化；超长正文按 4 万字符截断显示并提示"已截断（导出完整）"<br>4. 按钮：复制全文 / 复制该节点 / 导出到文件（复用 `utils::save_file()`）<br>5. 「视图」菜单加「输出」开关，**首次接线 `config.ui.show_output_window`**：切换即 `save_config`（与现有 `[ui]` 可见性持久化一致）<br>6. 空态提示：`尚无运行结果：点「▶ 运行」` |
| 验证 | `--run-selftest --web` 后需在 UI 手工确认；自检侧断言"导出函数输出 == `outputs()` 拼接结果"；重启后开关状态被记住 |
| 产出 | 新面板 + `ui/app.cpp`（菜单/停靠/绘制调用）+ `config` 接线 |

#### PA-03 结果呈现：节点摘要 + 参数面板「运行结果」✅（2026-09-24 · A2/F4）

| 项目 | 内容 |
|---|---|
| 目标 | 不跳面板也能看到结果；可复制全文 |
| 步骤 | 1. `ui/node_canvas.cpp`：参数预览之后追加结果摘要（首 2 行 / 共 N 字符；失败节点显示红色错误首行）<br>2. `ui/property_panel.cpp`：选中节点时新增「运行结果」折叠区（只读多行 + 复制按钮）<br>3. 两处**只读**，禁止写回 `params`，不触发撤销快照 |
| 验证 | 画布可见摘要；面板可复制；编辑参数/撤销/重做不影响结果展示；快照纯净（VA-07） |
| 产出 | `ui/node_canvas.cpp`、`ui/property_panel.cpp` |

#### PA-04 逐节点耗时写入 `workflow.log` ✅（2026-09-24 · A1）

| 项目 | 内容 |
|---|---|
| 目标 | 完成 M4-13 登记项「节点耗时明细」 |
| 步骤 | 1. `Executor` 在开始/完成/失败/跳过写 `log::workflow`：`node=<id> type=<t> state=<s> ms=<n> err=<msg>`<br>2. 运行结束写统计（计数 + 各节点耗时降序前 5）<br>3. 与 Console 输出职责分离（Console 仍由 `EditorState` 的 console handler 负责） |
| 验证 | 运行后 `workflow.log` 含逐节点行；自检读取日志断言含本次 node_id |
| 产出 | `src/engine/executor.cpp`（+ 自检） |

#### PA-05 Console 增强：复制 / 导出 / 自动滚动

| 项目 | 内容 |
|---|---|
| 目标 | 长文本可一键取出（当前只能截图） |
| 步骤 | 1. 顶部工具条：`复制全部` / `复制当前过滤` / `导出到文件`<br>2. 自动滚动开关（默认开）；用户上滚时自动暂停并显示「回到底部」按钮，点击后恢复<br>3. 保持 `kMaxEntries = 10000` 上限与现有过滤/搜索语义不变 |
| 验证 | 复制内容与过滤/搜索后的可见行一致；导出为 UTF-8 文本且行数一致 |
| 产出 | `src/ui/console_panel.cpp` |

#### PA-06 轻量错误条

| 项目 | 内容 |
|---|---|
| 目标 | 失败不再被忽略 |
| 步骤 | 1. `EditorState` 增 `last_error { node_id, message, at }`，运行结束时写入首个失败节点信息；成功运行则清空<br>2. 状态栏右侧错误色条（单行省略 + 悬停显示全文 + 关闭按钮）<br>3. 点击 → 选中该节点并 `NavigateToContent`（复用现有视图跟随）<br>4. 连续错误覆盖显示，不堆积 |
| 验证 | 失败运行出现错误条；点击后画布选中失败节点；关闭后不再出现直到下次失败 |
| 产出 | `src/ui/editor_state.{h,cpp}`、`src/ui/app.cpp`（必要时新增 `ui/error_bar.{h,cpp}`） |

#### PA-07 配置访问器地基 ✅（2026-09-24 · A1）

| 项目 | 内容 |
|---|---|
| 目标 | 让非 UI 代码（provider / 工具 / 自检）能读配置，为 Patch B 的超时重试与 Patch D 的设置面板铺路 |
| 步骤 | 1. `utils/config.{h,cpp}` 增 `const Config& app_config(); void set_app_config(Config);`（进程级缓存，线程安全只读）<br>2. `app.cpp` 启动时 `load_config` 后调用 `set_app_config`（替换现有局部 `Config config;`）<br>3. 无 UI 场景（`api_probe`）按需 `load_config + set_app_config`，保持工具独立性 |
| 验证 | 自检断言：设置后读取一致；未设置时返回默认值；现有 `config` 往返自检不变 |
| 产出 | `src/utils/config.{h,cpp}`、`src/ui/app.cpp` |

#### PA-08 未接配置字段治理

| 项目 | 内容 |
|---|---|
| 目标 | 消除"改了 config 却无效果"的困惑 |
| 步骤 | 1. 对 `ui.grid_size / console_height / running_animation` 三个能立刻接线的字段接线（网格尺寸传 `CanvasOptions`、Console 初始高度、动效开关控制执行态动效）<br>2. 其余未实现字段（`general.* / output.* / timeout.* / error.* / advanced.log_ttl_days`）在 `config.h` 注释与 `节点编辑器使用说明.md` 中**明确标注「Patch C/D 接线」**，并在加载时写一条 `log::info("[配置] 以下字段尚未生效：…")`<br>3. `config.deepseek.*` 与节点参数的关系在文档中说明（节点参数为准，config 段仅作默认值来源） |
| 验证 | 改 `grid_size`/`running_animation` 后重启生效；日志出现"尚未生效"清单 |
| 产出 | `src/utils/config.*`、`src/ui/app.cpp`、`src/ui/node_canvas.*`、文档 |

#### PA-09 文档与索引同步

| 项目 | 内容 |
|---|---|
| 步骤 | 1. `docs/CHANGELOG.md` 新增 Patch A 条目（含实测数据）与「相关文档索引」行<br>2. `docs/actionPlan/milestone_plan.md`：新增「补丁系列」行 + 修正 M2/M3/M4 过期状态<br>3. `docs/actionPlan/M4.md`：M4-13「节点耗时明细」标注由 PA-04 完成；M4-05 标注「并入 Patch B」；M4-12 标注由 PA-05 + PD-07 承接<br>4. `docs/节点编辑器使用说明.md`：新增「输出面板 / 结果呈现 / Console 复制 / 错误条」章节与人工确认项<br>5. `source/README.md`：补充新面板与自检命令说明（如有新增） |
| 验证 | 文档互链可达；描述与代码一致 |
| 产出 | 上述 5 个文档 |

### 3.3 技术验证清单

| 编号 | 验证项 | 通过标准 |
|---|---|---|
| VA-01 | 结果可见且一致 **✅ 已自动化** | `--run-selftest` 断言「输出面板全文 ⊇ 每个节点全文」（离线 3 节点 / web 4 节点均 OK）；节点内摘要与参数面板结果区复用同一 `node_output_text`，人工确认见使用说明 §10.5 |
| VA-02 | 结果可用 **🟡 PA-02 部分** | 复制全文/该节点、导出到文件已实现（同一 `run_output_text` 实现）；按钮行为的**人工确认**见使用说明 §10.5 |
| VA-03 | 耗时正确 **✅ A1** | 每节点 ≥0；各节点耗时之和 ≤ 总耗时；失败/跳过节点也有记录（已由 `--exec-selftest` 6 项断言 + `workflow.log` 明细实测覆盖） |
| VA-04 | 错误条 | 失败出现；悬停显示全文；点击定位失败节点；可关闭 |
| VA-05 | Console 增强 | 复制/导出与过滤结果一致；自动滚动在手动上滚后暂停、点「回到底部」恢复 |
| VA-06 | 配置地基 **🟡 A1 部分** | `app_config()` set→get 一致（✅ A1 已断言）；`grid_size`/`running_animation` 生效与"尚未生效"日志待 PA-08 |
| VA-07 | **快照纯净** **✅ PA-02** | 运行后工作流 JSON 与撤销快照**不含**任何运行结果（`--run-selftest` 已断言：`VA-07 工作流 JSON 不含运行结果 = OK`，web/离线两模式均通过） |
| VA-08 | 无回归 **✅ A1** | §0.4 基线全绿（七组 PASS / 95-0 / **62-0** / `--run-selftest` PASS / `--run-selftest --web` 5/5）；构建 0 error / 0 warning |

### 3.4 风险与对策

| 风险 | 级别 | 对策 |
|---|---|---|
| 运行结果误入 `Graph`/快照，污染撤销栈与 JSON | 中 | 只读 API + VA-07 显式断言；代码评审时检查所有写入点 |
| 长文/多节点导致面板卡顿 | 低 | 摘要截断 + `ImGuiListClipper` + 按需复制；正文不参与每帧布局计算 |
| 导出路径不可写 | 低 | 复用 `utils::save_file()` 的失败提示与日志 |
| 与 Patch B（流式）接口返工 | 中 | PA-01 预留 `delta_bytes`；PA-02 按"分段"组织渲染，B 只做增量追加 |
| 新增面板影响既有停靠布局 | 低 | 默认隐藏，仅在开关为真时出现；「重置布局」仍可用 |
| 错误条与状态栏拥挤 | 低 | 单行省略 + 关闭按钮；错误条仅在失败后出现 |

### 3.5 产出物

| 产出 | 说明 |
|---|---|
| 执行器只读运行信息 | `NodeRunInfo` + `Executor::runInfos()` + 逐节点耗时入 `workflow.log` |
| 输出面板 | `ui/output_panel.{h,cpp}` + 视图菜单 + `ui.show_output_window` 接线 |
| 结果呈现 | 节点内摘要、参数面板「运行结果」区 |
| Console 增强 | 复制全部/过滤、导出、自动滚动 |
| 错误条 | `EditorState.last_error` + 状态栏错误条 + 点击定位 |
| 配置地基 | 进程级 `app_config()`；`grid_size`/`console_height`/`running_animation` 接线；未接字段清单化 |
| 文档 | CHANGELOG / 索引 / milestone_plan / M4.md / 使用说明 |

---

## §4 M_patchB —— 执行异步化 + 流式 + Provider 统一

> 前置：Patch A 完成（输出面板与 `runInfos` 已就位）
> 目标：界面不再卡死、停止真正生效、正文逐字出现、official 与 web 走同一抽象
> 工作量：全职 1.5–2 天；风险：**中**（动执行器核心）

### 4.1 任务清单

| 编号 | 任务 | 要点 | 产出 |
|---|---|---|---|
| PB-01 | 执行线程化 | `Executor` 在独立线程按节拍推进 `tick`；主线程只消费线程安全事件队列（状态/日志/增量/结束）；退出时 join 并保证 `Graph` 状态回写主线程 | `engine/executor.{h,cpp}`、`ui/editor_state.{h,cpp}` |
| PB-02 | 真取消 | `cancel()` 置位 + 唤醒；HTTP 层支持中断（客户端取消 / 读超时）；节点协作检查点（`ctx.is_cancelled()` 在重试与流式块之间检查） | `ai/*`、`engine/executor.cpp` |
| PB-03 | 流式回调接口 | `ExecutionContext` 增 `on_delta(node_id, text)`；`NodeRunInfo.delta_bytes` 生效；UI 侧按"段"增量追加（复用 PA-02/03 渲染） | `engine/executor.h`、`ui/output_panel.cpp` |
| PB-04 | Provider 统一抽象（设计 §8.1） | `ai/inference_provider.h`（`GenerateParams` / `ProviderResult` / `InferenceProvider{name,supports_vision,generate,generate_stream}`）+ `ai/provider_factory.{h,cpp}`；**把现有 `web_chat()` 收编为 `DeepSeekWebProvider`**（保持 `--web-chat` 行为不变） | `ai/inference_provider.h`、`ai/provider_factory.{h,cpp}`、`ai/deepseek_web_client.*` |
| PB-05 | 官方 Provider（原 M4-05 / M4-10） | `ai/deepseek_official_provider.{h,cpp}`：`POST {api_base}/chat/completions`；**参数透传** `system_prompt/temperature/max_tokens/top_p`；`stream=true` 时解析官方 SSE 增量；错误分类（401/402/429/超时/DNS/5xx）；重试读 `config.error.*`、超时读 `config.timeout.*`；纯函数 `build_request_body/build_endpoint` 供离线自检 | `ai/deepseek_official_provider.{h,cpp}`、`ai/sse_reader.{h,cpp}` |
| PB-06 | 凭据与网络策略（原 M4-07） | `utils/credential.{h,cpp}`（`CredWrite/CredRead/CredDelete`）；Key 优先级：节点参数 → 环境变量 → 凭据管理器（`api_key_ref`）；代理（`HTTPS_PROXY/HTTP_PROXY`）与自签证书开关（默认严格） | `utils/credential.{h,cpp}`、`ai/provider_factory.cpp` |
| PB-07 | 会话失效引导（§8.5） | 识别网页版 `40002/401` 与官方 `401`；状态栏/错误条出现「会话已失效，点此重新登录」；按钮直达登录窗口（WebView2） | `ai/deepseek_web_client.cpp`、`ui/editor_state.*`、`ui/error_bar` |
| PB-08 | 流式呈现 | 输出面板与节点摘要逐字增长；运行中显示"生成中…（N 字）"；结束后落定并结合 PA-01 耗时 | `ui/output_panel.cpp`、`ui/node_canvas.cpp` |
| PB-09 | 自检扩展 | `api_probe --selftest` 增 **V-11 无 Key 请求构造**、**V-12 取消/超时语义**；`--run-selftest --official`（有 Key 时真实生成）；`--run-selftest --web` 保持 5/5 | `tools/api_probe.cpp` |

### 4.2 技术验证清单

| 编号 | 验证项 | 通过标准 |
|---|---|---|
| VB-01 | UI 不卡 | 生成期间窗口可拖动/可交互，帧率稳定（诊断日志无长帧） |
| VB-02 | 取消生效 | 运行中点「■ 停止」在 1 秒内进入 Cancelled；HTTP 请求被中断（日志有取消记录） |
| VB-03 | 流式正确 | 增量拼接结果 == 非流式结果（同一提示词对比）；`delta_bytes` 与正文长度一致 |
| VB-04 | 参数透传 | 自检断言 body 内含 `system/temperature/max_tokens/top_p` 期望值（离线） |
| VB-05 | 凭据 | 三优先级正确（参数 > 环境变量 > 凭据管理器）；写入/读取/删除均成功；日志无明文 |
| VB-06 | 会话失效 | 伪造失效 Token → 出现「重新登录」入口且按钮可打开登录窗口 |
| VB-07 | 线程安全 | 运行中编辑图/切工作流 → 安全终止不崩溃；无数据竞争告警（自检多次运行） |
| VB-08 | 无回归 | §0.4 基线全绿（`--run-selftest --web` 仍 5/5） |
| VB-09 | 异常不崩 | 断网/超时/坏 JSON 均转为节点 error + 可读提示 |

### 4.3 风险与对策

| 风险 | 级别 | 对策 |
|---|---|---|
| 线程化引入数据竞争（Graph/UI 状态） | 高 | 所有 `Graph` 写入仅在主线程；工作线程只产出事件；必要时加 `assert(主线程)` |
| 取消语义不彻底（HTTP 阻塞在读） | 中 | 缩短读超时并分块读；取消时主动关闭 socket |
| 凭据管理器权限/域问题 | 低 | 失败回退到"输入框 + 说明"，不阻断主流程 |
| SSE 格式变化 | 中 | `sse_reader` 独立模块 + `raw_head` 诊断（沿用现有做法） |
| 与 M4 验收重叠导致文档混乱 | 低 | M4.md 明确标注"由 Patch B 完成"（见 §10） |

### 4.4 产出物

`inference_provider` 抽象、`DeepSeekOfficialProvider`、`DeepSeekWebProvider`、`provider_factory`、`sse_reader`、`utils/credential`、事件队列版 `Executor`、会话失效引导、V-11/V-12 自检。

---

## §5 M_patchC —— 数据安全

> 前置：Patch A（配置访问器 + 结果通道）
> 目标：崩溃不丢稿、输出可追溯、版本可迁移
> 工作量：全职 ≈1 天；风险：中低

### 5.1 任务清单

| 编号 | 任务 | 要点 | 产出 |
|---|---|---|---|
| PC-01 | autosave | 工作流变更后节流（≥3 秒且空闲）写 `~/.brain-ai/snapshots/autosave.json`；**原子写**（临时文件 + rename）；保留最近 N 份（`autosave_1..N`） | `ui/editor_state.{h,cpp}`、`utils/paths`（复用 `snapshots_dir()`） |
| PC-02 | 恢复流程 | 启动时若存在 autosave 且比用户最近打开的工作流新 → 弹出一次询问「恢复未保存的更改？」（可禁用该提示）；恢复后不覆盖原文件，另存需用户确认 | `ui/app.cpp`、`ui/editor_state.*` |
| PC-03 | 最近列表治理 | 打开前校验存在性；失效条目在菜单中灰显并标注"（文件不存在）"；提供「清理失效条目」 | `engine/recent_files.{h,cpp}`、`ui/app.cpp` |
| PC-04 | 版本迁移框架 | `workflow_io` 的 `version` 与 `config_version` 建立迁移表（当前只 warn）；未知高版本拒绝并给出提示；新增迁移自检 | `engine/workflow_io.cpp`、`utils/config.cpp` |
| PC-05 | 输出归档最小集 ✅（2026-09-24 提前落地） | 运行完成写 `outputs/<yyyyMMdd-HHmmss>-<工作流名>/`：每节点一个 `.txt`（含元信息头：节点/类型/状态/耗时/错误/统计）+ `run.json`（统计 + 逐节点明细）；同秒多次运行加序号，本次归档永不删除 | `utils/output_archive.{h,cpp}`（新）、`ui/editor_state.cpp`（运行结束钩子）、`ui/output_panel.cpp`（显示归档路径） |
| PC-06 | 归档 TTL 与上限 ✅（2026-09-24 提前落地；`auto_open_on_complete` 留 M5） | 接线 `config.output.{archive_dir, ttl_days, keep_history, max_history}`：`keep_history=false` 只留最近 1 份；`true` 保留 `max_history` 份；`ttl_days>0` 清理超期目录（只清理符合命名规则的目录，先统计后删除并写日志） | `utils/output_archive.cpp`、`utils/config.*`（注释更新） |

### 5.2 技术验证清单

| 编号 | 验证项 | 通过标准 |
|---|---|---|
| VC-01 | 原子写 | 模拟写入中断不产生半个 JSON（临时文件残留可被清理）；autosave 可被正常加载 |
| VC-02 | 恢复流程 | 构造"autosave 比工作流新"场景 → 出现询问；选择恢复后图内容与 autosave 一致 |
| VC-03 | 迁移正确 | 构造 v1.0 旧文件 → 能加载并写回当前版本；构造未来版本 → 明确拒绝并提示 |
| VC-04 | 归档一致 **✅ 已提前验证** | 归档文件内容与输出面板一致（同一 `node_output_text` 实现，`--exec-selftest` 断言元信息头 + 正文；`--run-selftest --web` 断言归档内含生成文档） |
| VC-05 | TTL 清理 **✅ 已提前验证** | 造 40 天前目录 + `ttl_days=30` → 只清超期且**本次归档保留**；`ttl_days=0` 不清理（`--exec-selftest` 断言） |
| VC-06 | 无回归 | §0.4 基线全绿 |

### 5.3 风险与对策

| 风险 | 级别 | 对策 |
|---|---|---|
| 误删用户输出 | 中 | TTL 清理先列出再删、写日志、默认 `ttl_days` 保守；归档目录可配置 |
| autosave 频繁写盘 | 低 | 节流 + 仅变更时写 + 原子写 |
| 恢复提示打扰 | 低 | 仅当 autosave 更新时才提示，且提供"不再提示" |

### 5.4 产出物

autosave 与恢复、最近列表治理、迁移框架、输出归档与 TTL、对应自检。

---

## §6 M_patchD —— 交互打磨与配置接线

> 前置：Patch B（异步执行）与 Patch C（数据安全）
> 目标：手感与效率；把 C 类配置全部接线并给出设置面板
> 工作量：全职 ≈1.5 天；风险：中（粘贴修复涉及画布坐标系）

### 6.1 任务清单

| 编号 | 任务 | 要点 | 产出 |
|---|---|---|---|
| PD-01 | 快捷键表（设计 §6.7） | `Ctrl+Z/Y` 撤销重做、`Ctrl+S/O/N` 保存/打开/新建、`Ctrl+C/V/X` 复制粘贴剪切（依赖 PD-02）、`Del` 删除选中、`F5` 运行、`Shift+F5` 停止、`Ctrl+L` 清 Console、`Ctrl+0` 重置视图；统一在 `app.cpp` 的 `handle_shortcuts()` 分发，避免与 ImGui/ne 冲突 | `ui/app.cpp`、`ui/toolbar.cpp` |
| PD-02 | 复制/粘贴重做 | 修根因：粘贴后先写入 `Graph` 位置、再在画布 `sync_positions()` 阶段统一落位；`NavigateToContent` 延后到下一帧；对坐标做有限性夹紧（沿用现有防护） | `ui/editor_state.{h,cpp}`、`ui/node_canvas.cpp` |
| PD-03 | 设置面板（M6-06） | 新增 `ui/settings_panel.{h,cpp}`：分区编辑 `general/ui/output/timeout/error/advanced`；含校验（范围/路径存在性）+ 落盘 + 「需重启生效」标注；`deepseek` 段说明"节点参数为准" | `ui/settings_panel.{h,cpp}`、`ui/app.cpp` |
| PD-04 | 窗口几何持久化 | `config.ui` 增 `window_width/height/pos_x/pos_y/maximized`；启动恢复 + 越屏矫正（多显示器拔插后回到主屏） | `utils/config.*`、`ui/app.cpp` |
| PD-05 | 参数面板增强 | 每节点「重置为默认」；按类型批量应用参数；参数搜索过滤 | `ui/property_panel.cpp` |
| PD-06 | 重跑能力 | 「重跑该节点」「重跑该节点及下游」（依赖 `runInfos` 与拓扑）；重跑前清除受影响节点的结果与状态 | `ui/editor_state.*`、`ui/node_canvas.cpp`、`ui/toolbar.cpp` |
| PD-07 | 输出面板进阶 | 历史运行列表（内存 + 最近 N 次）、节点内搜索、复制为 Markdown/JSON | `ui/output_panel.cpp` |
| PD-08 | i18n 决策 | 二选一：① 最小 i18n（字符串表 + `general.language` 切换，重启生效）；② 从 `config.toml` 暂时移除 `general.language/startup` 并记入 M6（本补丁默认 ②，除非另行指定） | `utils/config.*`、文档 |

### 6.2 技术验证清单

| 编号 | 验证项 | 通过标准 |
|---|---|---|
| VD-01 | 快捷键无冲突 | 画布操作/文本输入时快捷键不误触；`Ctrl+Z` 与参数编辑的撤销一致 |
| VD-02 | 粘贴正确 | 粘贴后节点落在期望位置、视图不漂移（坐标有限且合理）；撤销可回退 |
| VD-03 | 设置面板一致 | 面板↔`config.toml` 双向一致；非法值被拒绝并提示 |
| VD-04 | 窗口几何 | 拖动/最大化后重启恢复一致；拔掉显示器后不越屏 |
| VD-05 | 重跑语义 | 只重跑目标节点时上游结果复用；重跑下游时受影响节点结果刷新 |
| VD-06 | 输出进阶 | 历史列表可切换；搜索命中正确 |
| VD-07 | i18n 处理 | 按所选方案：切换生效（重启）或配置项已移除且文档记录 |
| VD-08 | 无回归 | §0.4 基线全绿 |

### 6.3 风险与对策

| 风险 | 级别 | 对策 |
|---|---|---|
| 粘贴坐标修复回归（历史事故） | 中 | 复现用例先写成自检（坐标有限性 + 位置一致），再改代码 |
| 快捷键与 ImGui 文本输入冲突 | 低 | `ImGui::GetIO().WantTextInput` 时跳过全局快捷键 |
| 设置面板范围膨胀 | 低 | 仅接现有 `Config` 字段，不新增语义 |
| 重跑与撤销栈交互 | 中 | 重跑只改运行态与节点状态（不进快照）；撤销语义保持不变 |

### 6.4 产出物

快捷键体系、复制/粘贴恢复、设置面板、窗口几何持久化、参数面板增强、重跑能力、输出面板进阶、i18n 处置结论。

---

## §7 并行小项（PM）—— 可与 A–D 任意阶段并行

| 编号 | 任务 | 要点 | 验证 | 产出 |
|---|---|---|---|---|
| PM-01 | WebView2 Runtime 检测与引导（清零 M1 唯一遗留，落地 M6-02） | 启动自检 WebView2 Runtime 版本；缺失时给出**官方 Evergreen Bootstrapper** 链接与说明；不阻塞主界面（仅网页版相关功能提示） | VM-01：模拟缺失（改 profile 路径/环境变量）→ 出现引导且可正常使用非网页版功能 | `web/webview_host.*`、`ui/app.cpp`、文档 |
| PM-02 | 文档索引与状态治理 | `docs/CHANGELOG.md` 索引含本文件；`milestone_plan.md` 增「补丁系列」行并修正 M2/M3/M4 过期状态；建立"每次提交同步状态行"的规则 | VM-02：文档互链可达、状态与代码一致 | `docs/CHANGELOG.md`、`docs/actionPlan/milestone_plan.md` |
| PM-03 | 构建脚本一键化 | `scripts/build.ps1`：配置（若需要）→ 构建 → 跑全部自检 → 输出摘要（失败给文件名+行号） | VM-03：干净环境一条命令完成并可读摘要 | `scripts/build.ps1`、`source/README.md` |
| PM-04 | 诊断信息完善（可选） | 关于/诊断区一键复制：版本、依赖版本、数据目录、日志路径、GPU、DPI 缩放 | VM-04：复制内容可用于问题反馈 | `ui/app.cpp` |

---

## §8 横向追溯矩阵（缺口 → 任务 → 验证 → 验收）

| 缺口（证据） | 任务 | 验证 | 验收 |
|---|---|---|---|
| `Executor::outputs()` 在 UI 零引用 | PA-01 / PA-02 / PA-03 | VA-01 / VA-02 | AA-01 |
| Console 无复制/导出（`console_panel.cpp` L14/16/17） | PA-05 | VA-05 | AA-02 |
| 无节点耗时（`executor.cpp` L254/261） | PA-04 | VA-03 | AA-03 |
| 无错误呈现（`app.cpp` 无 Toast） | PA-06 | VA-04 | AA-04 |
| 无进程级配置访问器（`app.cpp` L328） | PA-07 | VA-06 | AA-05 |
| 未接配置字段（`grid_size` 等） | PA-08（部分）+ PD-03（全量） | VA-06 / VD-03 | AA-05 / AD-04 |
| 同步阻塞 UI（`executor.h` L9-10） | PB-01 | VB-01 | AB-01 |
| `cancel()` 不中断当前节点 | PB-02 | VB-02 | AB-01 |
| 无流式回调（`ExecutionContext` L62-75） | PB-03 / PB-08 | VB-03 | AB-02 |
| official 未接线（M4-05 / M4-10） | PB-04 / PB-05 | VB-04 | AB-03 |
| `api_key_ref` 无实现（M4-07） | PB-06 | VB-05 | AB-04 |
| 会话失效无引导（§8.5） | PB-07 | VB-06 | AB-05 |
| 无 autosave（`snapshots_dir` 空置） | PC-01 / PC-02 | VC-01 / VC-02 | AC-01 |
| 最近列表不校验、version 不迁移 | PC-03 / PC-04 | VC-03 | AC-02 |
| 输出不归档、无 TTL | PC-05 / PC-06 | VC-04 / VC-05 | AC-03 |
| 无快捷键（`ed::EnableShortcuts(false)`） | PD-01 | VD-01 | AD-01 |
| 复制/粘贴暂停（三处禁用） | PD-02 | VD-02 | AD-02 |
| 窗口几何不持久化 | PD-04 | VD-04 | AD-03 |
| 设置面板缺（M6-06）、参数面板无重置 | PD-03 / PD-05 | VD-03 | AD-04 |
| 无重跑能力 | PD-06 | VD-05 | AD-05 |
| M6-02 WebView2 遗留 | PM-01 | VM-01 | AM-01 |

### 8.1 各补丁验收标准

| 补丁 | 编号 | 验收项 |
|---|---|---|
| A | AA-01 | 结果在节点/参数面板/输出面板三处一致可见 |
| A | AA-02 | 可一键复制或导出运行结果（UTF-8 正确） |
| A | AA-03 | `workflow.log` 含逐节点状态与耗时 |
| A | AA-04 | 失败出现可关闭错误条，点击可定位失败节点 |
| A | AA-05 | 配置访问器可用；未接字段不再"改了没反应"（或已明确标注） |
| B | AB-01 | 生成期间界面不卡；「■ 停止」1 秒内生效 |
| B | AB-02 | 正文流式逐字显示，且与非流式结果一致 |
| B | AB-03 | `mode=official` 可真实生成文本（需 API Key） |
| B | AB-04 | API Key 可存/读/删于凭据管理器（`api_key_ref` 生效） |
| B | AB-05 | 会话失效时给出「重新登录」入口并可用 |
| C | AC-01 | 崩溃后可恢复未保存更改 |
| C | AC-02 | 旧版本工作流/配置可迁移；未来版本明确拒绝 |
| C | AC-03 | 运行输出归档且按 TTL/上限清理 |
| D | AD-01 | 设计 §6.7 快捷键可用且无冲突 |
| D | AD-02 | 复制/粘贴可用且无坐标事故 |
| D | AD-03 | 窗口几何重启恢复且不越屏 |
| D | AD-04 | 设置面板双向一致，覆盖 C 类全部配置 |
| D | AD-05 | 支持重跑单节点/下游 |
| PM | AM-01 | 缺失 WebView2 Runtime 时给出引导且不影响其它功能 |

---

## §9 执行节奏（单补丁循环）

1. **确认**：对照本文件逐条确认任务边界（如涉及"待决问题"，先记结论）。
2. **实现**：小步提交到工作区，保持构建 0 error / 0 warning（MSVC `/W4`）。
3. **自检扩展**：新增断言先写"会失败"的用例，再改实现（尤其 VA-07 快照纯净、VD-02 粘贴坐标）。
4. **本地回归**：跑 §0.4 全表，记录数字。
5. **文档同步**：CHANGELOG（含实测数据）+ 本文件勾选状态 + 相关里程碑文档交叉引用。
6. **提交**：一个补丁一次提交；提交信息包含"改了什么 / 为什么 / 如何验证 / 实测数据"。
7. **人工取证**：把需要你在界面确认的项整理成清单（沿用 `节点编辑器使用说明.md` §8 风格），你确认后勾验收项。

---

## §10 与既有里程碑文档的关系

| 既有条目 | 处置 | 说明 |
|---|---|---|
| M4-05 官方 Provider | **并入 PB-04/PB-05** | M4.md 标注"由 M_patchB 完成"；不再单独排期 |
| M4-07 API Key 存储 | **并入 PB-06** | 落地 §8.4 凭据管理器 |
| M4-09 PoW | ✅ 已在 W2 完成（方案 A） | 保持现状；`ai/web_pow.cpp` 作为兜底 |
| M4-10 SSE 解析 | **并入 PB-05**（官方）+ W2 已有（网页版） | 抽 `sse_reader` 共用 |
| M4-11 流式输出 | **并入 PB-03/PB-08** | 依赖执行线程化 |
| M4-12 Console | PA-05（复制/导出/自动滚动）+ PD-07（进阶） | M4.md 相应标注 |
| M4-13 workflow.log | PA-04 完成"节点耗时明细"；归档路径由 PC-05 | M4 仅剩缓存命中（随 M5 缓存） |
| M5-02 VLM / ImagePreview / Output 窗口 | 不变（M5） | PA-02 的输出面板是 Output 窗口的雏形，M5 只做图片/多模态与历史增强 |
| M5 §11 输出归档 | PC-05/PC-06 打底 | M5 负责历史/预览/自动打开 |
| M6-02 引导 / M6-06 设置面板 / M6-08 打包 | PM-01 / PD-03 / 仍留 M6 | — |

---

## §11 附录

### 11.1 术语

| 术语 | 含义 |
|---|---|
| 文档值 | 存在 `Graph` 中、会进工作流 JSON 与撤销快照的值（节点参数、位置、视图） |
| 运行态值 | 只存在于 `Executor`（`outputs()` / `runInfos()`）的执行产物，**永不**进文档 |
| 只读暴露 | UI 仅读取运行态值的接口；写入必须走 `EditorState` 快照事务 |
| 增量段 | 流式输出按节点组织的可追加文本段（Patch B 复用 Patch A 的渲染结构） |

### 11.2 自检命令速查

| 命令 | 用途 | 现状 / 目标 |
|---|---|---|
| `api_probe.exe --selftest` | V-01…V-10 + 新增 V-11（无 Key 请求构造）/ V-12（取消与超时） | 七组 PASS → 九组 PASS |
| `api_probe.exe --graph-selftest` | 图模型/注册表/撤销/序列化 | 95/0 → 随补丁增长 |
| `api_probe.exe --exec-selftest` | 拓扑 + 校验 + 执行器 | 56/0 → 随补丁增长 |
| `aiwrite.exe --run-selftest` | 示例工作流（official 占位） | PASS（3/5 预期） |
| `aiwrite.exe --run-selftest --web` | 示例工作流真实网页版生成 | PASS（5/5） |
| `aiwrite.exe --run-selftest --official` | 官方 API 真实生成（需 Key，PB-09 新增） | 待 PB |
| `aiwrite.exe --web-probe` / `--web-chat "<提示词>"` | 网页版协议探测 / 端到端生成 | PASS |

### 11.3 文件-职责索引（本系列新增/改动）

| 路径 | 职责 | 补丁 |
|---|---|---|
| `engine/executor.{h,cpp}` | `NodeRunInfo` / `runInfos()` / 逐节点耗时 / 线程化 / 流式回调 | A、B |
| `ui/output_panel.{h,cpp}`（新） | 输出面板（复制/导出/历史/搜索） | A、B、D |
| `ui/error_bar`（新，或并入 app.cpp） | 错误条与定位 | A、B |
| `ui/settings_panel.{h,cpp}`（新） | 设置面板（M6-06） | D |
| `ui/console_panel.cpp` | 复制/导出/自动滚动 | A |
| `utils/config.{h,cpp}` | 进程级配置访问器、迁移、窗口几何字段 | A、C、D |
| `ai/inference_provider.h`（新） | Provider 抽象（§8.1） | B |
| `ai/deepseek_official_provider.{h,cpp}`（新） | 官方 API（原 M4-05/M4-10） | B |
| `ai/deepseek_web_client.{h,cpp}` | 收编为 `DeepSeekWebProvider` | B |
| `ai/provider_factory.{h,cpp}`（新） | 按 mode 取 provider + Key/网络策略 | B |
| `ai/sse_reader.{h,cpp}`（新） | 官方与网页版共用的 SSE 增量解析 | B |
| `utils/credential.{h,cpp}`（新） | 凭据管理器（原 M4-07） | B |
| `utils/output_archive.{h,cpp}`（新，或并入 paths） | 输出归档与 TTL | C |
| `scripts/build.ps1`（新） | 一键构建 + 自检 | PM |

### 11.4 变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-09-23 | v1 | 首次建立：基于全库底层逻辑审计，确定补丁系列 A→D + 并行项，并详述 Patch A |

### 11.5 待决问题（见 §12 决策点）

---

## §12 决策点（开工前请确认）

| 编号 | 议题 | 选项 | 当前默认 |
|---|---|---|---|
| D-01 | 结果回显形态 | ① 节点摘要 + 输出面板（推荐）② 仅节点内 | ① |
| D-02 | 是否接受 Patch B 动执行器核心（线程化 + 真取消） | ① 接受 ② 用"分段 tick + 读超时"折中，暂不线程化 | ① |
| D-03 | 未接配置字段 | ① 现在就接线（A 接三个、其余 D 接）② 先标注"未生效" | ①（A-08 按此执行） |
| D-04 | i18n | ① 最小实现 ② 从 config 移除并记入 M6 | ② |
| D-05 | 补丁粒度 | ① A 一次提交 ② A 拆两次（A1 数据通道+日志、A2 界面呈现） | ②（便于逐步验证） |

---

**下游索引**：里程碑总览见 [`milestone_plan.md`](milestone_plan.md)；变更明细见 [`../CHANGELOG.md`](../CHANGELOG.md)；
网页版协议与 PoW 方案见 [`../网页版协议实测记录.md`](../网页版协议实测记录.md)。
