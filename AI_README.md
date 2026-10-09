# AI_README —— AI / 协作者行为契约（AIwrite）

> 本文是**给 AI 助手与新协作者的行为契约**：只写「读取顺序 + 命令 + 纪律 + 禁止项」，技术细节一律指向权威文档。
> 接手任何改动前，请按 §1 顺序读文档，并遵守 §3 取证纪律、§4 改动伴随义务、§6 提交纪律、§7 禁止项、§9 表达规范（写给人类的文字）。
> **本文 §2 的数字 = 最近一次全量实测（2026-10-08 本机）**；各任务**落地当时**的验收数字以对应计划文档为准（例：[docs/actionPlan/M_patchC_web_node_bind.md](docs/actionPlan/M_patchC_web_node_bind.md) §7.1 的历史行**不追溯改写**）。

---

## 1. 读取顺序（按序读 4 处）

1. **[docs/DocMAP.md](docs/DocMAP.md)** —— 文档地图：什么改动读哪份文档。
   > 更名记录：`docs/README.md` → `docs/DocMAP.md`（2026-10-08 · `git mv` 保留历史）。历史条目（含 `docs/Archive/**`）里出现的**旧文件名与旧自链写法**均指本文。
2. **[docs/actionPlan/](docs/actionPlan/)** 中**现行**的那一份（`M6.md` / `M7.md` / `M8.md` / `M_patchAB_rest.md` / `M_patchC_web_node_bind.md` / `milestone_plan.md`）—— 不变量 `Ixx`、决策 `D-xx`、计划 `PW-xx`/`M*-xx`、§7 验收标准都在这。
3. **[docs/CHANGELOG.md](docs/CHANGELOG.md)** 最近条目 —— 最后一次改了什么、上一轮验证数字是多少。
4. 搜不到代码里的名字（类 / 函数 / 配置键） ⇒ 先查 **[docs/变量术语对照表.md](docs/变量术语对照表.md)**（§12 已知文档≠代码 · §13 新增功能检索入口）。

**动手前先自问并写下答案**：这属于哪个里程碑/补丁？对应计划里的哪一条？验收标准是什么？需不需要真机验证？

---

## 2. 命令表（都在仓库根执行；产物在 `build\bin\`）

| 目的 | 命令 | 基线（**2026-10-08 本机实测**） |
|---|---|---|
| 构建（Debug） | `cd source; .\build.ps1`（= `cmake --preset default` + `cmake --build --preset debug`） | **0 error / 0 warning** |
| 工具 / 契约自检（总闸） | `api_probe.exe --selftest` | 末行 `=== 结果: … PASS ===` · exit **0**；含 配置表 **50/0** · 图片准备 **19/0** · 上传证据 **32/0** · 站点上传模板 **21/0** · **执行器 425/0** |
| 执行器自检 | `api_probe.exe --exec-selftest` | **425 / 0** · exit 0 |
| 图模型自检 | `api_probe.exe --graph-selftest` | **110 / 0** · exit 0 |
| 上传链路自检（离线三组；真机加 `--provider <id> --image <图>`） | `aiwrite.exe --upload-selftest` | **72 / 0** · exit 0（⚠️ 见 §3「GUI 子系统 stdout」；**不要并发跑**，共用 `%TEMP%\aiwrite-upload\selftest`） |
| 运行时（真实工作流） | `aiwrite.exe --run-selftest` / `--run-selftest --web` | PASS（`official` 需 `~/.brain-ai/config.toml` 凭据；`--web` 需已登录一次）· 本轮未复跑 |
| 网页登录自检 | `webview2_login.exe --selftest --timeout 30` | 退出码 **0** = PASS / 1 = FAIL / 2 = 超时 |
| 选择器诊断（**只读**） | `aiwrite.exe --web-adapter-selftest --provider <id>` · `--web-dom-dump --provider <id>` | 打印命中数 / 候选与建议选择器 |
| 夹具（抽真实源码脚本，迷你 DOM 真跑） | `node source\tools\poll_answer_gate_test.js` · `node source\tools\send_chain_test.js` | **12 / 0** · **17 / 0** |
| 夹具**负向**打假（必须 FAIL） | 上两条加 `--old` | 门夹具 **5 / 7** · **exit 1**（失败项 = 预期） |
| 日志 | `%USERPROFILE%\.brain-ai\logs\app.log`（每 2 秒落盘，可直接 tail） | `[诊断]` 行限流输出 |
| VS Code | `Ctrl+Shift+B` 构建 · `F5` 调试 · Run Task：`build` / `build (release)` / `verify: api_probe 自检` / `verify: 图模型自检` / `log: 查看 app.log` | — |

