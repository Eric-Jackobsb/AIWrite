# 文档归档（Archive）

> 本目录存放**已完成**的文档：里程碑 Action Plan、功能补丁计划、验证报告。
> 归档原则：**只增不改** —— 归档后内容保持原样（仅允许修正链接与补归档标记）；当前计划与任务看板见 [../actionPlan/](../actionPlan) 与 [../DevPlan.todo](../DevPlan.todo)。
> 首次归档：2026-09-26；**最近归档：2026-10-23**（**文档整理批次 `ARC-AR-02` · 7 份 actionPlan 一次性入库**：`M_code_cleanup`（`CLN-01…15` 勾账 + `CLN-12…15` 拆分落地 + 驼峰化 8 批）· `M_patchAB_rest`（v11 文档收口 · 决策 `D-25`~`D-32` 8/8）· `M_patchC_web_node_bind`（v8 文档收尾 `PW-12` ✅）· `M_canvas_snapshot`（`CS-10` 收口）· `M_8nodes_extension`（L1 六批 + 人工验收）· `M_node_expansion_part2`（四层定稿 + L1/L2/L3 实施完成）· `M_node_expansion`（v6 讨论稿 · 被 part2 取代）；残项**统一承接位 = [../actionPlan/M_polish.md](../actionPlan/M_polish.md)**（四阶段路线，见其 §4 承接表）+ [../audit/DEBT.todo](../audit/DEBT.todo)；各件加 `📦 已归档` 标记行 + 承接位 + 相对链接层级修正（正文未改））；前一批 **2026-10-18**（**`Composition_node.md` 指令式归档** —— 实施完成、仅剩人工验收 ⇒ 按 §三 5 直接归档；人工验收 + 待拍板项迁 [../DevPlan.todo](../DevPlan.todo) Test（2026-10-21 由 `ToVerify.todo` 并入）；承接位 = `DevPlan.todo Test` + [../draft/M_preset_library.md](../draft/M_preset_library.md)（组合层落地 PS-01…））；上一批 **2026-10-11**（**M4 / M5 / M7 三份里程碑计划 `git mv` 入库** —— M4「文本生成链路」核心完成、M5「多模态 · M5-C 核心切片」完成、M7「图片输入收口 + P7-a」固定；三份均加 `📦 已归档` 标记行 + 相对链接层级修正 + 7 处索引同步）；再上一批 **2026-10-10** = `网页版协议实测记录.md`（**停止维护**：M4/M7 时期的网页版协议取证（DeepSeek 取值 · 豆包 / Kimi 站点证据）冻结，现行「站点能不能用」的结论改由 `TST-L05` 台账（[../AI_Status.md](../AI_Status.md) §二）+ [../loop_dev.md](../loop_dev.md) 承载）；再上一批 **2026-10-05** = `M7B` · `M7_P7b_pydollRoute`，**网页通道路线退场**：`M7B` 的 Pydoll 路线被 **方案 E** 取代，现行计划 [../actionPlan/M8.md](../actionPlan/M8.md)）

---

## 一、归档索引

