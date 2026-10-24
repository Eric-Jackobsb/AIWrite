# audit_arch_e6：ARCH-E6 落地审计报告（打包下沉 · 打包-校验同源 · 修暴露节点保存）

> 批次：`ARCH-E6` · 日期：2026-10-23 · 计划：docs/refactor/ARCHITECTURE.md §2.1「打包-校验不同源」/ §3 阶段 2 E6
> 性质：**编辑侧事件化第六批** —— 主画布「打包为上级节点」的规格构建从 UI 层（`ce_pack.cpp`）**下沉**为 engine 单一函数 `buildCompositeSpecFromSelection`（composite.h/.cpp），生成阶段落实「暴露输入不得有内部入边」并**生成后严格校验**（先 `validate_composite_spec` 再落盘）—— 打包与校验**同源**。
> 判据：§4 验收判据 4 —— `ce_pack.cpp` 不再直接写 `spec.internal_graph` / `spec.inputs` / `spec.exposed_params`（`rg` 验证为 0 次）；`composite_selftest` 只升不降；构建 0 error / 0 warning；`check_all` 全 PASS。

## 1. 背景与根因（承接 audit/audit_architecture_20261021.md）

`audit_architecture_20261021.md` **§4.2 问题 2「上级节点存在暴露节点时无法保存」**：

- **根因**：`compositePackSelection` 打包时的暴露输入识别（判据 = **有选区外入边**）与 `validate_composite_spec` 的校验（判据 = **暴露输入端口不得有内部入边**）**对同一概念给出不同约束**。打包可生成「暴露输入同时带内部入边」的规格；保存时该规格被自己的校验拒绝（「暴露输入…对应的内部端口已有内部连线」）。
- **何时发生**：`Graph` 输入端口默认单入边（graph.cpp `occupied && !is_variadic`），唯 **variadic 端口**（如 `VLMGenerate.image`）可同时有选区内入边 + 选区外入边 ⇒ 选区边界端口被暴露、同时内部图带指向它的内边 ⇒ 非法规格。
- **§5 建议的 P0 解法**（本批落实）：**打包-校验同源** —— 把「暴露输入不得有内部入边」等规格约束放进 engine 单一函数（打包生成阶段自动修正），打包不再手工拼装。

## 2. 改动清单

### 2.1 新增 `engine::buildCompositeSpecFromSelection`（composite.h / composite.cpp）

- **签名**：`bool buildCompositeSpecFromSelection(const Graph& source, const std::vector<std::string>& selected_nodes, const std::string& name, CompositeSpec* out, std::string* error)`。
- **组装语义与 ce_pack.cpp 旧逻辑逐条对应**：
  - `internal_graph` = 选中节点（深拷贝，保持 id 与坐标；`state=Idle` + `error_message` 清空）+ 选区内部连线（两端都选中）+ `refreshSequences()`。
  - `inputs` = 选区边界输入端口（有来自**选区外**的入边；`display_name/type` 取自内部端口）。
  - `outputs` = 边界输出端口（有到**选区外**的出边）；无边界输出时 fallback 取选区第一个输出端口；再无可报错「选区内没有任何可对外输出的端口」。
  - `exposed_params` = 选中节点全部**非密钥**参数（`is_secret` 跳过；外部参数 id = 内部 param_id，重复取首）。
  - `name` 空 → 「组合节点」。
- **生成阶段自动修正**（问题 2 修复）：收集暴露输入端口集合后，**指向暴露输入端口的选区内入边不纳入内部图**（`targets_exposed_input` 检查）—— 外部输入在展开时接管该端口；产物必然通过 `validate_composite_spec`（与校验**同源**）。
- 拒绝场景与旧文案一致：`<2 节点` → 「打包需要至少选中 2 个节点」；选区内含上级节点 → 「选区内不能包含上级节点（不支持嵌套）」；无输出 → 「选区内没有任何可对外输出的端口」。

### 2.2 `ce_pack.cpp` 瘦身（150 行 → 45 行）

- `compositePackSelection` 改为：调用 `engine::buildCompositeSpecFromSelection` → **生成后严格校验**（`validate_composite_spec`，失败报「打包生成的规格未通过校验：…」）→ `makeId()` → `save` → `compositeEditorOpen`。
- 删除手工拼装（`internal_graph / inputs / outputs / exposed_params` 直写 **0 次**）；`<algorithm>` / `<vector>` include 移除。
- 行为等价：错误文案、保存路径、打开编辑器顺序与旧逻辑一致（校验失败文案前缀「打包生成的规格未通过校验」为新增可读错误，仅出现在旧逻辑会失败的新路径上）。

### 2.3 `composite_selftest.cpp` 新增「打包」断言 21 条（节 11）

