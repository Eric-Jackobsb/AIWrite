# AI_Status · AIwrite 项目与站点状态（一页看清）

> **用途**：接手时 **5 分钟看清「现在在哪 / 还差什么 / 别踩什么」**。配套：[../AI_README.md](../AI_README.md)（AI 行为契约）· [DocMAP.md](DocMAP.md)（文档地图）· [loop_dev.md](loop_dev.md)（循环规则 · 权威口径）· [CHANGELOG.md](CHANGELOG.md)（变更史）。
> **分通道明细**：**网页版 11 站** → [AI_Status_web.md](AI_Status_web.md) · **官方 API 9 条** → [AI_Status_api.md](AI_Status_api.md)（两份都是**机器生成的专项台账**；本文只留**汇总**，不重复列全量）。
> **维护**：**§二 汇总行机器生成** —— `loop_verify.ps1` 每轮结束 / `-StatusOnly` 回写哨兵块（**同一次调用**同时写 §2.2 合并一览 + 本文汇总 + 两份分片），**人工只审阅**（改数据请改 `source/assets/providers.json` / `source/assets/site_capability_evidence.json`）；§一 / §三 / §四 / §五 由 AI 协作者在每轮 / 每批收尾时更新。
> **最近更新**：2026-10-24（🧹 **仓库瘦身 S1+S2**：移除未编译的上游示例 / 外置 imgui 副本（约 −70 文件 / −3.5 MB）+ 根目录栈取证移出版本库 · 门禁 21/21 逐项不变）· 2026-10-23（📚 7 份已收口 actionPlan 归档 + 🗂️ 打磨主线入看板）· 2026-10-21（🗂️ ToVerify 并入 DevPlan Test）。
> **历史更新明细**：见 [CHANGELOG.md](CHANGELOG.md)（`[Unreleased]` 批次表 · 本文只留最近三轮）。

---

## 一、门禁基线（「起点是否干净」）