| 文档 | 类型 | 完成 | 关键结论 |
|---|---|---|---|
| [actionPlan/M1.md](actionPlan/M1.md) | 里程碑计划 | 2026-09-22 | 技术验证 + 项目骨架；V-01~V-08 / A-01~A-09 全部通过 |
| [actionPlan/M2.md](actionPlan/M2.md) | 里程碑计划 | 2026-09-23 | 核心工作流引擎（M2-01~07 全绿）：数据结构 / 节点注册表 / 拓扑排序 / 执行器 / JSON 序列化 / 三级校验 / 撤销重做 |
| [actionPlan/M3.md](actionPlan/M3.md) | 里程碑计划 | 2026-09-27 | **节点编辑器 UI**（画布交互 · 复制/粘贴 · 参数搜索与批量应用 · 视图与快捷键）；**唯一待办 `FEA-M3-07` 工作流变体保存** ⬜ **已拍板「延后 · 不排期」（`PQ-19`，2026-10-09）** ⇒ 承接位 [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) §7 |
| [actionPlan/M4.md](actionPlan/M4.md) | 里程碑计划 | 2026-09-26 | **文本生成链路**（`M4-01`~`M4-13` 核心链路：双模式 LLM Generate / 官方 API + 参数透传 / DPAPI 凭据库 / 网页版 PoW + 定制 SSE / 输出回流 + Console + `workflow.log`）—— **核心已完成**（`A-04` 流式数据源 🚫 暂停）；**残项统一归** [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) §1–§3 |
| [actionPlan/M5.md](actionPlan/M5.md) | 里程碑计划 | 2026-09-26 | **多模态 + 输出（M5-C 核心切片）**：图片理解链路（official / OpenAI 兼容多模态 · 智谱实测入口 `--vlm-selftest`）+ 图片显示（纹理缓存 + 输出面板缩略图）+ 示例 E-02 + 输出归档 `PC-05`；留后项（`M5-09` = `PD-07` 📦 不执行 / `M5-10` 图片复制导出 ⏸ 不排期 / 按日期归档 `PC-06` / 示例 E-03·E-05·E-06 ⏸）承接位见该文头部 |
| [actionPlan/M_textio.md](actionPlan/M_textio.md) | 功能补丁 | 2026-09-25 | 文本输出双职能：最终输出预览（置顶 + 高亮 + `label` 生效）与「导出为文档」（原子写、同名自动改名、导出路径可配置） |
| [actionPlan/M_rerun.md](actionPlan/M_rerun.md) | 功能补丁 | 2026-09-25 | 文本生成「重新生成（新 seed）」；§6 记录 `SliderInt` 范围越界崩溃（ImGui 半范围限制）的根因与修复约定 |
| [actionPlan/M_patchA.md](actionPlan/M_patchA.md) | 地基补丁（第一期） | 2026-09-27 | **Patch A 全部完成**（结果回流与可观测性 + 配置访问器地基：运行信息 / 输出面板 / 结果呈现 / Console 复制导出 / 错误条 / 配置治理）；含 Patch B/C/D/PM 的历史规格（**未完成部分已迁出** → [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md)） |
| [actionPlan/M_patchB.md](actionPlan/M_patchB.md) | 地基补丁（第二期） | 2026-09-27 | **Provider 可插拔化**：JSON 配置表（API 与网页版**同表同机制**）+ 用户覆盖层 + **L1 收口**（去硬编码 / 多站点会话 / `config.toml` 多 provider / `--provider-selftest`）+ **L3 通用 DOM 站点适配器** + **L4 部分落地**（`PB2-27`/`PB2-28`/`PB2-30①`/`PB2-31`）；含 `PB2-*` 任务与各批次实测记录（**未完成部分已迁出** → 承接文档 §3） |
| [actionPlan/M7.md](actionPlan/M7.md) | 里程碑计划（**两轮**） | 2026-09-27 / 2026-09-28 | **第一轮 = 图片输入收口 + 节点精简**（删图片输出节点 9→8 · 图片格式**按内容嗅探** · stb/WIC 双后端 · 可操作报错 · CRT 断言崩溃（退出码 3 / `0x80000003`）根因与冻结区 `I17`）；**第二轮 P7-a（v0.5.2）= 图片内部运行 + official 收口 + UI A 档**（✅ **现行契约**）；**P7-b（v0.5.3）📦 已失效**（Pydoll 路线 ⇒ 现行 = [../actionPlan/M8.md](../actionPlan/M8.md) 站点上传模板）—— 全量判定见该文头部 §0.0（引用规则：只读契约 + 决策证据） |
| [actionPlan/M7B.md](actionPlan/M7B.md) | 里程碑计划（第三轮 · **未完成即退场**） | 2026-10-05 | **网页通道整体迁 Pydoll**（WebView2 退场 · Python 守护进程 + 命名管道 · 协议词表 v4 · 登录态 L1~L4）—— **于 2026-10-05 被方案 E 取代**（文字生成**回退 WebView2** · 图片上传改走 **C++ native HTTP** · Python / 管道 / 守护进程退场 —— 📦 **2026-10-11 改判：栈休眠保留 · 不删**）。**用途 = 决策追溯 + 资料取用**（协议词表 §6.1 / 站点清单与选择器回填 §10 / 各批实测结论）；**内容取用终点 = `git cdf6d40`** |
| [actionPlan/M7_P7b_pydollRoute.md](actionPlan/M7_P7b_pydollRoute.md) | **摘录归档**（`M7.md` 的 Pydoll 章节） | 2026-10-05 | `M7.md` §9 `D5`/`D6` · §12.1 全表（含 B0~B3 实测结论）· §12.2 通道类条目 · §16 旧实施顺序 · §17 `Q2`/`Q3` 的**原文照录**（`M7.md` 正本原位保留 + 📦 指针） |
| [M1_技术验证报告.md](M1_技术验证报告.md) | 验证报告 | 2026-09-23 | M1 实测环境、分层验证结果、V/A 验收对照、问题与解决记录 |
| [网页版协议实测记录.md](网页版协议实测记录.md) | 实测记录（**停止维护**） | 2026-10-10 | 网页版协议取值取证（M4-06/08/09 · DeepSeek 取值 / 豆包真视觉 / Kimi 站点证据）；**方案 E 取代后不再维护** ⇒ 只作**追溯资料**（新实测证据改记 `source/assets/site_capability_evidence.json` + [../AI_Status.md](../AI_Status.md) §二 台账） |
| [actionPlan/Composition_node.md](actionPlan/Composition_node.md) | 架构设计（**指令式归档**） | 2026-10-18 | **上级节点（Composite Node · 组合层 L4 重启）**：设计定稿 + 实施完成（模型/注册表/独立文件存储/展开执行/编辑器/节点库分类/紫条视觉/新建草稿修正 · `composite_selftest` 51 断言 · exec 681/0 · graph 151/0）—— **仅剩人工验收** ⇒ 按 §三 5 指令式归档；**人工验收 + 待拍板项 → [../DevPlan.todo](../DevPlan.todo) Test**（2026-10-21 由 `ToVerify.todo` 并入 · 新建 → 编辑 → 打包 → 父画布运行 → 展开执行结果正确；待拍板 = `VCN-*`/`ACN-*` 验收框架 · 程序化生成规则 · 调试设施去留）；承接位 = `DevPlan.todo Test` + [../draft/M_preset_library.md](../draft/M_preset_library.md) |
| [actionPlan/M_node_expansion.md](actionPlan/M_node_expansion.md) | 讨论稿（**v6 · 已被取代**） | 2026-10-12 | 四层「**定向**」稿（L1 实施未开始时的版本）⇒ **被 [M_node_expansion_part2.md](actionPlan/M_node_expansion_part2.md) 取代**，只作决策追溯（`NE-Q*` / `NE-D1`~`NE-D29` / `NE-D38`~`NE-D39` · 四层拆解与约束面取证 §2/§3） |
| [actionPlan/M_8nodes_extension.md](actionPlan/M_8nodes_extension.md) | 功能计划（**L1 层**） | 2026-10-14（+ 人工验收 2026-10-15） | **8 节点深化 + 4 项横向能力**（公共参数面 / 参数二级分组 / 超时+重试 / 网页版参数「忽略」标注）；`L1-a`~`L1-f` 六批落地 · `VL1-01`…`VL1-12` 全过（graph **110 → 144 / 0** · exec **546 → 598 / 0**）· `AL1-01`…`AL1-06` 用户验收（[../audit/L1_checklist.todo](../audit/L1_checklist.todo)）；现行契约承接 = [../节点编辑器使用说明.md](../节点编辑器使用说明.md) §4 |
| [actionPlan/M_node_expansion_part2.md](actionPlan/M_node_expansion_part2.md) | 实施依据（**四层定稿**） | 2026-10-15 | **节点扩展四层全部定稿 + L1/L2/L3 实施完成**：L1 = 8 节点深化（[M_8nodes_extension.md](actionPlan/M_8nodes_extension.md)）· L2 = **6 个文本处理节点**（`TextSplit` / `TextExtract` / `TextClean` / `TextReplace` / `TextStats` / `TextDiff` · `VL2-01…VL2-07` 进 `--exec-selftest` 546 → 655）· L3 = **5 项 UX**（节点搜索 / 分类折叠 / 小地图 / 对齐辅助 / 连线交互提示）· L4 🔴 不做 · 引擎议题（条件分支 / 循环 / 子工作流）**不排期**；**= 本系列定稿正本** |
| [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) | 补丁系列**剩余项总集** | 2026-10-15（v11 收口） | Patch A/B 残项 + C 数据安全 + D 交互打磨 + PM + `B2` 残项；**决策 `D-25`~`D-32` 8/8 结案 · 待定 0**；附录 B–E（JSON 规范 / 加载顺序 / 自建站点 / 站点清单）**仍为现行规格**；**残项承接** = [../actionPlan/M_polish.md](../actionPlan/M_polish.md) 阶段一~三（`POL-L*` / `POL-UX*` / `POL-C*`） |
| [actionPlan/M_patchC_web_node_bind.md](actionPlan/M_patchC_web_node_bind.md) | 功能补丁（**第三期**） | 2026-10-15（v8） | 网页版「**节点 ↔ 窗口**」绑定与页面就绪判据：`PW-01`…`PW-13` · 不变量 `I24`~`I26` · 决策 `D-33`~`D-36`（独立窗口真并行 / `answer_selector` 可选 / 复位首次+失败 / `web.attach` 诊断字段）；**`PW-12` 文档收尾 ✅**；余 `PW-10` / `PW-11` → [../actionPlan/M_polish.md](../actionPlan/M_polish.md) 阶段二 |
| [actionPlan/M_canvas_snapshot.md](actionPlan/M_canvas_snapshot.md) | 跨里程碑**性能专项** | 2026-10-15（`CS-10` 收口） | **画布渲染性能**（高分辨率卡片快照替代实时绘制）：`CS-01` 指标行 ✅ + 计划外 **P0 根因热修**（纹理缓存容量 8 < 同屏 9 张 ⇒ **FPS 5~6 → 59~60** · 解码 9.0 → 0.0 次/帧）· `CS-02`/`CS-03` ⏸ 不排期 · `CS-04`…`CS-08` 📦 不执行 · `CS-09` ⏸ 不再适用 · `CS-10` ✅ 文档收口；**现行实测口径** = [../节点编辑器使用说明.md](../节点编辑器使用说明.md) §10.12 |
| [actionPlan/M_code_cleanup.md](actionPlan/M_code_cleanup.md) | 工程清理计划 | 2026-10-20 | **代码库清理 `CLN-01`…`CLN-15`**：死代码扫描（无删除项）/ 冗余 include / 失效分支 / TODO 台账 / 重复辅助函数收敛 5 组 14 处（→ `utils/string_*`）/ `CLN-06` 扩项**全库驼峰化 8 批**；**超大文件拆分 `CLN-12`~`CLN-15` 全部落地**（`property_panel` 1932 → 814 行 · `composite_editor` 1687 → 172 · `main.cpp` 2359 → 661 · `api_probe` 6937 → 321）；审计 15 份见 [../audit/](../audit/)；**承接** = [../actionPlan/M_polish.md](../actionPlan/M_polish.md) 四阶段（§4 方向 → `POL-*`）+ `POL-L-02`（`mode 5` 小地图 GUI 复核）+ [../audit/DEBT.todo](../audit/DEBT.todo) |


