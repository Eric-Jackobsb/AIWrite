# AI_Status · AIwrite 项目与站点状态（一页看清）

> **用途**：接手时 **5 分钟看清「现在在哪 / 还差什么 / 别踩什么」**。配套：[../AI_README.md](../AI_README.md)（AI 行为契约）· [DocMAP.md](DocMAP.md)（文档地图）· [loop_dev.md](loop_dev.md)（循环规则 · 权威口径）· [CHANGELOG.md](CHANGELOG.md)（变更史）。
> **分通道明细**：**网页版 11 站** → [AI_Status_web.md](AI_Status_web.md) · **官方 API 9 条** → [AI_Status_api.md](AI_Status_api.md)（两份都是**机器生成的专项台账**；本文只留**汇总**，不重复列全量）。
> **维护**：**§二 汇总行机器生成** —— `loop_verify.ps1` 每轮结束 / `-StatusOnly` 回写哨兵块（**同一次调用**同时写 §2.2 合并一览 + 本文汇总 + 两份分片），**人工只审阅**（改数据请改 `source/assets/providers.json` / `source/assets/site_capability_evidence.json`）；§一 / §三 / §四 / §五 由 AI 协作者在每轮 / 每批收尾时更新。
> **最近更新**：2026-10-10（`TST-L05` 台账**分通道拆分**：`AI_Status_web.md` + `AI_Status_api.md`；本文改为**总览**）

---

## 一、门禁基线（「起点是否干净」）

| 检查 | 命令 | 基线（最新实测） |
|---|---|---|
| 构建 | `cd source; .\build.ps1` | **0 error / 0 warning** |
| 工具 / 契约自检（总闸） | `api_probe.exe --selftest` | 七组全 PASS · **exit 0**（配置表 **50 / 0** · 图片准备 **19 / 0** · 上传证据 **32 / 0** · 站点上传模板 **21 / 0** · 执行器 **464 / 0**） |
| 图模型 / 执行器自检 | `--graph-selftest` / `--exec-selftest` | **110 / 0** · **464 / 0** |
| 上传模板（离线 · 不起浏览器） | `aiwrite.exe --upload-selftest` | **72 / 0** · exit **0** |
| 提供商标记 / 条目快照 | `aiwrite.exe --provider-selftest` / `--provider-dump` | 末行 `[配置表自检] 50 项通过 / 0 项失败` · **条目 20 条（official 9 / web 11）** |
| 离线夹具 + 负向打假 | `node source\tools\poll_answer_gate_test.js` · `send_chain_test.js` · `dom_gate_test.js`（各加 `--old` 必 FAIL） | **44 / 0** · **17 / 0** · **15 / 0** |
| 文档断链 | `python vcpkg-cache\check_links.py` | 检查 **769** / broken **8**（判据 = **本轮不得新增**，非「全库 0」；已登记 `DBG-L01`） |
| 循环一轮（全链路） | `powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -Round <名字>` | **门禁 FAIL 0 · 轮次 exit 0**（2026-10-10 `T-L26` 实测 · 11 项门禁全 PASS） |

> 数字的**权威位**是 [loop_dev.md](loop_dev.md) §4（门禁表）与 [../AI_README.md](../AI_README.md) §2（命令基线）；本表是**入口索引**，对不上时以那两处为准。

## 二、站点状态台账（20 条 · 分通道两份明细 · 汇总机器生成）

> **分通道看**：**网页版 11 站** → [AI_Status_web.md](AI_Status_web.md) §二（链路证据 + 能力与接线）· **官方 API 9 条** → [AI_Status_api.md](AI_Status_api.md) §二（协议与就绪 + 证据与动作）。
> **合并一览**（20 条：类型 / S2 探测 / S3 rc-n3 / 最终态 / 下一动作）= [loop_dev.md](loop_dev.md) §2.2 哨兵块。
> 下面汇总行与两份分片、§2.2 由 `loop_verify.ps1` **同一次调用**写入 ⇒ **同源、不会漂移**；分片依据 = `kind`（`web` × 11 / `official` × 9）。

<!-- SITE-STATUS:BEGIN （TST-L05 机器生成 · 人工只审阅 · 改数据请改 providers.json / site_capability_evidence.json） -->
> 生成：`loop_verify.ps1`（TST-L05）· 时间 2026-10-10 · 覆盖轮次 39：R1 · SMOKE · SMOKE2 · SMOKE3 · SMOKE4 · SMOKE5 · SMOKE6 · SMOKE6 · SMOKE7 · SMOKE8 · SMOKE9 · SMOKE10 · SMOKE11 · SMOKE12 · SMOKE13 · T-L06 · T-L06b · T-L06c · T-L06d · T-L07 · T-L08 · T-L09 · T-L10 · T-L11 · T-L12 · T-L13 · T-L14 · T-L15 · T-L16 · T-L17 · T-L18 · T-L19 · T-L20 · T-L21 · T-L22 · T-L23 · T-L24 · T-L25 · T-L26
> **分通道明细**（按 `kind` 分片 · 同一次调用写入 ⇒ 与本文汇总同源）：**网页版** `kind=web` **11** 站 → `docs/AI_Status_web.md` §二 · **官方 API** `kind=official` **9** 条 → `docs/AI_Status_api.md` §二
> **汇总**：共 20 条（web 11 / official 9）· 文字理解 ✅ 实测 **10** · 图片理解 ✅ 实测 **3** · 🟡 就绪/声明 6 · ⬜ 未接/未声明 9 · ⚠ 声明不一致 1
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
4. **未提交变更**：批 9（`TST-L05` 台账 + 本轮门禁修复 + 本轮**分通道拆分**）**尚未 commit**（2026-10-10 追加**站点条目去重**：移除与 `qwen-web` 重复的 `tongyi-web` ⇒ 条目 21 → **20 条**（web 12 → 11）· `testAllAI.json` 85/84 → **82/81** · 台账 20 站）—— ⛔ 按 [../AI_README.md](../AI_README.md) §6，**提交由开发者决定**。

## 五、纪律（别踩）

1. **改数据只改两处**：`source/assets/providers.json`（声明 / 接线）× `source/assets/site_capability_evidence.json`（实测证据）⇒ **四个落点**（[loop_dev.md](loop_dev.md) §2.2 合并一览 · 本文汇总 · [AI_Status_web.md](AI_Status_web.md) §二 · [AI_Status_api.md](AI_Status_api.md) §二）由脚本**同一次调用**重建，**不要手改表格**。
2. **声明 ≠ 实测**：`capabilities.vision` / `verified` / `web.upload_adapter` 是「声明」，只有 S3 / 上传双证据才算「实测」（§10.1 陷阱 7）。
3. **看结论先看物证**：`build/**` 是易失过程产物 ⇒ 结论必须落到 CHANGELOG + 本文档（+ 对应分片：web / api）+ [loop_dev.md](loop_dev.md) §11（**只写报告不算完成**）。
4. **⛔ 不擅自 `commit` / `tag` / `push` / 改版本号**（[../AI_README.md](../AI_README.md) §6）。