- 11.1 基础打包（选区 {TextInput, PromptTemplate}）：内部图 / 内部边 / 无暴露输入 / 边界输出 = pt.text / 暴露参数非空且**不含密钥 api_key** / **产物通过严格校验** / 运行态清零（Running+error → Idle 无错误）。
- 11.2 暴露输入识别（选区 {PromptTemplate, TextOutput}，TextInput 在选区外）：pt.vars 成为暴露输入且内部图无入边 / 产物通过严格校验。
- 11.3 **variadic 冲突自动修正**（问题 2 核心）：`ImageInput(选区内) → VLMGenerate.image` + `ImageInput(选区外) → VLMGenerate.image` ⇒ 打包成功、暴露输入 = vlm.image、**指向它的选区内入边被剔除**、边界输出 = vlm.text、**通过严格校验**、**可直接 save**（修暴露节点保存）。
- 拒绝：少于 2 节点。
- 前置：`composite_selftest.cpp` 补 `#include <algorithm>`。

## 3. 验证结果

| 验证 | 结果 |
|---|---|
| `cmake --build` | ✅ 构建 **0 error / 0 warning** |
| **§4 验收判据 4（打包-校验同源）** | ✅ `rg`：`spec.internal_graph` / `spec.inputs` / `spec.exposed_params` 在 `ce_pack.cpp` 命中 **0 次**（仅存在于 engine/composite.cpp 与 composite_selftest.cpp 测试构造） |
| `api_probe.exe --exec-selftest`（含 composite_selftest） | ✅ **composite_selftest 112 通过 / 0 失败**（新增 21 条全过）· executor 713/0 持平 |
| `api_probe.exe --graph-selftest` | ✅ **151 通过 / 0 失败**（基线持平） |
| `aiwrite.exe --run-selftest` | ✅ **PASS** |
| `ce_repro.exe 0` | ✅ **mode=0 全帧 OK** |
| `check_links.py` | ✅ **1817 / 0**（基线持平 + 新增 audit_arch_e6.md 引用） |
| `check_all.ps1` | ✅ **19/19 全 PASS** |

## 4. 行为等价说明

| 旧实现（ce_pack.cpp） | 新实现（engine + ce_pack） | 等价论证 |
|---|---|---|
| 手工拼装 internal_graph（深拷贝 + 运行态清零 + 内部边） | `buildCompositeSpecFromSelection` 同款逻辑 | 逐项对应；唯一差异 = 指向暴露输入端口的选区内入边被剔除（**问题 2 修复点**，旧逻辑此场景打包必然失败 ⇒ 无旧行为可破坏） |
| 暴露输入识别（选区外入边） | 同款判据 + `exposed_in_ports` 集合 | 识别不变；新增剔除步骤 |
| 暴露输出（边界 + fallback + 报错） | 同款逻辑 | 一致 |
| 暴露参数（非密钥 + 去重） | 同款逻辑 | 一致 |
| `<2` / 嵌套 / 无输出 拒绝 | 同款文案 | 一致 |
| `save`（内部严格校验，失败报「打包失败：…」） | 显式 `validate_composite_spec` → `save` | 校验路径同源；新增「打包生成的规格未通过校验」前缀仅覆盖旧逻辑必然失败的非法路径 |

## 5. 范围边界（本批**不**接入 · 有意保留）

| 项 | 保留原因 | 承接 |
|---|---|---|
| 打包不删除选区内节点（复制式打包） | 与旧行为一致；选区节点继续留在主画布 | 保持现状 |
| 暴露参数「全部非密钥参数」默认全暴露 | 与旧行为一致；编辑器内可勾选/取消（CN-D6） | 保持现状 |
| 打包后父画布外部边（选区外节点 → 原选区内端口）不自动重接到新实例 | 打包不删原节点，父画布原图照常工作；新实例端口需用户手动接线 | 保持现状 |
| 编辑器「保存」路径（compositeEditorSave） | 本已走严格校验 + 热重载；与打包共用同一 `validate_composite_spec` | 保持现状 |

## 6. 过渡登记

- `buildCompositeSpecFromSelection` 为 **engine 公共 API**（composite.h 头文件签名新增）—— 接口冻结 §1 例外登记（重构批次内新增，显式登记于 FREEZE §5 / ARCHITECTURE §8 v1.8）。
- 打包是规格构建的**唯一入口**；后续任何「从选区生成上级节点」逻辑应复用该函数，不得在 ui 层再手工写 `spec.internal_graph/inputs/exposed_params`（§4 判据 4 由 `rg` 持续守护）。
- 「暴露输入不得有内部入边」规则在**生成阶段**（自动修正）与**校验阶段**（`validate_composite_spec` 拒绝）同源双保险。

## 7. 回滚点

`git checkout -- source/engine/include/composite.h source/engine/src/composite.cpp source/ui/src/ce_pack.cpp source/tools/composite_selftest.cpp docs/CHANGELOG.md docs/DocMAP.md docs/变量术语对照表.md docs/refactor/FREEZE.md docs/refactor/ARCHITECTURE.md` 即回滚到 E5 形态（保留 `docs/audit/audit_arch_e6.md` 仅作登记；删除即可）。