---

## 二、为什么归档（而不是删除）

1. **验收依据**：里程碑验收对照表（`A-xx` / `V-xx`）需长期可查；
2. **决策追溯**：如 M_textio 的"不新增节点类型"决议、M_rerun 的 seed 语义，都是后续改动的约束来源；
3. **踩坑记录**：M1 报告与 M_rerun §6 沉淀了环境与库限制（CMake 与旧 vcpkg 端口、`httplib` 无 POST 流式接收重载、ImGui `SliderInt` 半范围、Windows 目录迭代器删除时机等）。

---

## 三、归档规则

1. **触发**：文档所述阶段**全部任务完成且回归通过**（含人工确认项）；
2. **动作**：`git mv` 到 `docs/Archive/` 下**镜像原路径**（`docs/X` → `docs/Archive/X`），保留 git 历史；
3. **必须同步**：① 顶部加 `📦 已归档` 标记行；② 状态行改为 ✅；③ 修正相对链接层级（归档后在更深一层）；④ 更新索引 —— [../DocMAP.md](../DocMAP.md)、[../CHANGELOG.md](../CHANGELOG.md)、[../DevPlan.todo](../DevPlan.todo)、[../../source/README.md](../../source/README.md)；
4. **归档后修订范围**：仅链接修正与归档标记；后续变更写入当前计划文档（`actionPlan/M*.md`）与 [../CHANGELOG.md](../CHANGELOG.md)。
5. **指令式归档（2026-10-10 新增口径）**：开发者明确要求「**该文档不再维护**」时，可按本页规则**直接归档**（不必等 §三 1 的「阶段全部收尾」）—— 归档后其结论的**承接位**必须同时指定（例：`网页版协议实测记录.md` ⇒ 承接位 = `source/assets/site_capability_evidence.json` + [../AI_Status.md](../AI_Status.md) §二（总览 ⇒ 分通道明细 [../AI_Status_web.md](../AI_Status_web.md) / [../AI_Status_api.md](../AI_Status_api.md)）+ [../loop_dev.md](../loop_dev.md)）。

