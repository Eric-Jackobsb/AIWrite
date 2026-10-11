# AI_Status · AIwrite 项目与站点状态（一页看清）

> **用途**：接手时 **5 分钟看清「现在在哪 / 还差什么 / 别踩什么」**。配套：[../AI_README.md](../AI_README.md)（AI 行为契约）· [DocMAP.md](DocMAP.md)（文档地图）· [loop_dev.md](loop_dev.md)（循环规则 · 权威口径）· [CHANGELOG.md](CHANGELOG.md)（变更史）。
> **分通道明细**：**网页版 11 站** → [AI_Status_web.md](AI_Status_web.md) · **官方 API 9 条** → [AI_Status_api.md](AI_Status_api.md)（两份都是**机器生成的专项台账**；本文只留**汇总**，不重复列全量）。
> **维护**：**§二 汇总行机器生成** —— `loop_verify.ps1` 每轮结束 / `-StatusOnly` 回写哨兵块（**同一次调用**同时写 §2.2 合并一览 + 本文汇总 + 两份分片），**人工只审阅**（改数据请改 `source/assets/providers.json` / `source/assets/site_capability_evidence.json`）；§一 / §三 / §四 / §五 由 AI 协作者在每轮 / 每批收尾时更新。
> **最近更新**：2026-10-11（**🧩 `M_patchC` 批 1 已收口** —— 网页通道**窗口表 / 归属 / 归因分离 / 复位**（`PW-02`·`PW-04`·`PW-05`·`PW-06`·`PW-08` · 纯离线）⇒ `--exec-selftest` **464 → 490 / 0** · `check_all` **14/14** · 断链 **1097 / 0**；见 [actionPlan/M_patchC_web_node_bind.md](actionPlan/M_patchC_web_node_bind.md) §7.1b / §10 v7）；2026-10-10（`TST-L05` 台账**分通道拆分**：`AI_Status_web.md` + `AI_Status_api.md`；本文改为**总览**；同日追加 **`PB2-24` 自建站点闭环** ⇒ §一 增 `--site-assert` / `check_all` 13 项两行 · §四 第 5 条；**再追加 `PC-01`/`PC-02` 自动保存 + 崩溃恢复** ⇒ §一 增 `--autosave-assert` 行 · `check_all` **14 项** · §四 第 6 条；**同日文档可信度收口（审计订正）**：§一 `--selftest` 补 **DOM 探测** 组 · `--settings-assert` 越界订为 **11 例** · 文档断链判据升级为「**全库 0**」（存量 8 条清零）· **提交后复核**：本批已提交 **`486c30a`**（§四 第 4~6 条改记「已提交」）+ **M6 状态同步**（[roadmap.md](roadmap.md) §五 / [actionPlan/milestone_plan.md](actionPlan/milestone_plan.md) §一 / [actionPlan/M6.md](actionPlan/M6.md) 头部 → **🟡 第一批已落地**））；**2026-10-11 追加**：**节点扩展讨论稿** [actionPlan/M_node_expansion.md](actionPlan/M_node_expansion.md) 建立（**四层框架 L1~L4 · 零代码 · 🟡 讨论中（未定向）**）—— 看板同步 `PQ-20`（**待定 · 未拍板**）+ `DOCS-NE-01`（文档项）；**门禁 / 断链基线本批未变**（仅文档）

---

## 一、门禁基线（「起点是否干净」）

| 检查 | 命令 | 基线（最新实测） |
|---|---|---|
| 构建 | `cd source; .\build.ps1` | **0 error / 0 warning** |
| 工具 / 契约自检（总闸） | `api_probe.exe --selftest` | 七组全 PASS · **exit 0**（配置表 **50 / 0** · 图片准备 **19 / 0** · 上传证据 **32 / 0** · 站点上传模板 **21 / 0** · **DOM 探测 27 / 0** · 执行器 **490 / 0**） |
| 自建站点闭环（`PB2-24`） | `api_probe.exe --site-assert` | **PASS · exit 0**（**2026-10-10 实测**：模板纯函数 **47 / 0** + 真实写盘 → 重载 → 进表 / 进下拉 / 进枚举 **20→21** → 新节点校验 / 旧实例刷新 / 清理零残留）；⚠ 会**临时**写并删除 `~/.brain-ai/providers.d/aiwrite-selftest-site.json` |
| 自动保存 / 崩溃恢复（`PC-01` / `PC-02`） | `api_probe.exe --autosave-assert` | **PASS · exit 0**（**2026-10-10 实测**：场景 **28 / 0** + 默认落点 = `snapshots/autosave.json`）；⚠ 场景全在**系统临时目录**内构造，**不碰**用户快照 |
| 一键自检（总入口 · 上述全含） | `powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_all.ps1` | **14 / 14 PASS · exit 0**（**2026-10-10 实测**；12 → 13 = `PB2-24` 批新增 `--site-assert`，13 → 14 = `PC-01/02` 批新增 `--autosave-assert`） |
| 图模型 / 执行器自检 | `--graph-selftest` / `--exec-selftest` | **110 / 0** · **490 / 0**（2026-10-11：`M_patchC` 批 1 新增 **26** 条离线断言 = 464 → 490） |
| 上传模板（离线 · 不起浏览器） | `aiwrite.exe --upload-selftest` | **72 / 0** · exit **0** |
| 提供商标记 / 条目快照 | `aiwrite.exe --provider-selftest` / `--provider-dump` | 末行 `[配置表自检] 50 项通过 / 0 项失败` · **条目 20 条（official 9 / web 11）** |
| 离线夹具 + 负向打假 | `node source\tools\poll_answer_gate_test.js` · `send_chain_test.js` · `dom_gate_test.js`（各加 `--old` 必 FAIL） | **44 / 0** · **17 / 0** · **15 / 0** |
| 文档断链 | `python vcpkg-cache\check_links.py` | 检查 **1097** / broken **0**（**2026-10-11 实测**：批 0 = 995 → `M8` Phase 3 改判批 = 1022 → 待拍板项清零批 = 1077 → **`M_patchC` 批 1 = 1097（+17）**）；判据**已升级为「全库 0」**，权威位 = [loop_dev.md](loop_dev.md) §4；存量 8 条已清零（7 条真断链 + 1 条检查器误判）；`DBG-L01` 仅余「纳入 `check_all`」一项 |
| 循环一轮（全链路） | `powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -Round <名字>` | **门禁 FAIL 0 · 轮次 exit 0**（2026-10-10 `T-L26` 实测 · 11 项门禁全 PASS） |