> 完整开关清单（含 `--image-decode` / `--sha3` / `--pydoll-*` / `--login-selftest` 等）见 [source/README.md](source/README.md) §4。
> 数字变动时：先在 `CHANGELOG` 记本轮实测 + 同步相关计划行的验收数字，**再**回来改本文这一栏（§4 义务 6）。

---

## 3. 取证纪律（硬约束 · 踩坑总结）

- **输出一律重定向**：`& .\build\bin\api_probe.exe --selftest > $env:TEMP\x.txt 2>&1` 再 `Get-Content $env:TEMP\x.txt` 读回。
  *为什么*：本机 VS Code 的 shell 集成会**吞掉管道的输出**，`| Select-String` / `| Tee-Object` 会得到空结果 ⇒ 曾导致三套 selftest 被误判为「失败」。
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

- **编号空间**：`Ixx`（不变量）/ `D-xx` / `W-Dx`（修正）/ `PW-xx`（补丁 C 任务）/ `M*-xx`（里程碑任务）/ `B*-xx`（批次）/ `AB*-xx`（验收）/ `VB*-xx` / `VB2-xx` / `A*-xx`（验证）/ `PB2-xx` / `R*-xx`（风险）/ `RM-D*`（路线决策）。
- **状态词白名单**（只用这几个，别自创）：**已拍板 / 已落地 / 部分落地 / 计划中 / 已归档 / 已退场 / 待真机**。
- **文档三态标记**（全库统一 · 定义见 [docs/DocMAP.md](docs/DocMAP.md) §五）：✅ **已固定**（已落地视为契约）/ 📦 **已失效**（⛔ **只标记不删除**，必须给「现行指向」）/ ⬜ **待定**（未拍板 ⇒ **必须**在 `DevPlan.todo` 的「**待定 · 未拍板**」分类里有同名 `PQ-xx` 项；拍板后同批置 done）。
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

**未获指令时禁止**：`git commit`（含 `--amend`）· `git tag` · `git push` · `git merge` · `git reset` · `git checkout`（切分支/回滚）· 改动 `CHANGELOG` 的版本号小节 · 把 `[Unreleased]` 条目版本化。

**随时允许（无需指令）**：`git status` / `git diff` / `git log` / `git grep` / `git show` 等**只读**命令；以及修改工作区文件本身（**改完不提交**）。

**遇到「必须提交才能继续」的情形**（例如验收要求打标签、需要基线 commit 做对比）⇒ **先问，再动**。

---

## 7. 禁止项（红线）

1. **未获指令就提交 / 改版本 / 打 tag / push**（见 §6）。
2. **未过验证就说「已完成」**：没有本轮 txt 物证（退出码 + 明细）不算完成。
3. **为了让测试变绿而改断言**：断言必须先有失败的**机理**解释（真机取证或代码级推演），再改代码或改期望。
4. **越界重写**：`source/third_party/**` 不做本地改造；冻结区（`M7.md` §1 · `I17` 图片解码、`M8.md` 退场清单）改动前先读对应文档。
5. **删除 / 移动用户数据**：`~/.brain-ai/**`（含登录态、图片资源）只读诊断，禁止清理；`source/python/_probe/out/**` 等未跟踪探针产物不擅自删。
6. **只改代码不修文档**（违反 §4），或反过来「文档先行但不登记」。
7. **猜测式结论**：不确定就写「**待真机/待验证**」并登记到对应文档，不许编数字、编路径、编 API 行为。

