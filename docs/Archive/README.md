# 文档归档（Archive）

> 本目录存放**已完成**的文档：里程碑 Action Plan、功能补丁计划、验证报告。
> 归档原则：**只增不改** —— 归档后内容保持原样（仅允许修正链接与补归档标记）；当前计划与任务看板见 [../actionPlan/](../actionPlan/) 与 [../DevPlan.todo](../DevPlan.todo)。
> 首次归档：2026-09-26；**最近归档：2026-10-05**（`M7B` · `M7_P7b_pydollRoute` —— **网页通道路线退场**：`M7B` 的 Pydoll 路线被 **方案 E** 取代，现行计划 [../actionPlan/M8.md](../actionPlan/M8.md)）

---

## 一、归档索引

| 文档 | 类型 | 完成 | 关键结论 |
|---|---|---|---|
| [actionPlan/M1.md](actionPlan/M1.md) | 里程碑计划 | 2026-09-22 | 技术验证 + 项目骨架；V-01~V-08 / A-01~A-09 全部通过 |
| [actionPlan/M2.md](actionPlan/M2.md) | 里程碑计划 | 2026-09-23 | 核心工作流引擎（M2-01~07 全绿）：数据结构 / 节点注册表 / 拓扑排序 / 执行器 / JSON 序列化 / 三级校验 / 撤销重做 |
| [actionPlan/M_textio.md](actionPlan/M_textio.md) | 功能补丁 | 2026-09-25 | 文本输出双职能：最终输出预览（置顶 + 高亮 + `label` 生效）与「导出为文档」（原子写、同名自动改名、导出路径可配置） |
| [actionPlan/M_rerun.md](actionPlan/M_rerun.md) | 功能补丁 | 2026-09-25 | 文本生成「重新生成（新 seed）」；§6 记录 `SliderInt` 范围越界崩溃（ImGui 半范围限制）的根因与修复约定 |
| [actionPlan/M_patchA.md](actionPlan/M_patchA.md) | 地基补丁（第一期） | 2026-09-27 | **Patch A 全部完成**（结果回流与可观测性 + 配置访问器地基：运行信息 / 输出面板 / 结果呈现 / Console 复制导出 / 错误条 / 配置治理）；含 Patch B/C/D/PM 的历史规格（**未完成部分已迁出** → [../actionPlan/M_patchAB_rest.md](../actionPlan/M_patchAB_rest.md)） |
| [actionPlan/M_patchB.md](actionPlan/M_patchB.md) | 地基补丁（第二期） | 2026-09-27 | **Provider 可插拔化**：JSON 配置表（API 与网页版**同表同机制**）+ 用户覆盖层 + **L1 收口**（去硬编码 / 多站点会话 / `config.toml` 多 provider / `--provider-selftest`）+ **L3 通用 DOM 站点适配器** + **L4 部分落地**（`PB2-27`/`PB2-28`/`PB2-30①`/`PB2-31`）；含 `PB2-*` 任务与各批次实测记录（**未完成部分已迁出** → 承接文档 §3） |
| [actionPlan/M7B.md](actionPlan/M7B.md) | 里程碑计划（第三轮 · **未完成即退场**） | 2026-10-05 | **网页通道整体迁 Pydoll**（WebView2 退场 · Python 守护进程 + 命名管道 · 协议词表 v4 · 登录态 L1~L4）—— **于 2026-10-05 被方案 E 取代**（文字生成**回退 WebView2** · 图片上传改走 **C++ native HTTP** · Python / 管道 / 守护进程退场）。**用途 = 决策追溯 + 资料取用**（协议词表 §6.1 / 站点清单与选择器回填 §10 / 各批实测结论）；**内容取用终点 = `git cdf6d40`** |
| [actionPlan/M7_P7b_pydollRoute.md](actionPlan/M7_P7b_pydollRoute.md) | **摘录归档**（`M7.md` 的 Pydoll 章节） | 2026-10-05 | `M7.md` §9 `D5`/`D6` · §12.1 全表（含 B0~B3 实测结论）· §12.2 通道类条目 · §16 旧实施顺序 · §17 `Q2`/`Q3` 的**原文照录**（`M7.md` 正本原位保留 + 📦 指针） |
| [M1_技术验证报告.md](M1_技术验证报告.md) | 验证报告 | 2026-09-23 | M1 实测环境、分层验证结果、V/A 验收对照、问题与解决记录 |

---

## 二、为什么归档（而不是删除）

1. **验收依据**：里程碑验收对照表（`A-xx` / `V-xx`）需长期可查；
2. **决策追溯**：如 M_textio 的"不新增节点类型"决议、M_rerun 的 seed 语义，都是后续改动的约束来源；
3. **踩坑记录**：M1 报告与 M_rerun §6 沉淀了环境与库限制（CMake 与旧 vcpkg 端口、`httplib` 无 POST 流式接收重载、ImGui `SliderInt` 半范围、Windows 目录迭代器删除时机等）。

---

## 三、归档规则

1. **触发**：文档所述阶段**全部任务完成且回归通过**（含人工确认项）；
2. **动作**：`git mv` 到 `docs/Archive/` 下**镜像原路径**（`docs/X` → `docs/Archive/X`），保留 git 历史；
3. **必须同步**：① 顶部加 `📦 已归档` 标记行；② 状态行改为 ✅；③ 修正相对链接层级（归档后在更深一层）；④ 更新索引 —— [../README.md](../README.md)、[../CHANGELOG.md](../CHANGELOG.md)、[../DevPlan.todo](../DevPlan.todo)、[../../source/README.md](../../source/README.md)；
4. **归档后修订范围**：仅链接修正与归档标记；后续变更写入当前计划文档（`actionPlan/M*.md`）与 [../CHANGELOG.md](../CHANGELOG.md)。

---

## 四、待归档候选（尚未满足条件）

| 文档 | 阻塞条件 |
|---|---|
| [actionPlan/M3.md](actionPlan/M3.md) | `FEA-M3-07` 工作流变体保存待做（其余已完成；计划已归档，2026-09-27） |
| [../actionPlan/M4.md](../actionPlan/M4.md) | 官方 Provider / 凭据 / 线程化已落地；流式（`PB-03/08`）暂停、`FEA-M4-11` 重跑待做 |
| [../actionPlan/M7.md](../actionPlan/M7.md) | 第一轮（图片输入收口）✅ 已完成、**P7-a（v0.5.2）✅ 已落地**；**P7-b 的通道底座已改归 [../actionPlan/M8.md](../actionPlan/M8.md)**（方案 E）⇒ 余下 `P7b-12`/`13`/`14`/`16` 完成后可归档 |
| [../actionPlan/M_patchAB_rest.md](../actionPlan/M_patchAB_rest.md) | **当前活跃的补丁系列计划**（不算候选）—— 待其中的 Patch B 残项 / 原 Patch C·D / PM / `B2` 残项**全部完成且回归通过**后，再按本页规则归档 |

> **2026-09-27 说明**：原候选行「`M_patchA`：Patch B 进行中、Patch C / D 未开工」**已过期** —— Patch B 早已独立成 `M_patchB.md`，且两份文档的**已完成部分已归档**、**未完成部分已迁入** [../actionPlan/M_patchAB_rest.md](../actionPlan/M_patchAB_rest.md)（见上表）。
