# M_rerun：文本生成「重新生成（新 seed）」

> 需求（用户 2026-09-25）：给「文本生成」（LLMGenerate）节点加重跑能力 —— 效果等同「再次运行」，但**使用不同的 seed**；**不需要用户改任何东西**。
> 与既有计划的关系：`M_patchA §6.1 PD-06` 与 DevPlan `FEA-M4-11` 只定义了「重跑该节点」「重跑该节点及下游」（复用上游结果），**未包含 seed 语义** → 本次按用户要求**先改文档**再实现。
> 状态：🟡 文档先行（本文件 + PD-06 修订 + DevPlan `FEA-M4-17`）

## 1. 语义（本批实现）

- **入口**：参数面板（选中「文本生成」节点）按钮 **「重新生成（新 seed）」**
- **行为**：① 给该节点写入一个**新的 seed**（时间/随机派生；**先压撤销快照**，可撤销）② 启动一次运行（整图；本地上游节点瞬时重算）③ Console / 状态栏提示 `seed=N`
- **不改动图结构、不改其他参数**；`seed` 默认 `0` = 不指定
- **网页版（mode=web）**：前端协议无 seed 槽位 → **忽略并在 Console 提示一次**
  （说明：重跑本身即会产生不同结果，因为网页版默认采样）
- **官方 API（mode=official）**：请求体在 `seed > 0` 时带上 `seed`（若后端忽略，无害）
- **与 `FEA-M4-11` 的关系**：`FEA-M4-11` 是「通用重跑（单节点 / 及下游，复用上游结果）」，需要执行器支持「从某节点开始」；本批**只做「新 seed 重跑」**，不改执行器与运行模型。

## 2. 实现点

| 位置 | 变更 |
|---|---|
| `engine/node_registry.cpp` | LLMGenerate 增参数 `seed`（Int，默认 0，范围 0–2147483647，说明「0 = 不指定；「重新生成」会写入新值」） |
| `ai/deepseek_official_provider.{h,cpp}` | `OfficialChatRequest.seed`；`build_request_body` 在 `seed > 0` 时加入 `seed` |
| `nodes/local_nodes.cpp` | official 分支传 `seed`；web 分支在 `seed > 0` 时 Console 提示「网页版不支持 seed，已忽略」 |
| `ui/property_panel.cpp` | LLMGenerate 节点加 **「重新生成（新 seed）」**（压快照 → 写新 seed → `start_run_async()`） |
| `src/main.cpp` | 断言：`build_request_body` 含/不含 `seed`；节点定义含 `seed` |

## 3. 断言（离线）

1. `seed > 0` → 请求体含 `seed`，值与输入一致
2. `seed == 0` → 请求体**不含** `seed`
3. LLMGenerate 定义含 `seed` 参数（id / 默认值 0 / 最大 2147483647）

## 4. 人工确认

1. 选中「文本生成」→ 点「重新生成（新 seed）」→ Console 打印 `seed=N`；运行结束后正文与上次**不同**（且可撤销恢复到旧 seed）
2. 网页版：Console 出现「网页版不支持 seed，已忽略」提示一次，重跑结果仍不同
3. 官方 API：请求体带 seed（后端若忽略不影响功能）

## 5. 明确不做

- 通用「只重跑该节点 / 及下游且复用上游结果」（归口 `FEA-M4-11`，需执行器支持起始节点）
- 快捷键（设计 §6.7：MVP 不保留快捷键，保持纯鼠标；按钮入口足够）


## 6. 修复（P6）：SliderInt 范围越界 → 选中节点即 abort

- **现象**：`Assertion failed: *(const ImS32*)p_min >= IM_S32_MIN / 2 && *(const ImS32*)p_max <= IM_S32_MAX / 2`（`imgui_widgets.cpp` `SliderBehavior`，Debug 构建直接 `abort()`）
- **根因**：`seed` 参数声明 `0..2147483647`，而 ImGui 的 `SliderInt` 只支持 `±IM_S32_MAX/2`（`1073741823`）
- **修复**：① `ui/property_panel.cpp` 范围越界时降级 `InputInt`/`InputFloat` + 裁剪（防未来同类）② `seed` 范围改 `0..1000000000`，派生新值 `[0, 1e9]` ③ 新增离线断言遍历注册表校验所有带范围参数
- **约定**：新增数值参数时 `max` 必须 ≤ `1073741823`（Int）／`1.7e38`（Float），否则断言与面板降级会同时提示