| 检查 | 命令 | 基线（最新实测） |
|---|---|---|
| 构建 | `cd source; .\build.ps1` | **0 error / 0 warning**（可选 `-SelfCheck` ⇒ 编译成功后自动跑一键自检，`PM-03` · 2026-10-11） |
| 工具 / 契约自检（总闸） | `api_probe.exe --selftest` | **十三组全 PASS · exit 0**（配置表 **52 / 0** · 图片准备 **19 / 0** · 上传证据 **32 / 0** · 站点上传模板 **21 / 0** · **DOM 探测 27 / 0** · 执行器 **682 / 0** · **无 Key 请求构造 6 / 0**（V-11）· **取消与超时 17 / 0**（V-12）· **L2 请求构造 23 / 0**（V-13）· **L2 工厂分派 13 / 0**（V-14）· **L2 回环端到端 11 / 0**（V-15）· **网络与凭据 16 / 0**（V-16）—— 批 7：十二组 → 十三组） |
| 自建站点闭环（`PB2-24`） | `api_probe.exe --site-assert` | **PASS · exit 0**（**2026-10-10 实测**：模板纯函数 **47 / 0** + 真实写盘 → 重载 → 进表 / 进下拉 / 进枚举 **20→21** → 新节点校验 / 旧实例刷新 / 清理零残留）；⚠ 会**临时**写并删除 `~/.brain-ai/providers.d/aiwrite-selftest-site.json` |
| 自动保存 / 崩溃恢复（`PC-01` / `PC-02`） | `api_probe.exe --autosave-assert` | **PASS · exit 0**（**2026-10-10 实测**：场景 **28 / 0** + 默认落点 = `snapshots/autosave.json`）；⚠ 场景全在**系统临时目录**内构造，**不碰**用户快照 |
| 一键自检（总入口 · 上述全含 + 依赖单向 + 全局变量 + 文档断链 + 打包 payload） | `powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_all.ps1` | **21 / 21 PASS · exit 0**（**2026-10-23 实测**（`ARCH-A5` 批）；12 → 13 = `PB2-24` 批新增 `--site-assert`，13 → 14 = `PC-01/02` 批新增 `--autosave-assert`，**14 → 15 = 批 5 新增 文档断链**（`source/tools/check_links.py` 入库），**15 → 16 = 批 ③ 新增 打包 payload**（`package.ps1 -SkipBuild -SkipInstaller` 入闸），**16 → 17~20 = 重构期新增项**（`ce_repro` / CLI 自检等），**19 → 20 = `ARCH-A4` 新增 ⑥ 依赖单向**，**20 → 21 = `ARCH-A5` 新增 ⑦ 全局变量**） |
| 图模型 / 执行器自检 | `--graph-selftest` / `--exec-selftest` | **151 / 0** · **682 / 0**（2026-10-19 复测 · 原 2026-10-11 = **110 / 0** · **546 / 0**；exec 链：546 → 559 → 585 → 592 → 598（L1）→ **655**（L2）→ **681**（CN 实施批 +26）→ **682**（2026-10-19 CN 双源回归补强 +1）· 2026-10-11：`M_patchC` 批 1 **+26** = 464 → 490 · `M_patchAB` 批 2 **+34** = 490 → 524 · 批 4 **+20** = 524 → 544 · 批 6 **+2** = 544 → 546（配置表 `R18` 嵌套 schema 两条）） |
| 上传模板（离线 · 不起浏览器） | `aiwrite.exe --upload-selftest` | **72 / 0** · exit **0** |
| 提供商标记 / 条目快照 | `aiwrite.exe --provider-selftest` / `--provider-dump` | 末行 `[配置表自检] 52 项通过 / 0 项失败`（批 6：50 → 52）· **条目 20 条（official 9 / web 11）** · dump 末行「**已实现协议：openai、anthropic、gemini、deepseek-web、dom**」 |
| 离线夹具 + 负向打假 | `node source\tools\poll_answer_gate_test.js` · `send_chain_test.js` · `dom_gate_test.js`（各加 `--old` 必 FAIL） | **44 / 0** · **17 / 0** · **15 / 0** |
| 文档断链（**已入闸** = `check_all` 第 ⑧ 项） | `python source\tools\check_links.py` | 检查 **1487** / broken **0**（**2026-10-15 L2/L3 批实测**（文档同步 **+138**）· 原 2026-10-13 批 9-4 = **1349 / 0** —— 归位前 **1219 / broken 100** ⇒ 归位后 **1219 / 0** ⇒ 批 9-0 索引同步 **+26** = **1246 / 0** ⇒ 批 9-1 登记面同步 **+37** = **1283 / 0**；历史链：批 5 = 1161 → 批 6 = 1175 → 批 7 = 1184 → 批 8 = 1197 → **批 9-0 = 1246** → **批 9-1 = 1283** → **批 9-2 = 1283**（用户拍板落盘 · 零代码 · 无新增链接）→ **批 9-3 = 1341**（创作流程依据文档落盘 **+20** + 登记面同步 **+38** + 归位 · **归位前 = 1303 / broken 16** ⇒ 归位后 **1303 / 0** ⇒ 终值 **1341 / 0**）→ **批 9-4 = 1349**（看板归档批的文档同步 **+8**）→ **L2/L3 批 = 1487**（2026-10-15 · 文档同步 +138））；脚本**已入库**（取代 `vcpkg-cache/_tmp` 临时脚本）+ **修掉跨行 / 跨反引号误判**；`DBG-L01` 两条残余**均已收口**）；判据 = 「全库 0」，权威位 = [loop_dev.md](loop_dev.md) §4 |
| 打包 payload（**已入闸** = `check_all` 第 ⑨ 项） | `powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\package.ps1 -SkipBuild -SkipInstaller` | **exit=2 · payload 19 文件 / 34.6 MB**（含随包 Bootstrapper · 批 ⑤ 起）· Python 摘钩 `*.py=0` · `.iss ↔ payload` 一致性锁 **4 条通配 PASS**（**2026-10-17 实测**）；✅ **ISCC 6.7.3 已装**（winget · 批 ⑥）· `package.ps1` 全流程 **exit 0** · 安装包 `build\dist\AIwrite-Setup-0.2.0.exe` **9.1 MB** 已编译（2026-10-17 批 ⑥） |
| 循环一轮（全链路） | `powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -Round <名字>` | **门禁 FAIL 0 · 轮次 exit 0**（2026-10-10 `T-L26` 实测 · 11 项门禁全 PASS） |