---

## 四、待归档候选（尚未满足条件）

| 文档 | 阻塞条件 |
|---|---|
| [../actionPlan/M6.md](../actionPlan/M6.md) | v1.0 收口未完成（第一批已落地 2026-10-10：`M6-01` / `M6-02` / `M6-08` 骨架 / `M6-09` / `M6-10`；`M6-03`~`M6-07` 未完成 —— `M6-04`/`M6-05`/`M6-06`/`M6-10` 已归 `POL-UX*` / `POL-C*`，`M6-07` 渲染优化随本批 `M_canvas_snapshot` 收口） |
| [../actionPlan/M8.md](../actionPlan/M8.md) | **Phase 4~6 未做**（~~Phase 3 退场~~ 📦 **已改判 2026-10-11 = 不执行 · 栈休眠保留** / Phase 4 解禁 `M8-21` / Phase 5 防识别 / Phase 6 节点层接线收尾）+ 真机 Gate `TST-M8-01` 未跑 |
| [../actionPlan/M_polish.md](../actionPlan/M_polish.md) | **当前主线**（不算候选）—— 四阶段打磨推进中（阶段一 `POL-L` / 阶段二 `POL-UX` 已细列并落地首项；阶段三 `POL-C` 前两项落地、后两项审计通过；阶段四 `POL-E` 待开工）⇒ **四阶段全部验收通过**后再按本页规则归档 |