> 数字的**权威位**是 [loop_dev.md](loop_dev.md) §4（门禁表）与 [../AI_README.md](../AI_README.md) §2（命令基线）；本表是**入口索引**，对不上时以那两处为准。

## 二、站点状态台账（20 条 · 分通道两份明细 · 汇总机器生成）

> **分通道看**：**网页版 11 站** → [AI_Status_web.md](AI_Status_web.md) §二（链路证据 + 能力与接线）· **官方 API 9 条** → [AI_Status_api.md](AI_Status_api.md) §二（协议与就绪 + 证据与动作）。
> **合并一览**（20 条：类型 / S2 探测 / S3 rc-n3 / 最终态 / 下一动作）= [loop_dev.md](loop_dev.md) §2.2 哨兵块。
> 下面汇总行与两份分片、§2.2 由 `loop_verify.ps1` **同一次调用**写入 ⇒ **同源、不会漂移**；分片依据 = `kind`（`web` × 11 / `official` × 9）。

<!-- SITE-STATUS:BEGIN （TST-L05 机器生成 · 人工只审阅 · 改数据请改 providers.json / site_capability_evidence.json） -->
> 生成：`loop_verify.ps1`（TST-L05）· 时间 2026-10-11 · 覆盖轮次 0：
> **分通道明细**（按 `kind` 分片 · 同一次调用写入 ⇒ 与本文汇总同源）：**网页版** `kind=web` **11** 站 → `docs/AI_Status_web.md` §二 · **官方 API** `kind=official` **9** 条 → `docs/AI_Status_api.md` §二
> **汇总**：共 20 条（web 11 / official 9）· 文字理解 ✅ 实测 **9** · 图片理解 ✅ 实测 **3** · 🟡 就绪/声明 6 · ⬜ 未接/未声明 9 · ⚠ 声明不一致 1
> 重建（零副作用，随时可跑）：`powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -StatusOnly`
<!-- SITE-STATUS:END -->

## 三、循环轮次（最近状态）