> 数字的**权威位**是 [loop_dev.md](loop_dev.md) §4（门禁表）与 [../AI_README.md](../AI_README.md) §2（命令基线）；本表是**入口索引**，对不上时以那两处为准。

## 二、站点状态台账（20 条 · 分通道两份明细 · 汇总机器生成）

> **分通道看**：**网页版 11 站** → [AI_Status_web.md](AI_Status_web.md) §二（链路证据 + 能力与接线）· **官方 API 9 条** → [AI_Status_api.md](AI_Status_api.md) §二（协议与就绪 + 证据与动作）。
> **合并一览**（20 条：类型 / S2 探测 / S3 rc-n3 / 最终态 / 下一动作）= [loop_dev.md](loop_dev.md) §2.2 哨兵块。
> 下面汇总行与两份分片、§2.2 由 `loop_verify.ps1` **同一次调用**写入 ⇒ **同源、不会漂移**；分片依据 = `kind`（`web` × 11 / `official` × 9）。

<!-- SITE-STATUS:BEGIN （TST-L05 机器生成 · 人工只审阅 · 改数据请改 providers.json / site_capability_evidence.json） -->
> 生成：`loop_verify.ps1`（TST-L05）· 时间 2026-10-24 · 覆盖轮次 0：
> **分通道明细**（按 `kind` 分片 · 同一次调用写入 ⇒ 与本文汇总同源）：**网页版** `kind=web` **11** 站 → `docs/AI_Status_web.md` §二 · **官方 API** `kind=official` **9** 条 → `docs/AI_Status_api.md` §二
> **汇总**：共 20 条（web 11 / official 9）· 文字理解 ✅ 实测 **9** · 图片理解 ✅ 实测 **3** · 🟡 就绪/声明 6 · ⬜ 未接/未声明 9 · ⚠ 声明不一致 0
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
2. **官方 API 线**（[AI_Status_api.md](AI_Status_api.md) §二 表 B）：`deepseek` 回填 `providers.json` 的 `capabilities.vision`（`M7-10`）；`custom-official` 填 `api_base` + Key；`openrouter` / `openai` / `ollama` / `siliconflow` 有 Key 时真机一次；**`anthropic` / `gemini` 已于 2026-10-11 批 6 实现（`PB2-10` / `PB2-11`）** ⇒ 有 Key 时用 `aiwrite.exe --provider-selftest --provider anthropic|gemini` 各跑一次真机 `ping`（打印「协议 / 实现 / HTTP / 耗时」）。
3. **循环侧**：把「探测态 → S3 → 最终态」的站数从 1 站逐步扩到全量（当前 `SMOKE*` 均为单站烟测；正式轮次仍以 §1 授权为准）。
4. ✅ **已提交（2026-10-10 · 单次提交 `486c30a`「docs update」· 32 文件 · `+2637 / −104`）** —— 原「未提交」记录（提交前）：批 9（`TST-L05` 台账 + 本轮门禁修复 + 本轮**分通道拆分**）**尚未 commit**（2026-10-10 追加**站点条目去重**：移除与 `qwen-web` 重复的 `tongyi-web` ⇒ 条目 21 → **20 条**（web 12 → 11）· `testAllAI.json` 85/84 → **82/81** · 台账 20 站）—— ✅ **已由开发者提交（2026-10-10 · `486c30a`）**。本批一次提交覆盖：批 9 + 站点去重（21 → **20 条**）+ `PB2-24`（第 5 条）+ `PC-01`/`PC-02`（第 6 条）+ 文档可信度收口；后续提交按 [../AI_README.md](../AI_README.md) §6 **由开发者决定**。
5. **`PB2-24` 自建站点闭环**（2026-10-10 · ✅ **已随 `486c30a` 提交**）：代码 + 离线断言已落地（`api_probe --site-assert` **PASS** · `check_all.ps1` **14/14**（本项落地时 13/13）· 构建 0/0），**⛔ 界面点选回路未人工实测** ⇒ 待真人做一次：「文件 → 设置…」→ 分区⑥ → 「新建网页版站点…」→ 创建 → 下拉出现 → 选中 → 「打开登录窗口」；细节见 [CHANGELOG.md](CHANGELOG.md) `[Unreleased]`「`PB2-24`」条目。同批**未做** = 面板「测试选择器」（`PB2-30②`）。
6. **`PC-01` / `PC-02` 自动保存 + 崩溃恢复**（2026-10-10 · ✅ **已随 `486c30a` 提交**）：纯逻辑 `engine/autosave.*` + `ui/editor_state.*` 节流 + `ui/app.cpp` 恢复询问；`api_probe --autosave-assert` **28/0** · `--settings-assert` 越界 11 例 · `check_all.ps1` **14/14** · 构建 0/0；GUI 实测（真机）= 启动「发现快照 / 将询问…」日志出现 + **12 秒后快照哈希不变**（启动不冲掉真快照）。**⛔ 仍需真人点一次鼠标**：「改一个节点参数（或拖动节点）→ 静默 3 秒 → `~/.brain-ai/snapshots/autosave.json` 出现/更新 → 重启 → 弹窗点『恢复』→ 画布内容一致（状态栏『已恢复未保存的更改』）」。细节 + 陷阱 10（`json::operator+=` = `push_back`）见 [CHANGELOG.md](CHANGELOG.md) `[Unreleased]`「`PC-01` / `PC-02`」条目与 [loop_dev.md](loop_dev.md) §10.1。
7. **`M_patchAB` 批 6 · L2 底座**（2026-10-11 · ⛔ **未提交**）：`PB2-04`…`PB2-12` 七项全部落地（`ai/inference_provider.*` · `ai/provider_factory.*` · `ai/http_json.*` · `ai/anthropic_provider.*` · `ai/gemini_provider.*` · 节点能力驱动接线 · `provider_spec.h` 嵌套 schema 注释）；**离线全绿**（`--selftest` **十三组**含 V-13 23/0 · V-14 13/0 · V-15 11/0 · `--exec-selftest` **546/0** · `--provider-selftest` **52/0** · `check_all` **15/15**）。**下一批顺序**：① `PD-06①` 通用重跑（该节点 / 该节点及下游）→ ② **真机批**（一次登录把 `PW-*` 端到端 · `M8-14` · `VB-02` GUI 停止 · `VB2-20` 真实失效响应 · 面板两个新按钮 · `deepseek-flash` 视觉复跑 · `anthropic`/`gemini` 真机 ping · 企业网络代理/自签 一起做完）。**⛔ 需真机 / 有 Key**：`anthropic` / `gemini` 真实生成；流式仍归 `PB-03` 暂停项。
8. **`M_patchAB` 批 7 · 代理 / 自签证书 / 凭据库配置**（2026-10-11 · `PB-06` 剩余 · ⛔ **未提交**）：`ai/http_net.*`（优先级 = `NO_PROXY` → `[network].proxy` → `HTTPS_PROXY`/`HTTP_PROXY` → `ALL_PROXY`；`[network].no_proxy` 覆盖环境；**默认严格校验**证书，`insecure_tls` 唯一开关；口令打码）+ 两条 HTTP 路径接线 + `[network]` / `[credentials]` 两节（凭据目录 / TTL / 后端全链路生效；未实现后端**明确报错 + 零文件留痕**）+ 设置面板两节（并修掉「暂不生效」过期标签）。**离线全绿**：V-16 **16/0**（含**代理回环证明**：目标域名不存在仍经代理成功、服务器侧回读绝对 URI；反向「直达 ⇒ 代理收不到请求」）· `--settings-assert` **越界 15 例 + 边界值 3 项** · `--diag-assert` **40 行** · `check_all` **15/15** · 断链 **1184/0**（本批入闸抓到 1 条真断链并修掉）。**⛔ 需真机**：企业网络真实 `HTTPS_PROXY` / 自签站点 + 面板点选（与真机批同批做）。
9. **节点扩展 L2 + L3**（2026-10-15 · ✅ **已提交**（开发者 2026-10-15 · `38b0a46`「minimap & L2 implement」））：L2 六个文本处理节点（TextSplit / TextExtract / TextClean / TextReplace / TextStats / TextDiff · 新分类「辅助功能」）+ L3 五项 UX（节点搜索 / 小地图 / 自动吸附 / 对齐辅助线 / 连线交互提示）全部落地；`--exec-selftest` **655/0** · `--graph-selftest` **151/0** · `check_all` **15/15** · 断链 **1487/0**；GUI 冒烟通过；**同日修正：L3 小地图 = 纯导航**（查看/跳转，不编辑节点 —— `ed::NavigateToCenter` 视口平移 + 区域输入隔离，废弃旧 `CenterNodeOnScreen` 平移节点坐标行为）；**同日再修正：L3 对齐线收口**（用户澄清：网格保留 —— 恢复 16px 网格吸附 + `ui.show_grid` 默认 true；实际删除的是每个节点标题下方的灰色 Separator 线）；**跟进：节点内条件分隔线 clamp 到内容区**（`ImGui::Separator()` 按画布窗口宽度画线 ⇒ 新增 `draw_node_separator()` 自绘 `g_content_width` 宽，无一条超出节点框）。**⛔ 待真人**：L3 交互走查（搜索 / 小地图 / 吸附对齐 / 连线提示）。
10. **`Composition_node` 实施批 · 上级节点（Composite Node · 组合层 L4 重启）**（2026-10-16 · ⛔ **未提交**）：模型/注册表/独立文件存储/展开执行 + 编辑器（打包·新建·独立标签页·撤销重做）+ 节点库「上级节点」分类 + 紫条视觉 + 新建草稿保存修正（`allow_incomplete_draft`）+ `composite_selftest` **51/0** 并入 `--exec-selftest`（构建 0/0 · exec **681/0** · graph **151/0** · 断链 0）· 📦 **2026-10-18 设计文档已归档（指令式 · Archive/README §三 5）** —— ⏳ **人工验收 + 待拍板项已迁 [DevPlan.todo](DevPlan.todo) Test**（2026-10-21 由 `ToVerify.todo` 并入 · 新建 → 编辑 → 打包 → 父画布运行 → 展开执行结果正确；待拍板 = ① `VCN-*`/`ACN-*` 验收框架 ② 调试设施去留 ③ 程序化生成规则）。—— **2026-10-19 复测**：上级节点自检 **66/0**（51 + 15 双源 / 空草稿实例化回归断言）· exec **682/0** · graph **151/0**。

## 五、纪律（别踩）

1. **改数据只改两处**：`source/assets/providers.json`（声明 / 接线）× `source/assets/site_capability_evidence.json`（实测证据）⇒ **四个落点**（[loop_dev.md](loop_dev.md) §2.2 合并一览 · 本文汇总 · [AI_Status_web.md](AI_Status_web.md) §二 · [AI_Status_api.md](AI_Status_api.md) §二）由脚本**同一次调用**重建，**不要手改表格**。
2. **声明 ≠ 实测**：`capabilities.vision` / `verified` / `web.upload_adapter` 是「声明」，只有 S3 / 上传双证据才算「实测」（§10.1 陷阱 7）。
3. **看结论先看物证**：`build/**` 是易失过程产物 ⇒ 结论必须落到 CHANGELOG + 本文档（+ 对应分片：web / api）+ [loop_dev.md](loop_dev.md) §11（**只写报告不算完成**）。
4. **⛔ 不擅自 `commit` / `tag` / `push` / 改版本号**（[../AI_README.md](../AI_README.md) §6）。