> **2026-10-23 说明（本批归档结案 · `ARC-AR-02`）**：原候选行「`M_patchAB_rest`：待 Patch B 残项 / 原 Patch C·D / PM / `B2` 残项全部完成后归档」「`M_patchC`：`PW-02` / `PW-04` / `PW-05` / `PW-06` / `PW-08` / `PW-10` / `PW-11` / `DOC-PW-12` 未完」「`M_canvas_snapshot` · `M_node_expansion`：专项 / 讨论稿（`CS-02`~`CS-10` 计划中 · 四层未定向）」**已随本批归档结案** —— 四件的收口结论与承接位见 §一 索引表末七行；同批另入库 `M_code_cleanup` · `M_8nodes_extension` · `M_node_expansion_part2`（原未列入候选，但已成「全部完成 + 人工验收 / 文档收口」）。三项残项**统一承接位 = [../actionPlan/M_polish.md](../actionPlan/M_polish.md) §4 承接表**（`PD-*` → `POL-UX*` · `PW-10`/`PW-11` → `POL-UX*` · `§4 优化方向` → `POL-E*`）+ [../audit/DEBT.todo](../audit/DEBT.todo)。

> **2026-10-11 说明**：原候选行「`M4`：官方 Provider / 凭据 / 线程化已落地；流式（`PB-03/08`）暂停、`FEA-M4-11` 重跑待做」与「`M7`：余下 `P7b-12`/`13`/`14`/`16` 完成后可归档」**已随本批归档结案** —— `M4` 残项（含流式）统一归 [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md) §1–§3，`M7` 的 P7-b 全量已由 [../actionPlan/M8.md](../actionPlan/M8.md) 承接；`M5`（M5-C 核心切片）亦同批归档。
>
> **2026-10-11 追加（二）**：`PQ-03`（`RQ-4` · P7-b 内部版本号 `v0.5.3` 是否留用）= ✅ **留用** ⇒ **归档件 [actionPlan/M7.md](actionPlan/M7.md) §12 第 184 行的**「⬜ 待定（`PQ-03`）」**按归档纪律 2「单向」不回改**（该行历史原文保留），结论**只在活文档覆盖**：[../roadmap.md](../roadmap.md) 轨道 A 注 + `RQ-4` 行 · [../actionPlan/M8.md](../actionPlan/M8.md) §7 `M8-Q4`（版本号口径 = 沿用 `v0.5.4`）。
> **2026-10-11 追加**：`M8` 的 **Phase 3（退场删除）已拍板改判 = 不执行**（`M8-Q7` = ②：**Pydoll 栈休眠保留 · 不删**）—— 归档件 [actionPlan/M7B.md](actionPlan/M7B.md) 与 [actionPlan/M7_P7b_pydollRoute.md](actionPlan/M7_P7b_pydollRoute.md) 的**资料链保持完整**（这也是改判理由之一）；归档纪律不变（**本目录不回改** —— §三 / §五）。

> **2026-09-27 说明**：原候选行「`M_patchA`：Patch B 进行中、Patch C / D 未开工」**已过期** —— Patch B 早已独立成 `M_patchB.md`，且两份文档的**已完成部分已归档**、**未完成部分已迁入** [actionPlan/M_patchAB_rest.md](actionPlan/M_patchAB_rest.md)（见上表）。

