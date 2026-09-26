# 文档归档（Archive）

> 本目录存放**已完成**的文档：里程碑 Action Plan、功能补丁计划、验证报告。
> 归档原则：**只增不改** —— 归档后内容保持原样（仅允许修正链接与补归档标记）；当前计划与任务看板见 [../actionPlan/](../actionPlan/) 与 [../DevPlan.todo](../DevPlan.todo)。
> 首次归档：2026-09-26

---

## 一、归档索引

| 文档 | 类型 | 完成 | 关键结论 |
|---|---|---|---|
| [actionPlan/M1.md](actionPlan/M1.md) | 里程碑计划 | 2026-09-22 | 技术验证 + 项目骨架；V-01~V-08 / A-01~A-09 全部通过 |
| [actionPlan/M2.md](actionPlan/M2.md) | 里程碑计划 | 2026-09-23 | 核心工作流引擎（M2-01~07 全绿）：数据结构 / 节点注册表 / 拓扑排序 / 执行器 / JSON 序列化 / 三级校验 / 撤销重做 |
| [actionPlan/M_textio.md](actionPlan/M_textio.md) | 功能补丁 | 2026-09-25 | 文本输出双职能：最终输出预览（置顶 + 高亮 + `label` 生效）与「导出为文档」（原子写、同名自动改名、导出路径可配置） |
| [actionPlan/M_rerun.md](actionPlan/M_rerun.md) | 功能补丁 | 2026-09-25 | 文本生成「重新生成（新 seed）」；§6 记录 `SliderInt` 范围越界崩溃（ImGui 半范围限制）的根因与修复约定 |
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
| [../actionPlan/M3.md](../actionPlan/M3.md) | `FEA-M3-07` 工作流变体保存待做（其余已完成） |
| [../actionPlan/M4.md](../actionPlan/M4.md) | 官方 Provider / 凭据 / 线程化已落地；流式（`PB-03/08`）暂停、`FEA-M4-11` 重跑待做 |
| [../actionPlan/M_patchA.md](../actionPlan/M_patchA.md) | Patch B 进行中（`PB-07` 待做）、Patch C / D 未开工 |