---

## 8. 路径速查

| 类别 | 路径 |
|---|---|
| 行为契约（本文） | `AI_README.md` |
| 文档地图 / 进度 | `docs/DocMAP.md` |
| 变更日志 | `docs/CHANGELOG.md` |
| 现行计划 | `docs/actionPlan/M6.md` · `M7.md` · `M8.md` · `M_patchAB_rest.md` · `M_patchC_web_node_bind.md` · `milestone_plan.md` |
| 开发看板（JSON） | `docs/DevPlan.todo` |
| 术语对照 | `docs/变量术语对照表.md` |
| 归档（只读追溯） | `docs/Archive/**`（含 `Archive/README.md` 归档规则） |
| 源码 / 构建 | `source/src/**` · `source/tools/**` · `source/build.ps1` · 产物 `build/bin/**` |
| 离线夹具 | `source/tools/poll_answer_gate_test.js` · `source/tools/send_chain_test.js` |
| 运行期数据 / 日志 | `~/.brain-ai/`（`config.toml` · `assets/images/` · `logs/app.log`） |

---

## 9. 表达规范（AI 写给人类的文字）

> 前提：AI 助手的任务是**辅助人类开发** ⇒ 产出（汇报 / 文档 / 代码注释）的**第一读者是人**，不是机器，也不是下一轮的 AI。
> 判断标准：**人读一遍就能懂**；若需要来回查定义才读懂，就是没写好。

1. **用人类易读的语言**：先讲清「做了什么 / 为什么 / 影响什么」，再给细节；能用一句话说明白的，不要用缩写与代号堆砌。
2. **不裸用 magic number（魔法数字 · 含义不明的裸数值）/ magic symbol（魔法符号 · 含义不明的生僻符号）**：
   - 裸数值必须写清它是什么，例：`退出码 3`（`0x80000003`，CRT 断言中断）、`425 / 0`（执行器断言通过数 / 失败数）。
   - 生僻符号同样要写清，例：`335 → 397`（断言数由 335 增到 397，即 **+62**）；`≥2`（至少 2 个）。
3. **链接与代号一律紧随括号注明内容**（**就地可读**，不必回查定义）：
   - 代号：`D-26`（网页版站点条目的**字段级回落**是否收紧）、`VB2-22⑧`（`format_answer_text` 排版纯函数的 **8 例**断言）、`PQ-09`（看板「待定 · 未拍板」分类里的编号，拍板后同批置 done）。
   - 链接：[docs/DocMAP.md](docs/DocMAP.md)（文档地图：什么改动读哪份文档）、[docs/CHANGELOG.md](docs/CHANGELOG.md)（变更日志：最近改了什么、上一轮验证数字）。
   - 首次出现必须注明；同一段落里已定义过的，其后可简写。
4. **问题描述要准确且简洁**：一句话写清「**现象 + 触发场景 + 后果**」；不夸大、不省略前提、不写「应该没问题」这类**无物证**结论（与 §3 取证纪律一致）。
5. **术语与代码逐字一致**：状态词只用 §5 白名单（已拍板 / 已落地 / 部分落地 / 计划中 / 已归档 / 已退场 / 待真机）；字段名 / 文件名 / 命令与代码一致，**不造新叫法**。

---

## 10. 本文的维护

- 本文**只放**：读取顺序、命令、纪律、表达规范、禁止项、路径。**技术细节一律外链**权威文档，杜绝双份维护。
- 与 [docs/DocMAP.md](docs/DocMAP.md) 的分工：**本文 = 行为契约**；**DocMAP = 文档地图**。
- 基线数字会过期：本文数字**带实测日期**；变动时同步本文 + `CHANGELOG`（§4 义务 6），历史任务的数字留在原计划文档里、不追溯改写。
- 已知取舍：本文**不会被 VS Code / AI 工具自动加载**（无钩子），所以任务开始时请显式要求「先读 `AI_README.md`」。
