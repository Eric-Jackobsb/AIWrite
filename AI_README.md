# AI_README —— AI / 协作者行为契约（AIwrite）

> 本文是**给 AI 助手与新协作者的行为契约**：只写「读取顺序 + 命令 + 纪律 + 禁止项」，技术细节一律指向权威文档。
> 接手任何改动前，请按 §1 顺序读文档，并遵守 §3 取证纪律、§4 改动伴随义务、§6 提交纪律、§7 禁止项、§9 表达规范（写给人类的文字）。
> **本文 §2 的数字 = 最近一次全量实测（2026-10-10 本机）**；各任务**落地当时**的验收数字以对应计划文档为准（例：[docs/actionPlan/M_patchC_web_node_bind.md](docs/Archive/actionPlan/M_patchC_web_node_bind.md) §7.1 的历史行**不追溯改写**）。

---

## 1. 读取顺序（按序读 5 处）

1. **[docs/DocMAP.md](docs/DocMAP.md)** —— 文档地图：什么改动读哪份文档。
   > 更名记录：`docs/README.md` → `docs/DocMAP.md`（2026-10-08 · `git mv` 保留历史）。历史条目（含 `docs/Archive/**`）里出现的**旧文件名与旧自链写法**均指本文。
2. **[docs/actionPlan/](docs/actionPlan)** 中**现行**的那一份（`M6.md` / `M8.md` / `M_polish.md` / `M_debt.md` / `milestone_plan.md`；**其余均已归档（2026-10-23 全部收口）** → [docs/Archive/actionPlan/](docs/Archive/actionPlan)：`M4` / `M5` / `M7`（2026-10-11）· `M_code_cleanup` / `M_patchAB_rest` / `M_patchC_web_node_bind` / `M_canvas_snapshot` / `M_8nodes_extension` / `M_node_expansion` / `M_node_expansion_part2`（2026-10-23 · `ARC-AR-02`））—— 不变量 `Ixx`、决策 `D-xx`、计划 `PW-xx`/`M*-xx`、§7 验收标准都在这。**节点扩展定稿 = [M_node_expansion_part2.md](docs/Archive/actionPlan/M_node_expansion_part2.md)**（四层定稿 + L1/L2/L3 实施完成；L1 实施依据 = [M_8nodes_extension.md](docs/Archive/actionPlan/M_8nodes_extension.md)）；**残项统一承接位 = [docs/actionPlan/M_polish.md](docs/actionPlan/M_polish.md)**（四阶段打磨 · §4 承接表）。
3. **[docs/CHANGELOG.md](docs/CHANGELOG.md)** 最近条目 —— 最后一次改了什么、上一轮验证数字是多少。
4. 搜不到代码里的名字（类 / 函数 / 配置键） ⇒ 先查 **[docs/变量术语对照表.md](docs/变量术语对照表.md)**（§12 已知文档≠代码 · §13 新增功能检索入口）。
5. **[docs/AI_Status.md](docs/AI_Status.md)** —— **一页状态（总览）**：门禁基线 + 台账汇总 + 循环轮次 + 下一步（接手 / 每轮开始与收尾时先扫一眼；§二 由 `loop_verify.ps1` 回写，勿手改）；**分通道明细** = [docs/AI_Status_web.md](docs/AI_Status_web.md)（网页版 11 站）/ [docs/AI_Status_api.md](docs/AI_Status_api.md)（官方 API 9 条）。

> **做「网页站逐站转可用」或自动化循环时**：先读 **[docs/loop_dev.md](docs/loop_dev.md)**（循环开发规则：启动闸 §1 · 三层结构 §2 · 站级 8 步 §3 · 轮次门禁 §4 · 失败分类 §5 · 退出码 §6 · 记录位 §7 · 停止条件 §8）。
> ⛔ **未获开发者「开始循环开发」的明确授权时，不得启动自动循环**（见 §6 授权表 · §7 禁止项第 8 条）。

**动手前先自问并写下答案**：这属于哪个里程碑/补丁？对应计划里的哪一条？验收标准是什么？需不需要真机验证？

---

## 2. 命令表（都在仓库根执行；产物在 `build\bin\`）