| 轮次 | 日期 | 结论 | 物证 / 备注 |
|---|---|---|---|
| **R1** | 2026-10-09 | S3 6 站 · 可用 **5** · 失败 **1** ⇒ exit 1 | `build\loop\R1-20261009-2055\` —— 首轮真机基线（暴露 VA-07 假阴性 + 退出码不分真假两个我方缺陷，已修） |
| `SMOKE7`~`SMOKE10` | 2026-10-09 | 单站烟测（非轮次） | 第三口径打假：`kimi-web` 站侧繁忙被正确判「失败」；`SMOKE10` 反面校准（短答案不误伤） |
| `SMOKE11`~`SMOKE13` | 2026-10-10 | **门禁 FAIL 0 · exit 0** | `SMOKE11` 暴露**门禁假 `FAIL`**（§10.1 陷阱 8）⇒ 修后 `SMOKE12` 全绿；`SMOKE13` 复验**跨路径幂等**（轮末块 = `-StatusOnly` 回写，SHA256 不变） |
| `T-L26`（去重轮 · 只跑门禁） | 2026-10-10 | **门禁 FAIL 0 · 轮次 exit 0**（11 项全 PASS） | `build\loop\T-L26-20261010-1523\` —— `-SkipProbe`（0 站 S3，本轮不探测）；**同时落地站点条目去重**（移除 `tongyi-web`）⇒ 条目 21 → **20 条**（web 12 → 11 = DOM 10 + `deepseek-web`）· 台账 **20 站** · 文字理解 ✅ **10** · 图片理解 ✅ **3** |

> 每轮逐行结论的权威位 = [loop_dev.md](loop_dev.md) §11；轮次细节 = [CHANGELOG.md](CHANGELOG.md) `[Unreleased]`。

## 四、下一步（按两条线分列 · 细节看各自分片表「下一动作」列）

1. **网页版线**（[AI_Status_web.md](AI_Status_web.md) §二 表 B）：`spark-web` 人工窗口内登录一次 → 重跑 S3；`kimi-web` 复测 2~3 次定性（偶发 / 限流）；`chatglm-web` / `spark-web` 回填 `web.input_selector` 并收紧 S2 判据；`kimi-web` 侦察 `attach_selector` → 图片链路真机一次。
2. **官方 API 线**（[AI_Status_api.md](AI_Status_api.md) §二 表 B）：`deepseek` 回填 `providers.json` 的 `capabilities.vision`（`M7-10`）；`custom-official` 填 `api_base` + Key；`openrouter` / `openai` / `ollama` / `siliconflow` 有 Key 时真机一次；`anthropic` / `gemini` 待协议实现排期。
3. **循环侧**：把「探测态 → S3 → 最终态」的站数从 1 站逐步扩到全量（当前 `SMOKE*` 均为单站烟测；正式轮次仍以 §1 授权为准）。
4. ✅ **已提交（2026-10-10 · 单次提交 `486c30a`「docs update」· 32 文件 · `+2637 / −104`）** —— 原「未提交」记录（提交前）：批 9（`TST-L05` 台账 + 本轮门禁修复 + 本轮**分通道拆分**）**尚未 commit**（2026-10-10 追加**站点条目去重**：移除与 `qwen-web` 重复的 `tongyi-web` ⇒ 条目 21 → **20 条**（web 12 → 11）· `testAllAI.json` 85/84 → **82/81** · 台账 20 站）—— ✅ **已由开发者提交（2026-10-10 · `486c30a`）**。本批一次提交覆盖：批 9 + 站点去重（21 → **20 条**）+ `PB2-24`（第 5 条）+ `PC-01`/`PC-02`（第 6 条）+ 文档可信度收口；后续提交按 [../AI_README.md](../AI_README.md) §6 **由开发者决定**。
5. **`PB2-24` 自建站点闭环**（2026-10-10 · ✅ **已随 `486c30a` 提交**）：代码 + 离线断言已落地（`api_probe --site-assert` **PASS** · `check_all.ps1` **14/14**（本项落地时 13/13）· 构建 0/0），**⛔ 界面点选回路未人工实测** ⇒ 待真人做一次：「文件 → 设置…」→ 分区⑥ → 「新建网页版站点…」→ 创建 → 下拉出现 → 选中 → 「打开登录窗口」；细节见 [CHANGELOG.md](CHANGELOG.md) `[Unreleased]`「`PB2-24`」条目。同批**未做** = 面板「测试选择器」（`PB2-30②`）。
6. **`PC-01` / `PC-02` 自动保存 + 崩溃恢复**（2026-10-10 · ✅ **已随 `486c30a` 提交**）：纯逻辑 `engine/autosave.*` + `ui/editor_state.*` 节流 + `ui/app.cpp` 恢复询问；`api_probe --autosave-assert` **28/0** · `--settings-assert` 越界 11 例 · `check_all.ps1` **14/14** · 构建 0/0；GUI 实测（真机）= 启动「发现快照 / 将询问…」日志出现 + **12 秒后快照哈希不变**（启动不冲掉真快照）。**⛔ 仍需真人点一次鼠标**：「改一个节点参数（或拖动节点）→ 静默 3 秒 → `~/.brain-ai/snapshots/autosave.json` 出现/更新 → 重启 → 弹窗点『恢复』→ 画布内容一致（状态栏『已恢复未保存的更改』）」。细节 + 陷阱 10（`json::operator+=` = `push_back`）见 [CHANGELOG.md](CHANGELOG.md) `[Unreleased]`「`PC-01` / `PC-02`」条目与 [loop_dev.md](loop_dev.md) §10.1。

## 五、纪律（别踩）

1. **改数据只改两处**：`source/assets/providers.json`（声明 / 接线）× `source/assets/site_capability_evidence.json`（实测证据）⇒ **四个落点**（[loop_dev.md](loop_dev.md) §2.2 合并一览 · 本文汇总 · [AI_Status_web.md](AI_Status_web.md) §二 · [AI_Status_api.md](AI_Status_api.md) §二）由脚本**同一次调用**重建，**不要手改表格**。
2. **声明 ≠ 实测**：`capabilities.vision` / `verified` / `web.upload_adapter` 是「声明」，只有 S3 / 上传双证据才算「实测」（§10.1 陷阱 7）。
3. **看结论先看物证**：`build/**` 是易失过程产物 ⇒ 结论必须落到 CHANGELOG + 本文档（+ 对应分片：web / api）+ [loop_dev.md](loop_dev.md) §11（**只写报告不算完成**）。
4. **⛔ 不擅自 `commit` / `tag` / `push` / 改版本号**（[../AI_README.md](../AI_README.md) §6）。