| 目的 | 命令 | 基线（**2026-10-11 本机实测** · 每个数字的唯一权威位见 §10.1） |
|---|---|---|
| 构建（Debug） | `cd source; .\build.ps1`（= `cmake --preset default` + `cmake --build --preset debug`）｜**日志自动落 `build\logs\build_debug_<yyyyMMdd_HHmmss>.log`**（`-LogName` 改名 / `-NoLog` 关闭） | **0 error / 0 warning**。⚠ 若报 `error MSB4018` + `FileTracker`（`GetOutOfDateItems` / `SetModuleDependencies`）⇒ **不是代码问题**、**重跑无效**：`.\build.ps1 -Reconfigure` 清空重建（陷阱 9 = [docs/loop_dev.md](docs/loop_dev.md) §10.1） |
| 工具 / 契约自检（总闸） | `api_probe.exe --selftest` | 末行 `=== 结果: … PASS ===`（**十三组**）· exit **0**；含 配置表 **52/0** · 图片准备 **19/0** · 上传证据 **32/0** · 站点上传模板 **21/0** · **DOM 探测 27/0** · **执行器 713/0**（**2026-10-21 复测** · 原 **546/0** = 2026-10-11 实测；同日链：2026-10-10 = 464 → **+26 = `M_patchC` 批 1**（`PW-02` / `PW-04` / `PW-05` / `PW-08`）→ **+34 = `M_patchAB` 批 2**（会话诊断）→ **+20 = 批 4**（版本迁移）→ **+2 = 批 6**（配置表嵌套 schema）→ **+26 = 2026-10-16 批**（`PD-06①` 通用重跑 / `PW-10` 模板占位符报错 / `PW-11` 归档保底）→ **+1 = 2026-10-19 CN 回归补强批** → **+4 = 2026-10-21 债务批 A-2**（`DEBT-L7` 假增量 · 682→686）→ **+5 = 债务批 A-4**（`DEBT-L8` 失败汇总 · 686→691）→ **+15 = `POL-L-01` 创作链四段模板补强**（691→706 · 2026-10-21）→ **+7 = `POL-C-02` 归档 TTL/保留边界补强**（706→713 · 2026-10-21）；**组数：七组 → 九组** = 批 3（V-11 / V-12）· **九组 → 十二组** = 批 6（V-13 / V-14 / V-15）· **十二组 → 十三组** = 批 7（V-16）） |
| 执行器自检 | `api_probe.exe --exec-selftest` | **713 / 0** · exit 0（**2026-10-21 复测** · 原 **546 / 0** = 2026-10-11 实测；同日链 **464 → 490**（`M_patchC` 批 1）**→ 524**（批 2）**→ 544**（批 4）**→ 546**（批 6）**→ 655**（L2/L3 + 收口批）**→ 681**（`PD-06①` 通用重跑 / `PW-10` 模板占位符报错 / `PW-11` 归档保底 · 2026-10-16）**→ 682**（**2026-10-19 CN 双源回归补强批** +1）**→ 686**（**债务批 A-2** `DEBT-L7` 假增量断言 +4 · 2026-10-21）**→ 691**（**债务批 A-4** `DEBT-L8` 失败汇总 +5）**→ 706**（**`POL-L-01` 创作链四段模板补强 +15** · 2026-10-21）**→ 713**（**`POL-C-02` 归档 TTL/保留边界补强 +7** · 2026-10-21）；同日 **`Composition_node` 实施批** 新增「上级节点自检」子段 **51/0** 并入 `--exec-selftest`（汇总仍 **681/0** —— 数字持平因自检分组归属调整，**非断言回退** · 构建 0/0 · graph **151/0** · 断链 0）；**2026-10-19 复测**：上级节点自检子段 **66/0**（**51 + 15** = 双源 / 空草稿实例化回归断言 · 汇总 **682/0**）：窗口表 / 复位计划 / 归属准入 / 三类归因文案 / 版本迁移 / L2 工厂与回环 —— 见 [CHANGELOG](docs/CHANGELOG.md) `[Unreleased]`）；**executor 子段同日链：83→94**（`POL-C-01` 校验边界补强 7 组 8 断言 + 3 次 `add_node` 内部断言 · 合计 +11 · 2026-10-21；`POL-C-02` 归档 TTL 补强落在 execution 段 ⇒ 子段保持 94） |
| 图模型自检 | `api_probe.exe --graph-selftest` | **151 / 0** · exit 0（**2026-10-15 复测** · 原 **110 / 0** = 2026-10-11 · L1 批升至 **144 / 0** → L2 批 **151 / 0**） |
| 上传链路自检（离线三组；真机加 `--provider <id> --image <图>`） | `aiwrite.exe --upload-selftest` | **72 / 0** · exit 0（⚠️ 见 §3「GUI 子系统 stdout」；**不要并发跑**，共用 `%TEMP%\aiwrite-upload\selftest`） |
| 运行时（真实工作流） | `aiwrite.exe --run-selftest` / `--run-selftest --web` | **PASS · exit 0**（**2026-10-09 复跑**：完成 3/5、失败 1、跳过 1 —— official 占位分支缺 Key 属预期；`official` 需 `~/.brain-ai/config.toml` 凭据；`--web` 需已登录一次） |
| 画布性能（**GUI** · 读日志判据） | `Start-Process .\build\bin\aiwrite.exe -ArgumentList '--open','<工作流>'` ⇒ 读 `%USERPROFILE%\.brain-ai\logs\app.log` 的 `[诊断] 画布` 行 | 85 节点基准图（**2026-10-09 实测** · 该图 2026-10-10 去重后为 **82 节点**）：`FPS 59~60` · `解码 0.0 次/帧` · `缓存 9/64 条 30.9MB`；修复前同图为 `FPS 5~6` · `解码 9.0 次/帧` |
| 网页登录自检 | `webview2_login.exe --selftest --timeout 30` | 退出码 **0** = PASS / 1 = FAIL / 2 = 超时 |
| 选择器诊断（**只读**） | `aiwrite.exe --web-adapter-selftest --provider <id>` · `--web-dom-dump --provider <id>` | 打印命中数 / 候选与建议选择器 |
| 网页站批量探测 + 报告 + 上轮对比（`FEA-L01` · **只读**） | `aiwrite.exe --web-adapter-selftest --provider all --timeout 30 --json <报告路径> [--json-diff <上一轮报告>]` | 10 站一次跑完 · **退出码 0 / 1 / 2 / 3**（2026-10-09 实测：可用 8 · 需登录 2 · 失败 1 · exit 1） |
| 会话 / 登录态清单（`FEA-L02`） | `aiwrite.exe --web-session-list [--json <路径>]` | 内存会话 / L2 快照元数据 / 11 条网页版条目判据 · **exit 0**（有没有会话都算成功） |
| **循环「一键一轮」（`TST-L01` · `loop_dev.md` §4/§6.1）** | `powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 [-Round R2] [-Providers kimi-web,doubao-web] [-SkipGates] [-SkipProbe] [-StatusOnly] [-UpdateDocs:$false]` | 门禁 → S2 探测 → S3 逐站 → 报告（**四块**）+ 台账（`TST-L05`）落 `build\loop\<轮次>-<时间戳>\`（`report.md` **四块** + `report.json`）；**站点台账（`TST-L05`）**：**20 站**（web 11 / official 9）×「文字理解 / 图片理解」两列跨轮累积 ⇒ `build\loop\SITE_STATUS.json` + 回写 `docs\loop_dev.md` §2.2 哨兵块（`-StatusOnly` 零副作用重建）；**站点绿判据 = 三口径：`rc=0` 且 `n3(LLMGenerate)=done` 且归档正文非站侧错误/忙碌串**（`n3` 取磁盘 PC-05 归档 ⇒ stdout 丢失也能判）；轮次退出码：有真失败或「未测」⇒ **1** · 需登录 ⇒ **3** · 全绿 ⇒ **0**（2026-10-09 R1 实测：S3 6 站 · 可用 5 · 失败 1 ⇒ 1；`SMOKE8/9` 实测：`kimi-web` 站侧繁忙 ⇒ **失败** · 1） |
| 夹具（抽真实源码脚本，迷你 DOM 真跑） | `node source\tools\poll_answer_gate_test.js` · `send_chain_test.js` · **`dom_gate_test.js`** | **44 / 0** · **17 / 0** · **15 / 0** |
| 夹具**负向**打假（必须 FAIL） | 上三条加 `--old` | 门夹具 **18 / 26** · 链路 **13 / 4** · **DOM 判据 9 / 6** · **exit 1**（失败项 = 预期） |
| 日志 | `%USERPROFILE%\.brain-ai\logs\app.log`（每 2 秒落盘，可直接 tail） | `[诊断]` 行限流输出 |
| **一键自检（`PM-03`）** | `powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_all.ps1 [-SkipBuild] [-SkipGui] [-SkipLoop]` | 构建 + **9 项 `api_probe` 自检** + **`--run-selftest`** + **`ce_repro`** + **3 个 node 夹具** + `--upload-selftest` + 台账重建 + **文档断链** + **依赖单向** + **全局变量** + **打包 payload** ⇒ **21/21 PASS · exit 0**（**2026-10-24 实测** · 物证 `build/logs/check_all_20261024_095356.log`；原 **2026-10-11 实测** · 物证 `build\logs\check_all_<时间戳>.log`；12 → 13 = 新增 `--site-assert`，13 → 14 = 新增 `--autosave-assert`，**14 → 15 = 批 5 新增第 ⑥ 项文档断链**（`source/tools/check_links.py` 入库）；**15 → 16 = 批 ③（2026-10-17）新增第 ⑦ 项打包 payload**（`source/tools/package.ps1` 入闸）；**16 → 17 = 自定义 AI 收口（2026-10-21）新增 `--official-assert`**） |
| **诊断 / 依赖（`PM-01`·`PM-04`）** | `api_probe.exe --diag-dump` ｜ `--diag-assert` ｜ `--dep-check` ｜ `--settings-assert` | 前两者 = 诊断报告（**8 分节 40 行**）打印/断言；`--dep-check` = `WebView2 运行时 152.0.4191.66（api）`（**2026-10-11 实测**）；`--settings-assert` = 默认通过 + 越界 **15 例全拦** + 边界值 **3 项**通过（`[network]` / `[credentials]` 两节新增越界例）。均为**单行 PASS/FAIL**（不进 §2 分组数字） |
| **自建站点闭环（`PB2-24` · `RM-D4`）** | `api_probe.exe --site-assert` | **PASS · exit 0**（**2026-10-24 实测**：纯函数 **50 / 0** + 真实写盘 → 重载 → 进表 / 进网页版下拉 / 进 provider 枚举 **20→21** / 新建节点运行前校验 / **旧实例刷新回归** / 清理零残留）。⚠ 会**临时**写并删除 `~/.brain-ai/providers.d/aiwrite-selftest-site.json`（自清理）；**界面点选回路未人工实测**，见 [CHANGELOG](docs/CHANGELOG.md) `[Unreleased]` |
| **自定义 AI 收口（冻结前最后一个功能 · 2026-10-21）** | `api_probe.exe --official-assert` | **PASS · exit 0**（纯函数 **40 / 0** + 真实写盘 → 重载 → 进表 / 进 provider 枚举 **20→21** / 新建节点运行前校验 / **旧实例刷新回归** / 清理零残留 · 设置面板「新建 API 提供商…」模态）。⚠ 会**临时**写并删除 `~/.brain-ai/providers.d/aiwrite-selftest-official.json`（自清理）；✅ **界面点选回路已人工验收（2026-10-21 用户真机点选 · 「新建 API 提供商…」创建→重载→下拉立即出现）**，见 [CHANGELOG](docs/CHANGELOG.md) `[Unreleased]` |
| **自动保存 / 崩溃恢复（`PC-01` / `PC-02`）** | `api_probe.exe --autosave-assert` | **PASS · exit 0**（**2026-10-21 复测**：场景 **32 / 0** · 原 **28 / 0** = 2026-10-10 实测；含 **`VC-01` 原子写**（半截 `.tmp` 不参与恢复 / 可清理）+ **`VC-02` 恢复判定 4 场景** + 恢复后内容**指纹逐字相同** + **`POL-C-02` 原子写失败路径** 4 条（rename 目标被目录顶替 ⇒ 返回 false + 有错误信息 / 失败后 `.tmp` 自清理 / 原快照位置不动 / 清理后重写恢复））。⚠ 场景全在**系统临时目录**内构造，**不碰**用户快照；**GUI「改图 → 静默 3 秒 → 写快照」与点「恢复」未人工实测**，见 [CHANGELOG](docs/CHANGELOG.md) `[Unreleased]` |
| **安装包（`M6-08`）** | `powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\package.ps1 [-Version 0.2.0] [-SkipBuild] [-SkipInstaller]` | ①构建 ②payload `build\dist\payload`（**18 文件 / 28.7 MB** · **Python 摘钩** `*.py=0` · `.iss ↔ payload` 一致性锁 PASS）③`ISCC` 编译。exit **0 / 2（未装 ISCC：已打印获取方式）/ 1**。⚠ **本机未编译验证**（无 ISCC + winget 无网络 `0x80072efd`）⇒ 见 [CHANGELOG](docs/CHANGELOG.md) `[Unreleased]` |
| VS Code | `Ctrl+Shift+B` 构建 · `F5` 调试 · Run Task：`build` / `build (release)` / `verify: api_probe 自检` / `verify: 图模型自检` / `log: 查看 app.log` / **`log: 查看最新构建日志`** | — |

> 完整开关清单（含 `--image-decode` / `--sha3` / `--pydoll-*` / `--login-selftest` 等）见 [source/README.md](source/README.md) §4。
> 数字变动时：先在 `CHANGELOG` 记本轮实测 + 同步相关计划行的验收数字，**再**回来改本文这一栏（§4 义务 6）。

---

## 3. 取证纪律（硬约束 · 踩坑总结）

- **输出一律重定向**：`& .\build\bin\api_probe.exe --selftest > $env:TEMP\x.txt 2>&1` 再 `Get-Content $env:TEMP\x.txt` 读回。
  *为什么*：本机 VS Code 的 shell 集成会**吞掉管道的输出**，`| Select-String` / `| Tee-Object` 会得到空结果 ⇒ 曾导致三套 selftest 被误判为「失败」。
- **构建日志有固定去处**：`source\build.ps1` 自动把**整段输出**写 `build\logs\build_<config>_<时间戳>.log`（UTF-8 无 BOM）⇒ **不要**再往仓库根写 `build_xxx.log`；自建的取证输出（selftest / 探针 / 性能复现）也请落到 `build\logs\`（例：`> build\logs\selftest.txt 2>&1`）或 `$env:TEMP`。快速取最新一份：`Get-ChildItem build\logs\*.log | Sort-Object LastWriteTime | Select-Object -Last 1`（VS Code 任务 `log: 查看最新构建日志`）。
  *踩坑留痕*：PS 5.1 的 `Tee-Object` **无 `-Encoding`**，默认写 **UTF-16LE** ⇒ 用 `| Tee-Object -FilePath build\logs\x.log` 落盘会得到 UTF-16（并混排乱码）；脚本内改用 `StreamWriter`（UTF-8 无 BOM）。
- **两种进程，两种捕获方式**：`api_probe.exe` / `node` 是**控制台程序** ⇒ `> file 2>&1` 有效；**`aiwrite.exe` 是 GUI 子系统** ⇒ `>` 捕**不到** stdout（实测 0 行，极易误判「没输出」）⇒ 必须用
  `Start-Process -FilePath .\build\bin\aiwrite.exe -ArgumentList '--upload-selftest' -NoNewWindow -Wait -PassThru -RedirectStandardOutput $env:TEMP\a.txt` 再 `Get-Content $env:TEMP\a.txt`（读 `$p.ExitCode` 取退出码）。
- **不凭旧记录下结论**：数字必须来自本轮 txt 物证（含退出码）；PASS 行的**文案**也要对（应含本轮新措辞 ⇒ 证明跑的是新二进制）。
- **断言失败要打印逐例明细**：例：`VB2-22⑧` 失败时打印 `crlf_got=[…] blank_got=[…] …`（转义 `\n`/`\r`/`\t`）—— 没有明细的断言不许合入。
- **静态锁必须能被反向打假**：夹具提供 `--old` 负向注入（把新写法换回旧写法）⇒ **必须如实 FAIL 且 exit 1**，否则该锁无意义。
- **口径变更四处同步**：代码 + 注释 + 离线断言 + 文档；并在文档里写清「为什么换」（例：A1 → A2）。
- **文档≠代码时以代码为准**，并当场修文档（不得留「已知文档≠代码」的新条目）。

---

## 4. 改动伴随义务（每次改代码都要做）

1. **[docs/CHANGELOG.md](docs/CHANGELOG.md)** `[Unreleased]` 追加条目：根因 / 改法 / 离线锁 / **验证（实测数字）** / 涉及文件。（**不要**改动历史条目与版本号小节 —— 见 §6）
2. **对应计划文档**：`docs/actionPlan/M*.md` 的 §6 计划行状态 + §7 验收数字 + §10「vN」演进行；涉及真机 ⇒ `docs/actionPlan/M8.md` §9.6 复盘行。
3. **「未做项 / 待真机项」如实登记**，含「**默认不做 + 理由**」（例：`M_patchC` §4.5 的 A2/A1 分叉）。
4. **新增文件 / 新命令** ⇒ 同步 [source/README.md](source/README.md) §4 与 [docs/DocMAP.md](docs/DocMAP.md) 文档地图。
5. **新增术语 / 命名** ⇒ 同步 [docs/变量术语对照表.md](docs/变量术语对照表.md)。
6. **本文的命令或基线数字变了** ⇒ 同步本文（口径：数字带**实测日期**；历史任务的数字留在原计划文档里，不追溯改写）。

---

## 5. 编号与登记约定

- **编号空间**：`Ixx`（不变量）/ `D-xx` / `W-Dx`（修正）/ `PW-xx`（补丁 C 任务）/ `M*-xx`（里程碑任务）/ `B*-xx`（批次）/ `AB*-xx`（验收）/ `VB*-xx` / `VB2-xx` / `A*-xx`（验证）/ `PB2-xx` / `R*-xx`（风险）/ `RM-D*`（路线决策）/ **`CS-xx`**（画布快照任务 · [docs/actionPlan/M_canvas_snapshot.md](docs/Archive/actionPlan/M_canvas_snapshot.md)）/ **`VC-xx`**（画布快照验证项）/ **`AC-xx`**（画布快照验收项）· **`NE-Qx`**（**节点扩展**待讨论问题）/ **`NE-Dx`**（**节点扩展**决议 —— 见 [docs/actionPlan/M_node_expansion.md](docs/Archive/actionPlan/M_node_expansion.md)；任务号 **`NE-01…`** 于**拍板后**启用）/ **`L1-*`**（**节点扩展 L1 深化**任务号）/ **`VL1-*`**（L1 离线验证项）/ **`AL1-*`**（L1 人工验收项）—— 三项见 [docs/actionPlan/M_8nodes_extension.md](docs/Archive/actionPlan/M_8nodes_extension.md)（🆕 2026-10-12 批 9-0 登记）· **`L1-R1`~`L1-R3`**（**L1 收尾决议** —— ⚠️ **不是**任务号 `L1-*`）/ **`L2-1`~`L2-5`**（L2 定向议题）/ **`L3-1`~`L3-4`**（L3 定向议题）/ **`G-1`~`G-5`**（全局口径）—— 四项见 [docs/actionPlan/M_node_expansion.md](docs/Archive/actionPlan/M_node_expansion.md) §7 / §11（🆕 2026-10-12 批 9-1 登记）· **`L2-01…`** / **`L3-01…`**（**L2 / L3 任务号** —— 🆕 2026-10-12 **批 9-2 先立** · `NE-D39`；⚠️ 与上列 `L2-1`~`L2-5` / `L3-1`~`L3-4`（**定向议题号**）**同形不同物**）· **`CR-*`**（**创作流程**阶段）/ **`CR-OPS-*`**（原子操作）/ **`CR-GAP-*`**（缺口）—— 三项见 [docs/actionPlan/Creation_Roadmap_Guideline.md](docs/actionPlan/Creation_Roadmap_Guideline.md)（🆕 2026-10-12 批 9-3 登记 · 同批归位 `docs/actionPlan/`）· **`CN-*`**（**上级节点**任务）/ **`CN-D*`**（**上级节点**决议）/ **`CN-Q*`**（**上级节点**待确认）—— 见 [docs/Archive/actionPlan/Composition_node.md](docs/Archive/actionPlan/Composition_node.md)（🆕 2026-10-16 批 CN-0 登记 · ⚠️ **不占用 M1–M8 编号**；任务号 `CN-01…` 已启用（CN 实施批 2026-10-16 · **实施完成** · 📦 **2026-10-18 归档** · ⏳ **人工验收 + 待拍板项 → [docs/DevPlan.todo](docs/DevPlan.todo) Test**）· `CN-D20` = **L4 重启**，覆盖 `M_node_expansion`「L4 不做」）· **`CLN-xx`**（**代码库清理**任务）/ **`CLN-Vx`**（清理后离线验证）/ **`CLN-Ax`**（清理后人工验收）—— 见 [docs/actionPlan/M_code_cleanup.md](docs/Archive/actionPlan/M_code_cleanup.md)（🆕 2026-10-19 批 CLN-0 登记 · ⚠️ **不占用 M1–M8 编号**；任务号 `CLN-01…` 已启用 · 审查/勾账全部完成（2026-10-20）· 已拍板：§3.4 拆分（CLN-12~15）落地 + `CLN-06` 扩项驼峰化执行 · 匿名命名空间专项放弃）· **`POL-L*`** / **`POL-UX*`** / **`POL-C*`** / **`POL-E*`**（**打磨与优化**四阶段任务：逻辑 / 操作体验 / 正确性 / 便捷性）/ **`POL-V*`**（验证）/ **`POL-A*`**（验收）—— 见 [docs/actionPlan/M_polish.md](docs/actionPlan/M_polish.md)（🆕 2026-10-20 批 POL-0 登记 · ⚠️ **不占用 M1–M8 编号**）。⚠️ **两处「同形不同物」警示**（引用前先看）：① **`NE-D*` 分两段** —— 上游 `M_node_expansion.md` §7 的 `NE-D1`~`NE-D7` + `NE-D15`~`NE-D29` = **全局/四层决策**，其 `NE-D30`~`NE-D37` = **L1 细节别名**；姊妹文档 `M_8nodes_extension.md` §2 的 `NE-D1`~`NE-D13` = **L1 细节** ⇒ **引用 L1 细节一律引姊妹文档的号**；② **`L1`/`L2`/`L3` 既是 PatchB 的层名**（配置表 / 接线 / 站点适配）**又是节点扩展的层名**（深化 / 新节点 / 库·画布 UX）。
- **状态词白名单**（只用这几个，别自创）：**已拍板 / 已落地 / 部分落地 / 计划中 / 已归档 / 已退场 / 待真机**。
- **文档三态标记**（全库统一 · 定义见 [docs/DocMAP.md](docs/DocMAP.md) §五）：✅ **已固定**（已落地视为契约）/ 📦 **已失效**（⛔ **只标记不删除**，必须给「现行指向」）/ ⬜ **待定**（未拍板 ⇒ **必须**在 `DevPlan.todo` 的「**待定 · 未拍板**」分类里有同名 `PQ-xx` 项；拍板后同批置 done）。⚠️ **2026-10-13 批 9-4**：该分类为**空落点分支**（只承载**新** ⬜ 项）；**已拍板历史 20 项**已整组移入 `Archive > ARC-ORG-01`。
- 新事项先**登记**（计划文档 + `DevPlan.todo`），再开工；收口时回填**实测数字**，不写「应该没问题」。
- 归档动作遵循 [docs/Archive/README.md](docs/Archive/README.md) §三（`git mv` + 顶部 📦 + 索引 7 处同步 + 相对链接层级修正）。

---

## 6. 提交与版本纪律 —— ⛔ 禁止擅自提交

**默认行为：改完即停。** 做完改动后，**只汇报**（改了什么 / 验证结果 / 待办），**然后停下等指令**。

**只有开发者明确说出下列指令时才行动**：

| 开发者明确说了 | 才可以做 |
|---|---|
| 「提交」/「commit」/「存档」 | `git add` + `git commit`（只提交本任务相关文件） |
| 「改版本」/「升版本」/「发版」 | 改版本号 + `CHANGELOG` 版本小节 + `tag` |
| 「建分支 / 切分支 / 合并 / 推」 | 对应分支或远程操作 |
| 「回滚 / 撤销」 | `revert` / `reset` / `checkout`（**先确认范围与后果**） |
| 「开始循环开发」（或同义明确指令） | 启动 [docs/loop_dev.md](docs/loop_dev.md) §1 定义的**无人监管循环**（默认上限 **3 轮** / 10 站；只在该文档授权范围内「跑 + 记录」，不夹带产品代码改动） |
| 「开始循环开发」（**L2/L3** · 实施 [M_node_expansion_part2.md](docs/Archive/actionPlan/M_node_expansion_part2.md)） | 启动 **无人监管实施循环**（规则 = 本文 §12）：自行 build / debug / test / 修错推进 L2（6 文本处理节点）+ L3（5 项画布·节点库 UX）；**含产品代码改动**；遇恶性 bug / 卡死按 §12 记录处理 |

**未获指令时禁止**：`git commit`（含 `--amend`）· `git tag` · `git push` · `git merge` · `git reset` · `git checkout`（切分支/回滚）· 改动 `CHANGELOG` 的版本号小节 · 把 `[Unreleased]` 条目版本化。

**随时允许（无需指令）**：`git status` / `git diff` / `git log` / `git grep` / `git show` 等**只读**命令；以及修改工作区文件本身（**改完不提交**）。

**遇到「必须提交才能继续」的情形**（例如验收要求打标签、需要基线 commit 做对比）⇒ **先问，再动**。

---

## 7. 禁止项（红线）

1. **未获指令就提交 / 改版本 / 打 tag / push**（见 §6）。
2. **未过验证就说「已完成」**：没有本轮 txt 物证（退出码 + 明细）不算完成。
3. **为了让测试变绿而改断言**：断言必须先有失败的**机理**解释（真机取证或代码级推演），再改代码或改期望。
4. **越界重写**：`source/third_party/**` 不做本地改造；冻结区（[docs/Archive/actionPlan/M7.md](docs/Archive/actionPlan/M7.md) §1 · `I17` 图片解码、`M8.md` 退场清单）改动前先读对应文档。
5. **删除 / 移动用户数据**：`~/.brain-ai/**`（含登录态、图片资源）只读诊断，禁止清理；`source/python/_probe/out/**` 等未跟踪探针产物不擅自删。
6. **只改代码不修文档**（违反 §4），或反过来「文档先行但不登记」。
7. **猜测式结论**：不确定就写「**待真机/待验证**」并登记到对应文档，不许编数字、编路径、编 API 行为。
8. **未获授权就启动自动循环 / 在循环里夹带代码改动**：不得自行开始 [docs/loop_dev.md](docs/loop_dev.md) 的循环开发（连续多站自动跑、自动回填站点结论）；循环内只允许改站点数据（`providers.json` 的 `verified` / `notes` / 选择器）与文档，**产品代码改动必须出循环**、走正常计划流程。（⚠ 例外：本文 §6 授权表「开始循环开发（L2/L3）」= **实施循环**，含产品代码改动，按 §12 规则运行，不受本条约束。）

---

## 8. 路径速查

| 类别 | 路径 |
|---|---|
| 行为契约（本文） | `AI_README.md` |
| 文档地图 / 进度 | `docs/DocMAP.md` |
| 变更日志 | `docs/CHANGELOG.md` |
| 现行计划 | `docs/actionPlan/M6.md` · `M8.md` · `M_patchAB_rest.md` · `M_patchC_web_node_bind.md` · `M_canvas_snapshot.md`（画布渲染性能专项 · **计划中**）· `M_node_expansion.md`（节点扩展：L1 已定向·**已实施完成** · **v5 定稿 = [M_node_expansion_part2.md](docs/Archive/actionPlan/M_node_expansion_part2.md)**（L2 6 节点 / L3 5 UX 项 🟢 定稿 · L4 不做 ⚠️ **组合层 L4 已于 2026-10-16 重启** · `CN-D20` · **实施完成（2026-10-16）· ✅ 2026-10-18 归档** · ⏳ 人工验收见 [docs/DevPlan.todo](docs/DevPlan.todo) Test · 原设计见 [Composition_node.md](docs/Archive/actionPlan/Composition_node.md)））· `milestone_plan.md` |
| 开发看板（JSON） | `docs/DevPlan.todo` |
| 循环开发规则 | `docs/loop_dev.md`（网页站逐站转可用：启动闸 / 三层结构 / 站级 8 步 / 轮次门禁 / 失败分类 / 退出码 / 记录位 / 停止条件） |
| 术语对照 | `docs/变量术语对照表.md` |
| 归档（只读追溯） | `docs/Archive/**`（含 `Archive/README.md` 归档规则） |
| 源码 / 构建 | `source/<模块>/{src,include}/**`（模块 = ai / engine / nodes / ui / utils / web；入口 `source/app/src/main.cpp`）· `source/tools/**` · `source/build.ps1` · 产物 `build/bin/**` |
| 离线夹具 | `source/tools/poll_answer_gate_test.js` · `source/tools/send_chain_test.js` · `source/tools/dom_gate_test.js` |
| 循环「一键一轮」驱动 | `source/tools/loop_verify.ps1`（`TST-L01` + `TST-L05` 站点台账 · 门禁 + S2 探测 + S3 逐站 + 报告落 `build\loop\<轮次>-<时间戳>\`） |
| 项目 / 站点状态页 | `docs/AI_Status.md`（**接手先看**：门禁基线 + 循环轮次 + 下一步；§二 = `TST-L05` 台账**汇总**，由 `loop_verify.ps1` 每轮 / `-StatusOnly` 机器回写 · **勿手改**）；分通道明细 `docs/AI_Status_web.md`（web 11 站）+ `docs/AI_Status_api.md`（official 9 条） |
| 运行期数据 / 日志 | `~/.brain-ai/`（`config.toml` · `assets/images/` · `logs/app.log`） |

---

## 9. 表达规范（AI 写给人类的文字）

> 前提：AI 助手的任务是**辅助人类开发** ⇒ 产出（汇报 / 文档 / 代码注释）的**第一读者是人**，不是机器，也不是下一轮的 AI。
> 判断标准：**人读一遍就能懂**；若需要来回查定义才读懂，就是没写好。

1. **用人类易读的语言**：先讲清「做了什么 / 为什么 / 影响什么」，再给细节；能用一句话说明白的，不要用缩写与代号堆砌。
2. **不裸用 magic number（魔法数字 · 含义不明的裸数值）/ magic symbol（魔法符号 · 含义不明的生僻符号）**：
   - 裸数值必须写清它是什么，例：`退出码 3`（`0x80000003`，CRT 断言中断）、`464 / 0`（执行器断言通过数 / 失败数）。
   - 生僻符号同样要写清，例：`335 → 397`（断言数由 335 增到 397，即 **+62**）；`≥2`（至少 2 个）。
3. **链接与代号一律紧随括号注明内容**（**就地可读**，不必回查定义）：
   - 代号：`D-26`（网页版站点条目的**字段级回落**是否收紧）、`VB2-22⑧`（`format_answer_text` 排版纯函数的 **8 例**断言）、`PQ-09`（看板「待定 · 未拍板」分类里的编号 · 已拍板 ⇒ **2026-10-13 随历史整组移入 `Archive`**）。
   - 链接：[docs/DocMAP.md](docs/DocMAP.md)（文档地图：什么改动读哪份文档）、[docs/CHANGELOG.md](docs/CHANGELOG.md)（变更日志：最近改了什么、上一轮验证数字）。
   - 首次出现必须注明；同一段落里已定义过的，其后可简写。
4. **问题描述要准确且简洁**：一句话写清「**现象 + 触发场景 + 后果**」；不夸大、不省略前提、不写「应该没问题」这类**无物证**结论（与 §3 取证纪律一致）。
5. **术语与代码逐字一致**：状态词只用 §5 白名单（已拍板 / 已落地 / 部分落地 / 计划中 / 已归档 / 已退场 / 待真机）；字段名 / 文件名 / 命令与代码一致，**不造新叫法**。

---

## 10. 本文的维护

- 本文**只放**：读取顺序、命令、纪律、**代码组织规范**、表达规范、禁止项、路径。**技术细节一律外链**权威文档，杜绝双份维护。
- 与 [docs/DocMAP.md](docs/DocMAP.md) 的分工：**本文 = 行为契约**；**DocMAP = 文档地图**。
- 基线数字会过期：本文数字**带实测日期**；变动时同步本文 + `CHANGELOG`（§4 义务 6），历史任务的数字留在原计划文档里、不追溯改写。
- 已知取舍：本文**不会被 VS Code / AI 工具自动加载**（无钩子），所以任务开始时请显式要求「先读 `AI_README.md`」。

### 10.1 数字口径（SSoT：每个数字只留一个权威位）

> **起因（2026-10-10 审计）**：同一批基线数字被复制到 ≥6 份文档，而每批只同步其中一部分 ⇒ 必然漂移（实例：本文件 §2 `--selftest` 行曾写 `执行器 452/0`，而同表下行 `--exec-selftest` 行已是 `464/0`）。**对策 = 每个数字只认一个权威位，其余处一律写「见 X」，不复述数值。**

| 数字 | **唯一权威位** | 其余文档怎么写 |
|---|---|---|
| 各命令的断言数 / 通过数（`--selftest` 各组 · `--exec-selftest` · `--graph-selftest` · `--upload-selftest` · 离线夹具 · `check_all` 项数） | **本文 §2** | 「见 [AI_README.md](AI_README.md) §2」 |
| 门禁**逐条命令与期望**（构建 0/0 · 逐项 PASS） | **本文 §2** | [docs/loop_dev.md](docs/loop_dev.md) §4 = 同一批数字的**循环化用法**；两处不一致时**以本文 §2 为准**并立即同步另一处 |
| **文档断链**数字（checked / broken） | **[docs/loop_dev.md](docs/loop_dev.md) §4** | 其他处只写判据（现行 = 「**全库 0**」） |
| 看板各分类的 `（N 项）` | **[docs/DevPlan.todo](docs/DevPlan.todo) 本身** | 其他处写「见看板」，不写数字 |
| 站点条数 / 各站能力与接线 | [docs/AI_Status.md](docs/AI_Status.md)（总览）→ `AI_Status_web.md` / `AI_Status_api.md` | 只写「见台账」 |
| 里程碑**落地当时**的验收数字 | 对应 `docs/actionPlan/*.md` | **不追溯改写**（历史快照） |

- **同步义务**：改权威位 ⇒ **只改权威位** + 在 `CHANGELOG` 记一行；某处若先前复制过数值，**改为指向权威位**，不再更新副本（否则下批必再漂移）。
- **看板计数**：用一次性脚本从 `todotree` 递归数叶子核对（2026-10-10 订正 = `Test` 28→29 · `Docs` 14→21 · `Test > Loop Dev` 3→4；`Debug 15` / `Feature 72` / `Archive 31` / `M7B 7` / `待定 5` 经核对无误）。

---

## 11. 代码组织规范（**新增文件必须遵守** · 2026-10-09 定稿 · `FEA-L05`）

> 起因（两轮）：① `FEA-L04` **之前**头文件与 `.cpp` 混放、`ai/` 下站点专属与通用件混放；② `FEA-L04` 只做到"头 / 实现分离"，但**左右两棵镜像树**（`source/src/**` + `source/include/**`）让「改一个模块」仍要跨两处跳转。
> 适用范围：`source/` 下的 C++ 源码（`source/python/**` 与 `source/third_party/**` **不受本规范约束**）。

### 11.1 两条硬规则

1. **每个模块目录自带 `src/` + `include/`**：实现 `.cpp` 放 `source/<模块>/src/**`，头文件放 `source/<模块>/include/**`（**模块内拍平**，不再镜像 `src/` 的子目录）。
   - 例：`source/engine/src/graph.cpp` ↔ `source/engine/include/graph.h`；`source/utils/src/log.cpp` ↔ `source/utils/include/log.h`。
   - **新文件落位判据**：先定它属于哪个模块（`ai` / `engine` / `nodes` / `ui` / `utils` / `web`），再接 `src/`（实现）或 `include/`（头文件）；程序入口 = `source/app/src/main.cpp`。
   - **引用写法 = 裸文件名**（`#include "graph.h"`，**不写**模块前缀）—— 因为 `source/CMakeLists.txt` 把**每个模块的 `include/`** 都挂成头文件根（`target_include_directories(aiwrite_core PUBLIC ...)` 逐行列 6 行）。
   - **前提与代价（如实说明）**：裸名成立的前提是「全仓的头文件**互不同名**」（2026-10-09 实测：53 个头文件无重名）；将来若真撞名，**改文件名**即可 —— 不要退回「模块前缀 + 单一 include 根」的双写形态。
2. **`source/ai/src/` 下按站点分目录**：只服务某一个站点的实现，放 `ai/src/<站点>/`（站点名取 `providers.json` 的条目名去掉 `-web`，例：`ai/src/deepseek/`、`ai/src/doubao/`、`ai/src/kimi/`）；**通用件不动**（配置表 `provider_spec` · DOM 适配器 `dom_web_client` · PoW `web_pow` · 供料探针 `upload_supply_probe` · 上传**框架** `upload/image_convert` / `upload_contract` / `upload_evidence` / `upload_registry` —— 直接放 `ai/src/`，不建站点子目录）；**站点上传单元** `site_upload_<站>.cpp` 属站点知识 ⇒ 归 `ai/src/<站>/`。
   - **判据**：改这个文件会只影响**一个**网页站 → 进 `ai/src/<站点>/`；会影响**任意**站 → 留 `ai/src/`。
   - **头文件不再跟着分站点**：站点专属头文件也放 `ai/include/` 平铺（例：`source/ai/src/deepseek/deepseek_web_client.cpp` ↔ `source/ai/include/deepseek_web_client.h`）—— 因为引用写裸名，站点子目录已无区分作用。

### 11.2 现状与迁移（诚实说明）

- 本规则**自 2026-10-09 起对"新增 / 修改的文件"生效**；
- **第 1 步 `FEA-L04`（2026-10-09 · 已被取代）**：53 个头文件 → `source/include/**`（镜像 `source/src/**`）、`source/src/ai/` 下按站点分目录、CMake 与连带文档同步；当时回归数字逐项不变。
- **第 2 步 `FEA-L05`（2026-10-09 · ✅ 已完成 · 现行）**：改成**每个模块自带 `src/` + `include/`**：**104 个文件** `git mv`（53 头 + 51 `.cpp`；其中入口 `source/src/main.cpp` → `source/app/src/main.cpp`），`source/src/` 与 `source/include/` 两个旧根**已删除**；**~250 处 `#include` 去掉模块前缀**；CMake 头文件根由 1 个改为 **6 个**（逐模块列出）；`source/tools/*.js`（三个夹具**直接从源码读页面脚本**）与 `.py` 注释里的路径一并同步。
  - **验证（2026-10-09 实测 · 逐项与旧基线一致）**：构建 **0 error / 0 warning**（`build/logs/build_debug_20261009_203915.log`）· `api_probe --selftest` **exit 0 · 七组全 PASS** · 执行器 **452/0** · 图模型 **110/0** · `aiwrite --upload-selftest` **72/0** · 夹具 **12/0 · 17/0 · 15/0**（夹具**直接读** `source/ai/src/dom_web_client.cpp`，路径改对才算过）。
- **搬迁已完成**（2026-10-09）⇒ 仓库里**只有新位置**：后续任何改动都必须按 §11.1 落位（**不要**再建 `source/{src,include}/**` 这种全局两根树）。

### 11.3 其他落位（既有约定 · 重申）

| 类型 | 位置 |
|---|---|
| 程序入口（GUI 主程序） | `source/app/src/main.cpp`（无头文件 ⇒ `app/` 只有 `src/`） |
| 命令行工具（`api_probe` / `webview2_login`） | `source/tools/*.cpp`（单文件，无头） |
| 离线夹具 / 驱动脚本 | `source/tools/*.js` · `source/tools/*.ps1` |
| 第三方源码（**禁止本地改造**） | `source/third_party/**` |
| Python 运行代码 | `source/python/**`（**不受 §11.1/11.2 约束**） |
| 站点数据 / 资产 | `source/assets/**`（`providers.json` 是站点数据的**唯一权威位**） |


---

## 12. 循环开发规则（无人监管 · L2/L3 实施循环专项）

> **适用**：开发者明确授权「开始循环开发（L2/L3）」实施 [docs/actionPlan/M_node_expansion_part2.md](docs/Archive/actionPlan/M_node_expansion_part2.md) 时生效（授权表见 §6）。
> 授权后 = **无人监管**：不再逐项征询、不再中途停下等确认，自行 **build / debug / test / 修错 / 推进**，直至任务完成或按本节规则暂停。
> 与 [docs/loop_dev.md](docs/loop_dev.md)（网页站逐站循环）是**两套独立规则**：本文 §12 只适用于 L2/L3 实施循环。

### 12.1 循环内基本纪律

1. **沿用本文全部既有纪律**：§3 取证（重定向 + 物证）、§4 改动伴随义务（每轮 CHANGELOG `[Unreleased]` 登记）、§6 提交纪律（**只改不提交**）、§7 禁止项。
2. **每轮闭环**：改完 → `.\source\build.ps1`（0 error / 0 warning）→ 相关自检（`--selftest` / `--exec-selftest` / `--graph-selftest` / `check_all.ps1`）→ 数字只升不降。
3. **每轮登记**：本轮的「改了什么 / 验证数字 / 遗留 / 跳过项」写入 CHANGELOG 与本文 §二 状态（如适用）。

### 12.2 恶性 bug / 无法解决的冲突 → **记录并跳过**

- **触发**：编译无法继续、功能无法实现且无绕过方案、需改动架构级契约（超出 part2 授权范围）、或排错超过合理轮次仍无机理。
- **动作**：在 CHANGELOG 记「**跳过 + 原因 + 现象物证**」，继续下一项实施，**不阻塞循环**；跳过项列入最终汇报的「遗留待办」。

### 12.3 运行卡死 → **有限重试，超限暂停**

- **第一次卡死**（build 后运行无响应 / 死循环 / 界面冻结）：**记录**（命令、场景、现象、日志物证），随后**再测 2 次**。
- **同一项累计超过 3 次卡死**：**不再反复运行测试**，在 CHANGELOG 记「**3 次卡死 · 暂停循环开发** + 现象物证」，**暂停循环**，等待开发者指示（不得自行继续改动绕过）。
- 卡死与「编译错误」分开计数；编译错误按 §12.2 正常排错处理。

---

## 13. 后台终端管理纪律（Cline 后台处理 · 2026-10-16 · **2026-10-19 强化：先杀后开 · 每次必做**）

> **适用**：Cline 的后台处理会为文件处理命令开启新的终端（terminal）来执行。
> 本规则是**硬约束**：控制后台终端数量，防止内存膨胀。
> **强化动因（2026-10-19）**：复核发现「新开 terminal 之前先执行 kill terminal 的命令」**未被实际执行**（后台终端持续累积）⇒ 现将 **先杀后开** 升格为**无条件硬性动作**，不设豁免。

- **先杀后开 · 每次新开 terminal 必做（无条件）**：每次要开启一个新的 terminal **之前**，**必须先执行 kill terminal 的命令**（清理已空闲 / 已完成的终端，执行闭环见 §13.1 三步），**再开启**新终端。
- **触发口径（powershell.exe 活跃终端数）**：以 **`powershell.exe` 活跃终端进程数**（§13.1 枚举口径）判定：
  - **≥ 3 ⇒ 硬触发**：先杀后开，杀完复查存活 **≤ 3**（理想 = 1，仅剩当前命令自身 shell）后才能开新终端；
  - **＜ 3 ⇒ 同样先杀再开**：数量少也**必须**先执行一轮 kill（清理空闲 / 已完成终端）再开，不得因「数量少」跳过 —— **无豁免**。
- **目的**：**节约内存，防止内存膨胀导致进程暴死** —— 后台终端长期挂起会持续占用内存，累积过多会造成内存膨胀 / 卡死 / 崩溃。

### 13.1 验证措施（kill 是否生效 · 2026-10-17 实测闭环）

> **执行时机（2026-10-19 强化）**：§13 三步闭环在**每次新开 terminal 之前**都必须完整执行一遍（先杀后开 · 无条件），不仅限于「数量 ≥ 3」时。
> Cline 无法直接操作 IDE 终端面板标签，因此用 **VS Code 集成终端后端进程**（`powershell.exe`）作为「活跃终端」的可执行代理口径。
> 实测：基线 8 → kill 7（保留当前命令自身 shell）→ 复查存活 1 ≤ 3，达标。

- **枚举口径**：`Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.ParentProcessId -eq <VS Code NodeService PID> }`
  - 判定「IDE 集成终端」：父进程为 **VS Code 的 NodeService utility**（`Code.exe --type=utility --utility-sub-type=node.mojom.NodeService`），且启动参数以 `-noexit -command "...shellIntegration.ps1..."` 开头（终端集成初始化脚本）。
  - **排除不杀**：`conhost.exe` / `Code.exe` 自身 / 系统进程（如 `SearchHost.exe` 下属的 `msedgewebview2`）均不是本纪律清理对象。
- **执行闭环（三步）**：
  1. **基线**：枚举当前 shell 的 PID 集合与数量（打印物证）。
  2. **kill**：对除 **`$PID`（当前命令自身 shell，杀掉即自断）** 与「正在跑任务的 shell」（命令行含 build.ps1 / python / 测试命令等）之外的终端执行 `Stop-Process -Id <pid> -Force`。
  3. **复查**：**同一枚举命令再跑一次**，核对已 kill 的 PID 不在存活列表（消失 = 生效），且存活数量 **≤ 3**（理想 = 1，仅剩当前命令自身）。
- **未达标处理**：存活 > 3 时**不得开新终端**，改由人工在 IDE 终端面板关闭标签（回收站按钮），关闭后**再次复查**直至 ≤ 3。
- **物证纪律**：基线 / kill 动作 / 复查三段输出保留在会话输出中，作为 kill 生效物证（对齐「收口时回填实测数字」纪律）。
- **杀后现象**：被杀终端的 VS Code 标签显示「进程已退出」，此为 kill 预期结果，标签可手动关闭，不属异常。

