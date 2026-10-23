# M_code_cleanup：代码库清理 + 后续优化打磨（阶段性计划）

> 📦 **已归档（2026-10-23 · 文档整理批次 `ARC-AR-02`）** —— `CLN-01`…`CLN-15` **审查 + 勾账全部完成**（2026-10-20）+ 拆分项 `CLN-12`~`CLN-15` **全部落地**（2026-10-20 · 审计报告 [12](../../audit/audit_cleanup_12.md)/[13](../../audit/audit_cleanup_13.md)/[14](../../audit/audit_cleanup_14.md)/[15](../../audit/audit_cleanup_15.md)）+ `CLN-06` 扩项驼峰化 批 1~8 落地；§4 优化方向已以 `POL-*` 落入打磨主线，按 [../README.md](../README.md) §三 归档（`git mv` 保留历史，正文只增不改）。
> **承接位** → [M_polish.md](../../actionPlan/M_polish.md)（§4 承接表「方向已并入 2026-10-20」· `POL-*` 任务表）；**§2 暂停任务 `mode 5` 小地图导航** = `POL-L-02`（真机 GUI 复核）+ 债务总账 [../../audit/DEBT.todo](../../audit/DEBT.todo)；删除类操作仍走 **§3.0 删除前置审计闸门**（现行规则）。

> 类型：**跨里程碑工程质量补丁**（不占用 M1–M8 编号；**独立成文**，与 [M_patchAB_rest.md](M_patchAB_rest.md) / [M_patchC_web_node_bind.md](M_patchC_web_node_bind.md) 平级，**单向引用**、不改动既有计划文件任何一行）
> 动机：**2026-10-19 上级节点编辑器小地图排查中暂停**（见 §2）—— 借暂停窗口，把「代码库清理 → 后续优化打磨」固化为可执行计划；清理的目的 = **让代码库更干净**（去无用 / 去重复 / 去难读），为下一阶段优化打磨打地基。
> 编号空间：任务 **`CLN-01`…**（清理）· 验证 **`CLN-V*`**（清理后离线验证）· 验收 **`CLN-A*`**（清理后人工验收）—— ⚠️ 新编号空间，落地批需按库规登记（AI_README §5 / DocMAP / DevPlan.todo / CHANGELOG）。
> 状态：🟡 **已开工（2026-10-20）** · 清理批 0 = §2.3 恢复路径**已完成**（`ce_repro` 编译 0/0 · mode 5 = FAIL 已登记 §2.2）· **`CLN-01` 死代码扫描完成 = 无删除项**（[../audit/audit_cleanup_01.md](../../audit/audit_cleanup_01.md)）· **`CLN-02` 删 `api_probe.cpp` 2 行重复 include · 构建 0/0 + selftest 全绿**（[../audit/audit_cleanup_02.md](../../audit/audit_cleanup_02.md)）· **`CLN-03` 失效分支扫描 = 无删除项**（[../audit/audit_cleanup_03.md](../../audit/audit_cleanup_03.md)）· **`CLN-04` TODO/FIXME/HACK 台账 = 初始即归零 · 无删除项**（[../audit/audit_cleanup_04.md](../../audit/audit_cleanup_04.md)）· **`CLN-09`+`CLN-10` 小地图/对齐吸附共享化已落地（构建 0/0 · selftest 全绿 · [../audit/audit_cleanup_09_10.md](../../audit/audit_cleanup_09_10.md)）· `CLN-11` 复核无残留勾账** · **`CLN-06`~`CLN-08` 纯审计零改造勾账 + `CLN-12`~`CLN-15` 拆分可行性审查勾账（未落地 · [../audit/audit_cleanup_06_07_08.md](../../audit/audit_cleanup_06_07_08.md)）· `CLN-06` 扩项（驼峰化）= **批准执行（§3.4 完成后）** —— 已拍板（2026-10-20）：asyncMode = **保留**（观察项关闭 · [../audit/audit_cleanup_01.md](../../audit/audit_cleanup_01.md)）· mode 5 = **仅文档推进**（导航链路 + 4 根因候选登记 §2.2 · 待真实 GUI 复核）· **§3.4 拆分（CLN-12~15）= 批准执行 · `CLN-12` 已落地**（property_panel 拆分 · 构建 0/0 · selftest 682/151/13 组全绿 · [../audit/audit_cleanup_12.md](../../audit/audit_cleanup_12.md)） · **匿名命名空间成员专项 = 放弃** · **打磨与优化阶段立项**（[M_polish.md](../../actionPlan/M_polish.md) · roadmap §二 · 最优先）。
> 相关：现行计划 [M_patchAB_rest.md](M_patchAB_rest.md)（交互打磨残项承接位）· 上级节点编辑器重构草案 [../draft/CN_editor_refactor.md](../../draft/CN_editor_refactor.md)（阶段 3 收尾 = 清理本库重复实现的直接落点）· 上级节点设计与实施 [../Archive/actionPlan/Composition_node.md](Composition_node.md)（📦 已归档）· 网页通道方案 E [M8.md](../../actionPlan/M8.md)（**Pydoll 休眠栈 · 清理时禁止触碰**，见 §6）
> 建立：**2026-10-19**（v1 · v1.1 同日增补「删除前置审计闸门」）

---

## §0 文档元信息与约定

### 0.1 定位

- **本文不是新里程碑，也不是一次性的「大扫除」**：它把「清理」与「后续优化打磨」两个阶段写成一份**可执行清单**，每批小步落地、随时验证，禁止「改到一半停不下来」。
- **清理的边界**：只动 `source/**` 里**确实无用 / 确实重复 / 确实难读**的代码；**契约、休眠栈、第三方库、已归档文档**一律不碰（§6）。
- **与重构草案的关系**：[../draft/CN_editor_refactor.md](../../draft/CN_editor_refactor.md) 阶段 3 的「删除被共享替代的上级编辑器残留」是**清理清单的一部分**（§3.3），两者不冲突 —— 本文是**全库级**清理计划，该草案只管上级编辑器自身。

### 0.2 本轮范围（做 / 不做）

| 做 | 不做 |
|---|---|
| 死代码 / 无用代码扫描与移除 | ❌ 触碰 `third_party/**`（vendored imgui-node-editor） |
| 重复实现合并（主画布 ↔ 上级编辑器平行代码） | ❌ 触碰 **Pydoll 休眠栈**（`source/python/**`、`pydoll_channel.*`、`pipe_client` —— `M8-Q7` 已拍板「休眠保留 · 不删」） |
| 大文件拆分（可读性） | ❌ 改动既有**契约**（`I*` 不变量 / JSON 规格 / CLI 契约 / 端口语义） |
| 头文件 / include 清理、未用全局/函数清理 | ❌ 重命名已登记的**任务号 / 编号空间**（`CLN-*` 之外全部沿用） |
| 编译告警清零、命名一致性 | ❌ 归档文档改写（`docs/Archive/**` 只读） |
| 文档 / 看板登记（本计划落地批） | ❌ 一次跨多模块的「大爆炸」重构（**每批独立验证**，见 §0.3） |

### 0.3 执行原则

1. **每批小步、每批验证**：一个清理项 = 一个批；每批结束时 `source/build.ps1` 构建 **0/0** + 相关 selftest 全绿 + `check_links` broken 0，才进下一批。
2. **先扫后动**：每批先产出「扫描证据」（哪个文件哪一行 / 引用计数 / 是否被测试或文档引用），**证据不过不删**。
3. **语义等价**：合并重复代码只收敛**实现**，不改变**行为**；涉及行为差异（如上级编辑器差异主题）一律保留差异参数，不强行并。
4. **删除即登记**：任何删除都要在批记录里写清「删了什么 / 为什么 / 依据」，避免将来「又觉得有用」回来考古。
5. **测试保护**：合并或删除后，`composite_selftest` / `exec-selftest` / `graph-selftest` 只升不降；行为类改动加断言。
6. **删除前置审计闸门（⛔ 硬性）**：凡涉及**正式删除**（代码 / 文件 / 分支）的批，必须先产出**审计报告**（删除对象清单 + 引用证据 + 影响面 / 回滚点）+ **行动计划**，交**人工审核通过后**才执行删除；**未经审核一律不得删**（格式见 §3.0，验收见 `CLN-A8`）。

---

## §1 结论摘要（先读这一段）

**一句话**：当前上级节点编辑器小地图排查**已暂停**（§2 登记了恢复路径）；接下来先做一轮**代码库清理**（`CLN-01`…），再在其上做**优化与打磨**（§4），两条线都以「**小批 + 每批独立验证**」推进，禁止大爆炸式重构。

**两条主线**：

1. **清理（§3，第一优先）**：去**无用**（死代码 / 未用全局 / 冗余 include）→ 去**重复**（主画布与上级编辑器的平行实现，最小地图 / 对齐辅助线即现存实例）→ 去**难读**（超大单文件拆分、命名一致、编译告警清零）。
2. **优化打磨（§4，清理之后）**：在干净地基上做**性能 / 体验 / 结构**优化与打磨（承接 `M_patchAB_rest.md` 交互打磨残项 + 本库自有的可读性 / 一致性提升项）。

---

## §2 暂停任务状态登记（恢复入口）

> 本节记录 **2026-10-19 暂停时的精确状态**，供随时恢复；**暂停 ≠ 放弃**，恢复后按本登记继续。

### 2.1 任务背景

上级节点编辑器（composite editor）的小地图**点击 / 拖动导航不生效**，而主画布小地图正常。两处实现经**逐行比对确认逻辑完全一致**（`node_canvas.cpp` 的 `minimap_rect` / `minimap_hit` / `block_minimap_input` / `draw_minimap` ↔ `composite_editor.cpp` 的 `ce_minimap_rect` / `ce_minimap_hit` / `ce_block_minimap_input` / `ce_draw_minimap`），`ed::NavigateToCenter` 调用时序也已对齐（`ed::End()` 之后、`SetCurrentEditor(nullptr)` 之前）。故问题疑点在**运行期行为**而非静态结构。

### 2.2 已完成的排查

- **headless 复现工具**：`source/tools/ce_repro.cpp` 已新增 **mode 5**（`m5_draw_composite_editor` / `m5_draw_minimap` / `m5_block_minimap_input` / `m5_minimap_rect` / `m5_minimap_hit` + 跨帧断言全局 `g_m5_target` / `g_m5_view_center` / `g_m5_target_set`），用于**隔离「导航机制」vs「输入命中」两类故障**。
- 比对结论：两套小地图代码**逐行一致**；上级编辑器小地图**绘制时机已修复**（v0.4，移出 `ed::Begin`…`ed::End`）。
- **恢复基线（2026-10-20）**：`ce_repro.cpp` 编译错误已修（L389-390 `std::fabs` → `std::abs`，补 `<cmath>` include）；`build.ps1 -Target ce_repro` **0 error / 0 warning**（物证 `build/logs/build_debug_20261020_001751.log`）。
- **mode 5 复跑结果（2026-10-20 · `ce_repro.exe 5`）**：**FAIL** —— `NavigateToCenter target=canvas(140.0, 120.2)` 正常打印（输入命中 + 调用链路无碍），但随后逐帧 `view_center` **恒定 `canvas(32.0, -27.8)`**，视图未移动 ⇒ **导航机制在第二个编辑器上下文（`g_ce_ctx`）未生效**（对照 §2.3 步骤 4 FAIL 分支：疑点在 `ed::NavigateToCenter` + `ed::Begin` 在第二编辑器上下文的交互 / 导航状态机）。已核实 harness 与真实调用时序一致（`ce_block_minimap_input` / `m5_block_minimap_input` 均在 `ed::Begin` 之前；`NavigateToCenter` 实际位于输入处理函数内、绘制在 `ed::End` 之后）。
- **导航链路全拆解（2026-10-20 · plan 模式只读取证）**：imgui-node-editor 导航链路逐环确认 ——
  1. `ed::NavigateToCenter(target, 0)`（api.cpp L579）→ `s_Editor==nullptr` 直接 no-op；否则构造 2×2 bounds → `m_NavigateAction.NavigateTo(bounds, ZoomMode::None, 0)`。
  2. ZoomMode::None → `viewRect.Translate(targetCenter - viewRectCenter)` → `m_Animation.NavigateTo(target, 0)`。
  3. `Animation::Play(0)`（L2957）→ **同步** Register + `Finish()` → `OnFinish` → `SetViewRect(m_Target)`（L3617）→ **正确反解并更新权威状态 `m_Scroll/m_Zoom`**（非仅缓存）。
  4. 下一帧 `EditorContext::Begin` → **L1238 `m_Canvas.SetView(m_NavigateAction.GetView())`** 应用视口 → `ScreenToCanvas` 理论应返回 target。
- **机制层结论**：`ed::End()` 之后调 `NavigateToCenter` 的时序**设计上应生效**（动画同步完成 · 下帧 Begin 应用）；FAIL 根因不在链路设计，而落环境 / 时机细节。
- **4 个根因候选（按嫌疑排序）**：
  - **H1 · headless 模拟失真**（首要嫌疑）：ce_repro 无真实窗口 / GPU，Canvas window rect / DisplaySize 可能异常 → `CalcCenterView` / `SetView` / `ScreenToCanvas` 变换错位；若成立 = 模拟问题而非真实 bug。
  - **H2 · SetView 时机**：L1238 的 SetView 相对用户绘制 / `ScreenToCanvas` 的先后未证实（需读 `EditorContext::Begin` L1150-1260 确认）。
  - **H3 · 真实 bug**：H1 / H2 排除后才考虑第二编辑器上下文互扰；导航状态挂在各自 ctx（`g_ce_ctx.m_NavigateAction`），机制上不共享。
  - **H4 · 帧时序**：`ed::End()` 后调用本帧 SetView 已过；若下帧 Begin 的 canvas 分支在 headless 下提前 return 则永不应用。
- **2026-10-20 用户拍板 = 仅文档推进（选项 C）**：以上登记入档；**待有条件时先做真实 GUI 复核**（构建 app → 人工点上级编辑器小地图）—— 正常 ⇒ FAIL 为 headless 失真（H1/H4），任务收尾关闭；不生效 ⇒ 真实 bug，按 §2.3 步骤 4 FAIL 分支深挖 H2 / H3。

### 2.3 恢复路径（下一步怎么做）

1. **修一个编译错误**（暂停前最后一次构建失败，仅 `ce_repro` 目标）：`ce_repro.cpp` L389-390 用了 `std::fabs`（MSVC 下不在作用域）→ 改用 `std::abs`（`<cmath>`）即可。⚠️ **当前 `ce_repro` 目标编译是坏的，恢复时第一件事就是修这个。**
2. 重新构建：`cd source; powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target ce_repro`。
3. 运行：`build\bin\ce_repro.exe 5`，观察输出：
   - `NavigateToCenter target=canvas(x, y)` 是否打印；
   - 下一帧 `view_center` 是否等于 `target`（程序自带 PASS/FAIL 断言）。
4. 按结果二分：
   - **PASS** → 导航机制在第二个编辑器上下文可用 ⇒ 问题在**实际 UI 的输入命中 / 鼠标坐标**，回到 `composite_editor.cpp` 查输入链路；
   - **FAIL** → 查 `ed::NavigateToCenter` + `ed::Begin` 在第二编辑器上下文的交互（`imgui_node_editor` 导航状态机）。
5. 结论落地后，本任务回归位：`docs/draft/CN_editor_refactor.md`（小地图对齐表）+ 本库清理清单 §3.3（若根因是重复实现差异导致，则优先走「合并共享实现」路线）。

> **2026-10-20 更新**：步骤 1–3 **已完成**（编译已修 · 构建 0/0 · mode 5 已跑 = **FAIL**，结论已登记 §2.2）。后续按步骤 4 **FAIL 分支**继续（查 `ed::NavigateToCenter` + `ed::Begin` 在第二编辑器上下文的交互）。恢复路径在**暂停任务恢复时**生效，不阻塞当前清理。
> **2026-10-20（二）· 用户拍板**：mode 5 排查 = **仅文档推进**（导航链路全拆解 + 4 根因候选已登记 §2.2）。FAIL 分支第一步更新为：**真实 GUI 复核**（构建 app → 人工点上级编辑器小地图）—— 区分「headless 模拟失真（H1/H4）」vs「真实 bug（H2/H3）」。真实复核环境可用时恢复执行。

### 2.4 本任务与清理计划的衔接

- 若排查发现根因是**两套平行实现漂移** ⇒ 直接并入 §3.3 的合并任务，清理 = 修复。
## §3 清理清单（第一优先 · `CLN-01`…）

> 顺序 = 优先级（先低风险、后高风险）；每项一个小批，**批内独立验证**（§0.3）。标注「📌 依赖」的项须等前项落地。**含删除的项（`CLN-01` / `CLN-03` / `CLN-05` / `CLN-09`~`CLN-11` 等）必须走 §3.0 删除前置审计闸门。**

### 3.0 删除前置审计流程（⛔ 每批删除前必走）

> 对应 §0.3 第 6 条。**任何删除（代码 / 文件 / 分支）都按此流程，先审核、后动手；审核未过 = 不删。**

| 步骤 | 产出物 | 说明 |
|---|---|---|
| ① 对象清点 | 删除对象清单 | 精确到「文件 : 行号 : 符号 / 文件名」，按删除理由分组（无用 / 重复 / 难读 / 失效） |
| ② 引用取证 | 引用证据 | 每个对象给出：全库引用计数 + 是否被测试 / 文档 / 配置 / 诊断入口引用；`grep` 与 `check_links` 输出留档 |
| ③ 影响面评估 | 影响面 / 回滚点 | 被删内容的影响范围、是否有行为变化、git 回滚点（commit / 基线）、是否影响契约或休眠栈 |
| ④ 行动计划 | **行动计划（交人工审核）** | 按批列出「删什么 → 怎么验 → 回滚方式」，提交用户**人工审核** |
| ⑤ 审核闸门 | 审核结论 | ✅ 通过 → 执行删除；⛔ 不通过 / 需修改 → 修改后重新提交，**通过前不删** |
| ⑥ 落地与登记 | 批记录 + 证据存档 | 删除落地后：构建 0/0 + selftest 全绿 + 批记录写清「删了什么 / 为什么 / 依据」，审计报告存档 `build/logs/` 或 `docs/audit/` |

> 📌 **审计报告建议落点**：`docs/audit/`（与库内 `DEBT.todo` / `L1_checklist.todo` 同层，沿用审计目录约定）；每次删除批的审计报告以 `audit_cleanup_<批次>.md` 命名。


### 3.1 死代码 / 无用代码扫描与移除（`CLN-01`~`CLN-05`）

| # | 任务 | 做法与判据 | 验证 |
|---|---|---|---|
| `CLN-01` | **未使用的全局 / 静态变量与函数** | 全库扫描 `source/**`（排除 `third_party`）：未被引用的 `static` / 文件内匿名命名空间符号 → 逐个列证据后删；⚠️ **诊断入口除外**（`--pydoll-*` / `--vlm-selftest` / `--image-decode` 等按契约保留） | 构建 0/0 · selftest 全绿 |
| `CLN-02` | **冗余 / 未用 include** | 用 IWYU 思路逐个 `.cpp/.h` 核对 include 是否真用到；删除后必须能**干净重编译**（不靠碰运气，靠构建日志） | 构建 0/0 |
| `CLN-03` | **失效分支 / 注释掉的代码块** | 扫描大段 `// ...` 注释掉的代码与 `#if 0` 块：确认无恢复价值 → 删除（历史进 git，不必留在源码里）；有「考古价值」的移入对应归档文档 | 构建 0/0 · `check_links` 0 |
| `CLN-04` | **`TODO` / `FIXME` / `HACK` 台账** | 全库收集 → 逐条判定：已实现（删标）/ 仍有效（登记进 `DevPlan.todo`，不留在代码里）/ 已无意义（删） | 台账归零（文档级） |
| `CLN-05` | **重复的辅助函数** | `ce_find_ci`（composite_editor.cpp）等与 `utils/**` 既有实现的重复 → 收敛到 utils 单一来源（⚠️ 先查引用，勿动语义） | 构建 0/0 · 相关 selftest 升 |

> ✅ **`CLN-01` 已勾账（2026-10-20 · 审计报告 [../audit/audit_cleanup_01.md](../../audit/audit_cleanup_01.md)）**：全库扫描 `source/**`（163 文件，排除 `third_party` / `source/python`）→ **static 未引用 0** · **`#if 0` 块 0** · 非 static 函数 refs=1 仅 3 项且全部核验非死代码（`is_id_char` 扫描器误报 / `sink_it_`+`flush_` 为 spdlog 虚覆写 / `asyncMode` 属公共 API 建议保留）⇒ **无删除项，勾账**。与 `/W4` 0 警告构建互证（C4505 未触发）。匿名命名空间成员专项**已拍板 = 放弃（2026-10-20 · 用户指令）**（不做 clang-tidy 专项，相关评估并入驼峰化批一并处理）。

> ✅ **`CLN-02` 已勾账（2026-10-20 · 审计报告 [../audit/audit_cleanup_02.md](../../audit/audit_cleanup_02.md)）**：全库 225 文件 include 符号级审计（扫描器过合成阳性对照）→ **未用项目头 include 0**（库风严谨，抽查重构文件每个 include 均带用途注释）· **唯一可删项 = `tools/api_probe.cpp` 2 行重复 include**（第 2 处 `paths.h` 原 L39 / 第 2 处 `webview_host.h` 原 L52，均保留首处）→ **已删 · 构建 0 error / 0 warning · selftest 全绿**（`--selftest` 13 组 PASS · `--exec-selftest` 682/0 · `--graph-selftest` 151/0）⇒ **勾账**。外部头（std / 系统 / vendored）无静态符号表，本批无候选不盲删。扫描器 3 个 bug 修复记录见审计报告 §5。

> ✅ **`CLN-03` 已勾账（2026-10-20 · 审计报告 [../audit/audit_cleanup_03.md](../../audit/audit_cleanup_03.md)）**：全库 225 文件扫描失效分支 / 注释掉的代码块 → **`#if 0` / `#if false` 块 0**（与 CLN-01 互证）· **注释掉的代码块候选 13 块全部核验为文档性注释**（含 CEA-D2 修复时序、imgui-node-editor 契约、P0 修复原因、SSE/PoW 协议事实等「删了会踩坑」的记录，建议保留）· 单行注释语句（调用 / 赋值 / 裸括号）**0** ⇒ **无删除项，勾账**。扫描器过合成正/负对照，修复行注释跨度闭合 bug 1 个；方法学见审计报告 §5。

> ✅ **`CLN-04` 已勾账（2026-10-20 · 审计报告 [../audit/audit_cleanup_04.md](../../audit/audit_cleanup_04.md)）**：全库 163 个 `.cpp/.h` 扫描 `TODO` / `FIXME` / `HACK` 台账（双扫描器互证 + 合成正例对照）→ **真标记 0**（11 处命中全为占位符 `xxx-web` 撞 `XXX` 与正文「真 bug」撞 `BUG` 的误报）· 中文等价词（待办/未实现/未完成）全为**功能状态与运行时文案**（如「协议未实现」错误串、selftest 断言），非债务标记不入台账 · `third_party` 上游 `TODO/FIXME` 属 §6 硬边界不碰 ⇒ **台账初始即归零 · 无删除项 · 无登记项，勾账**。

> ✅ **`CLN-05` 已勾账（2026-10-20 · 审计报告 [../audit/audit_cleanup_05.md](../../audit/audit_cleanup_05.md)）**：全库同名辅助函数清点（163 文件，逐行一致判定）→ **5 组真重复 14 处定义全部收敛到 `utils/string_util.{h,cpp}` 单一来源**（`findCi` / `trim` / `nowString` / `toLower` / `narrow`，命名按用户指示用驼峰先导）· 删除 14 处本地定义 + 改 55 处调用点（`node_library` 4 · `composite_editor` 2 · `deepseek_web_client` 2 · `http_net` 10 · `provider_spec` 17 · `site_template` 11 · `graph` 1 · `autosave` 1 · `recent_files` 1 · `session_store` 3 · `diagnostics_report` 1 · `webview2_runtime` 2）· 12 文件加 `#include "string_util.h"` · CMakeLists 登记 `string_util.cpp` 入 `aiwrite_core` · **构建 0 error / 0 warning · selftest 全绿**（`--selftest` 13 组 PASS · `--exec-selftest` 682/0 · `--graph-selftest` 151/0）· `check_links` **1610 / 0** ⇒ **勾账**。排除项（同名不同义，勿动语义）：`join_names` / `join_list` / `base64_encode` / `local_time_text` / `to_utf8` / `human_bytes` / `megabytes_text` / `read_reg_string` / `extract_cookies`（差异证据见审计报告 §① B）。**按用户指示（2026-10-20）：全库函数/变量统一驼峰化为新一批（暂记 `CLN-06` 扩项），本批 5 函数已作先导样板。**

### 3.2 头文件 / 结构一致性（`CLN-06`~`CLN-08`）

| # | 任务 | 做法与判据 | 验证 |
|---|---|---|---|
| `CLN-06` | **公共头只放声明** | `composite_editor.h` / `node_canvas.h` / `canvas_view.h` 等头文件的**实现体**（`inline` 大函数 / 静态数据）移入 `.cpp`；头文件保持「能独立编译」 | 构建 0/0 · 头文件自包含检查 |
| `CLN-07` | **命名与分层一致性** | 同一模块内同一概念统一前缀（`ce_*` 只在 composite_editor 内使用，不得泄漏到其他 TU）；文件级职责注释与头部约定一致 | 构建 0/0 · 人工抽查 |
| `CLN-08` | **编译告警清零** | 构建日志 warning 逐条清零（非 third_party 产生的）；`/W4` 可开则开，开不了的记录原因 | 构建 **0 error / 0 warning** |

> ✅ **`CLN-06`~`CLN-08` 已勾账（2026-10-20 · 审计报告 [../audit/audit_cleanup_06_07_08.md](../../audit/audit_cleanup_06_07_08.md) §③/§④，用户授权审查后勾账）**：纯审计批次，**零代码/零结构改动**。`CLN-06` 头文件只放声明：`canvas_view.h` / `composite_editor.h` / `node_canvas.h` / `property_panel.h` 均纯声明 + `constexpr` + 结构体，无 `inline` 大函数 / 非 constexpr 静态数据；自包含静态证据齐全（include 覆盖使用类型）+ 构建 0/0 佐证 ⇒ **无改造项，勾账**。`CLN-07` 命名与分层：`ce_*` 短前缀仅 `composite_editor.cpp` 内部 88 处（header 无声明、他 TU 不可见）**无泄漏**；对外 API 用 `composite_editor_*` / `composite_*` 长前缀分层清晰；文件级职责注释一致 ⇒ **无违规，勾账**（观察点：`canvas_view.cpp` L582 imgui ID `##ce_inline_` 借 `ce_` 字样，**非符号**，可选后续统一）。`CLN-08` 编译告警清零：构建日志 **warning 0 / error 0** + `/W4` 已开（CMakeLists L325/389/409/436）⇒ **判据达成，勾账**。
>
> ✅ **`CLN-06` 扩项（全库函数/变量统一驼峰化）= 已拍板（2026-10-20）· 批准执行**（§3.4 拆分完成之后开始 · 分批次推进）：改动面未量化（全库命名 + 调用点 + 文档）、收益为主观决策、节奏未定（大爆炸 vs 分批次）——属用户拍板项，详细论证见审计报告 §⑤-e。

> 🟢 **驼峰化（CLN-06 扩项）批 1 = utils 域已落地（2026-10-20）**：81 个 snake_case 公共函数 → camelCase（asset_store 12 / config 7 / credential 11 / diagnostics 6 / file_dialog 4 / log 4 / output_archive 1 / paths 23 / settings_validation 1 / text_export 8 / webview2_runtime 2 / window_geometry 1）· 全库定义 + 调用点 **550 次替换 / 60 文件**（头文件声明同步改：`paths::configFile()` / `loadConfig()` / `setAppConfig()` 等）· 脚本 = C++ 感知状态机（跳过 `//` 注释 / `/* */` 块注释 / 字符串 / 字符字面量 / raw string，并识别 **C++14 数字分隔符** `60'000` 避免误判字符字面量 —— 首轮漏替换即由此起）· 成员变量 / 局部变量不在此批范围（`archive_dir` 等保留）· **构建 0 error / 0 warning（4 目标）· selftest 全绿**（682 / 151 / 13 组 · 输出零内容差异——剥离时间戳后仅剩耗时数字 / 带日期临时文件名差异）· `check_links` 1684/0。**执行序**：批 2 engine → 批 3 nodes → 批 4 ai → 批 5 web → 批 6 ui → 批 7 app → 批 8 tools。

> 🟢 **驼峰化（CLN-06 扩项）批 2~8 已全部落地（2026-10-20）**：**批 2 = engine 域** 48 个 snake_case 公共函数 → camelCase（autosave 7 / composite 11 / graph 3 / migration 3 / node_registry 4 / provider_resolve 10 / recent_files 6 / workflow_io 4）· 全库定义 + 调用点 **315 次替换 / 35 文件** · 构建 0/0 · selftest 682/151/13 组全绿（输出零内容差异）。**批 3 = nodes 域**：公共 API 仅 `registerAllExecutors()`（已驼峰）→ **0 项跳过**。**批 4 = ai 域** 123 个 snake_case 公共函数 → camelCase（anthropic 2 / cancel_token 2 / deepseek_official 13 / deepseek_web 3 / dom_web_client 16 / gemini 2 / http_json 3 / http_net 7 / image_convert 3 / image_preprocess 1 / inference_provider 5 / provider_factory 4 / provider_spec 30 / retry_policy 3 / site_template 9 / site_uploads 2 / upload_evidence 9 / upload_registry 7 / upload_supply_probe 1 / web_pow 3）· **918 次替换 / 62 文件** · `join_names` 与 `cli_selftest.cpp` 全局版命名空间隔离冲突无 · 构建 0/0 · selftest 682/151/13 组全绿。**批 5 = web 域** 72 个 snake_case 公共函数 → camelCase（channel_frames 9 / pipe_client 16 / probe_verdict 5 / pydoll_channel 11 / session_snapshot 5 / session_store 11 / webview_host 15）· **544 次替换 / 32 文件** · 构建 0/0 · selftest 682/151/13 组全绿。**批 6 = ui 域** 145 个 snake_case 公共函数 → camelCase（canvas_view 20 / ce_internal 6 / cli_internal 12 / composite_editor 24 / connection_test 10 / console_panel 2 / editor_state 38 / error_bar 1 / node_canvas 3 / node_library 1 / output_panel 3 / panel_internal 5 / prompt_generator 4 / settings_panel 4 / text_view 1 / texture_cache 10 / toolbar 1）· **734 次替换 / 48 文件** · 构建 0/0 · selftest 682/151/13 组全绿。**批 7 = app 域**（仅 3 入口 cpp、无公共头）· **批 8 = tools 域**（自检/诊断工具、无公共头 snake_case 声明）→ **0 项跳过**。**8 批全部完成**：utils 81 / engine 48 / ai 123 / web 72 / ui 145 = **469 个函数 · 3061 次替换 · 237 文件**（nodes/app/tools 跳过）。每批脚本 = C++ 感知状态机（跳过注释 / 字符串 / 字符字面量 / raw string + **C++14 数字分隔符** `60'000` 识别）· CRLF 保留（`newline=''`）· 零内容差异验证（剥离时间戳后 Compare-Object）。


### 3.3 重复实现合并（主画布 ↔ 上级编辑器）· 📌 与 [../draft/CN_editor_refactor.md](../../draft/CN_editor_refactor.md) 阶段 3 同向

| # | 任务 | 现存的平行实现（清理对象） | 合并方向 |
|---|---|---|---|
| `CLN-09` | **小地图共享化** | `node_canvas.cpp`：`minimap_rect` / `minimap_hit` / `block_minimap_input` / `draw_minimap` ↔ `composite_editor.cpp`：`ce_minimap_rect` / `ce_minimap_hit` / `ce_block_minimap_input` / `ce_draw_minimap`（逻辑逐行一致，仅主题差异） | 收敛到 `canvas_view.{h,cpp}` 共享只读绘制层（`CanvasContext` + `CanvasTheme`），主题差异走参数；删 `ce_minimap_*` |
| `CLN-10` | **对齐吸附 + 辅助线共享化** | `node_canvas.cpp`：`apply_alignment_snap` / 对齐线绘制 ↔ `composite_editor.cpp`：`ce_find_dragged_node` / `ce_apply_alignment_snap` / `ce_apply_snap` / `ce_draw_snap_lines` | 收敛共享实现（保留上级编辑器差异主题）；删 `ce_*` 平行版 |
| `CLN-11` | **节点绘制 / 结果视图 / 交互处理** | 核查 `ce_*` 中是否仍有与主画布平行的节点绘制 / 交互残留（v0.3 已删 `ce_draw_node` / `ce_result_view` / `ce_*_handle`，**复核无残留**） | 有则并、无则勾账 |

> ⚠️ **合并不等于删功能**：上级编辑器的**差异行为**（蓝紫主题、规格面板、沙箱执行）必须原样保留，只收敛**机制相同**的部分。合并完成后再做 §2 的小地图回归。

> ✅ **`CLN-09` + `CLN-10` 已勾账（2026-10-20 · 审计报告 [../audit/audit_cleanup_09_10.md](../../audit/audit_cleanup_09_10.md)，§⑤ 人工审核通过后落地）**：小地图（`minimap_rect` / `minimap_hit` / `block_minimap_input` / `draw_minimap` + `MinimapState`）+ 对齐吸附（`find_dragged_node` / `apply_alignment_snap_core` / `draw_snap_lines` + `SnapResult`）全部收敛到 `canvas_view.{h,cpp}` 单一来源 · 删除上级 `ce_minimap_rect/hit/block/draw` / `ce_find_dragged_node` / `ce_apply_alignment_snap` / `ce_draw_snap_lines` / `CeSnapResult` · 主画布同步删本地重复实现（7 个符号）+ `g_*_minimap_*` 8 全局收敛为 2 个 `MinimapState` · 保留全部差异行为：两套主题取色 / 节点块色回调 / 辅助线色宽（`snap_line,1.6f` ↔ `(200,80,220),1.0f`）/ Alt 平移互斥 / 网格 16px 吸附（G7）/ `kCeSnapDistance=8` / 两画布开关 · `ce_apply_snap` 保留薄封装（含网格吸附）· `g_ce_prev_positions` 保留为共享 `find_dragged_node` 参数宿主 · **构建 0 error / 0 warning · selftest 全绿**（`--selftest` 13 组 PASS · `--exec-selftest` 682/0 · `--graph-selftest` 151/0）· `check_links` **1613 / 0** · 残差断言 0 ⇒ **勾账**。
> ✅ **`CLN-11` 已勾账（2026-10-20 · 审计报告同上 §6.5）**：`ce_draw_node` / `ce_result_view` / `ce_*_handle` 全库 **0 残留** → **复核无残留，勾账（无删除）**。

### 3.4 超大文件拆分 / 可读性（`CLN-12`~`CLN-15`）

| # | 文件（现状） | 拆分方向 | 判据 |
|---|---|---|---|
| `CLN-12` | `ui/src/property_panel.cpp`（≈95 KB） | 按「参数控件类型」拆子文件（如 `panel_widgets.*` / `panel_enum.*`） | 不改变行为；单文件 < 60 KB |
| `CLN-13` | `ui/src/composite_editor.cpp`（≈83 KB） | 按「画布区 / 面板区 / 状态管理 / 沙箱运行」拆 | 同上 |
| `CLN-14` | `app/src/main.cpp`（≈122 KB） | **按 CLI 功能区拆**（主循环/窗口已承载于 `platform_glfw.cpp` / `platform_win32.cpp` / `webview_host.cpp`，不在此拆）：启动自检与 CLI 分派域抽 `cli_selftest.*`（`run_selftest` 700 行为最大块）+ 诊断/杂项 CLI 域抽 `cli_diag.*` | 同上 |
| `CLN-15` | `tools/api_probe.cpp`（≈399 KB） | **只拆结构不动断言**：selftest 各节拆独立文件（或分节 `#include` 自包含测试文件） | `api_probe` 行为与输出**逐字节不变**（先跑基线存档再动） |


> ✅ **`CLN-12`~`CLN-15` 审查项已勾账（2026-10-20 · 审计报告 [../audit/audit_cleanup_06_07_08.md](../../audit/audit_cleanup_06_07_08.md) §⑤，用户授权审查后勾账）**：拆分可行性全部审查完成，**未落地任何拆分（落地属改造批次，留待人工批准执行）**。`CLN-12` `property_panel.cpp`（≈93 KB）**可拆**：控件分派核心 `draw_param_widget`（460 行）→ 按控件类型抽 `panel_widgets.*` / `panel_enum.*`，风险中低。`CLN-13` `composite_editor.cpp`（≈71 KB，CLN-09/10 后已从 83 KB 降）**可拆、收益递减**：区块已高度自组织，`draw_composite_editor`（660 行）可细化，低优先。`CLN-14` `main.cpp`（≈122 KB）**可拆但方向须修正 ⚠️**：原始方向「拆『窗口/输入/主循环』」**已过时**——主循环承载于 `platform_glfw.cpp` / `platform_win32.cpp` / `webview_host.cpp`，main.cpp 主体 = CLI/自检分派 → **修正方向 = 按 CLI 功能区拆**（`cli_selftest.*` / `cli_diag.*`），`run_selftest`（700 行）为最大块，拆前需重写本表 CLN-14 描述。`CLN-15` `api_probe.cpp`（≈390 KB）**可拆**：分节边界清晰，`execution_selftest`（≈3500 行单函数）为最大对象；判据「逐字节不变」须先跑基线（`--selftest` 13 组 / `--exec-selftest` 682 / `--graph-selftest` 151）存档再动。

> 🟢 **`CLN-12` 已落地（2026-10-20 · 审计报告 [../audit/audit_cleanup_12.md](../../audit/audit_cleanup_12.md)）**：`property_panel.cpp`（1932 行 → **814 行**）按域拆三份 —— ① `panel_widgets.cpp`（参数控件域：`draw_param_widget` + 控件辅助，598 行）② `panel_web.cpp`（网页会话 + 提供商下拉域，476 行）③ `panel_internal.h`（三 TU 内部共享声明：`param_label` / `is_web_mode` / `draw_web_session_section` / `build_provider_choices` / `sync_provider_id_with_mode` + `ProviderChoice`）。**行为等价验证**：构建 **0 error / 0 warning**（4 目标）· `--exec-selftest` **682/0** · `--graph-selftest` **151/0** · `--selftest` **十三组全 PASS**（与基准零升降）。**执行序**：`CLN-13`（composite_editor）→ `CLN-14`（main，先重写方向描述）→ `CLN-15`（api_probe，先跑基线存档）。

> 🟢 **`CLN-13` 已落地（2026-10-20 · 审计报告 [../audit/audit_cleanup_13.md](../../audit/audit_cleanup_13.md)）**：`composite_editor.cpp`（1687 行 → **172 行**）按域拆五份 + `ce_internal.h` 内部共享头 —— ① `ce_state.cpp`（会话状态 + 撤销域：`composite_editor_state` + 撤销栈 + `composite_editor_undo_once/redo_once`，86 行）② `ce_session.cpp`（会话操作域：open / save / request_close / force_close / snapshot / add_node / delete_node / add_edge / `ce_remove_node`，210 行）③ `ce_run.cpp`（试运行「执行连线」域：start / stop / clear / run_active / run_snapshot / tick_run，222 行）④ `ce_pack.cpp`（打包选区域：`composite_pack_selection`，156 行）⑤ `ce_draw.cpp`（编辑器窗口 + 新建/打包对话框域：`draw_composite_editor` + `draw_composite_dialogs` + `composite_new_dialog` / `composite_pack_dialog`，924 行）⑥ `ce_internal.h`（六 TU 内部共享声明）。**跨 TU 提升**：`g_ce_*` 画布全局 / 撤销栈（`g_ce_undo`/`g_ce_redo` + `CeUndoEntry`）/ `ce_push_snapshot` / `ce_remove_node` / `ce_apply_snap` / `ce_open_rename` / `ce_apply_rename` / `ensure_ce_context` 由匿名提升为 `auiwrite::ui` 直接可见（经内部头 extern 共享；**不进公共头 composite_editor.h，对外 API 不变**）。**行为等价验证**：构建 **0 error / 0 warning**（4 目标）· `--exec-selftest` **682/0** · `--graph-selftest` **151/0** · `--selftest` **十三组全 PASS**（与基准零升降）。**执行序**：`CLN-14`（main，先重写方向描述为「按 CLI 功能区拆」）→ `CLN-15`（api_probe，先跑基线存档）→ 驼峰化（CLN-06 扩项）。

> 🟢 **`CLN-14` 已落地（2026-10-20 · 审计报告 [../audit/audit_cleanup_14.md](../../audit/audit_cleanup_14.md)）**：`main.cpp`（2359 行 → **661 行**）**按 CLI 功能区拆**（审计方向修正后）—— ① `cli_selftest.cpp`（自检域：`run_selftest` 700 行最大块 + `official_generate_selftest` / `web_chat_selftest` / `web_session_selftest` / `vlm_selftest` / `provider_selftest` + `join_names` + `utf8_length`/`utf8_prefix`，1201 行 / ≈57 KB）② `cli_diag.cpp`（诊断/杂项域：`print_usage` / `pipe_selftest` / `web_session_list` / `print_spec_list` / `provider_dump` + `report_timestamp`/`read_text_file`/`write_text_file`，564 行 / ≈30 KB）③ `cli_internal.h`（三 TU 内部共享声明，40 行）。**跨 TU 提升**：匿名块 17 个符号提升为全局命名空间（经内部头 extern 共享，**不进公共头**）。**冲突排查无**：`join_names` 与 `aiwrite::ai` / `aiwrite::engine` 模块同名函数命名空间不同；`print_usage` 与 api_probe / webview2_login（独立 exe 目标）不冲突。**行为等价验证**：构建 **0 error / 0 warning**（4 目标）· `--exec-selftest` **682/0** · `--graph-selftest` **151/0** · `--selftest` **十三组全 PASS** · `aiwrite.exe --help` 正常（CLI 分派跨 TU 路径完好）。**执行序**：`CLN-15`（api_probe，先跑基线存档）→ 驼峰化（CLN-06 扩项）。

> 🟢 **`CLN-15` 已落地（2026-10-20 · 审计报告 [../audit/audit_cleanup_15.md](../../audit/audit_cleanup_15.md)）**：`api_probe.cpp`（6937 行 / ≈390 KB → **321 行 / ≈14 KB**）**按自检分节拆** —— ① `api_probe_selftest.cpp`（协议/网络/CLI 单测域 · 1866 行 / ≈101 KB）② `api_graph_selftest.cpp`（图模型自检段 · 736 行 / ≈37 KB）③ `api_executor_selftest.cpp`（执行器自检 P3-1 段 · 393 行 / ≈20 KB）④ `api_execution_selftest.cpp`（执行核心 P3-2 段 · `execution_selftest` ≈3500 行单函数 · 3624 行 / ≈220 KB）⑤ `api_diag.cpp`（诊断/断言域 · 482 行 / ≈24 KB）⑥ `api_internal.h`（跨 TU 内部共享声明：`Options` / `Check` + 断言辅助 + 13 个 `test_*` + 3 个自检入口 + 6 个诊断函数）。**跨 TU 提升**：匿名块 A（含嵌套块 B）+ 块 C 符号提升为全局命名空间（经内部头 extern 共享，**不进公共头**）；`set_param` 第 4 参 `nlohmann::json`（字符串/bool 实参兼容）。**行为等价验证**：构建 **0 error / 0 warning**（4 目标）· `--exec-selftest` **682/0** · `--graph-selftest` **151/0** · `--selftest` **十三组全 PASS**（与拆前基线存档 `build/_cln15_baseline/` **输出逐行一致**，`Compare-Object` 差异仅日志时间戳）· `check_links` 1679/0。**执行序**：§3.4 拆分**全部完成** → 驼峰化（CLN-06 扩项）。

> `webview_host.cpp`（≈159 KB）与 `dom_web_client.cpp`（≈138 KB）**本轮不拆**（高耦合运行时代码，拆分风险 > 收益），登记为后续优化项（§4）。

---

## §4 后续优化打磨（清理完成之后 · `OPL-*` 占位）

> 清理（§3）全部落地后再进入本节；本节只定**方向与顺序**，具体条目在清理收尾时按「干净地基上重新审视」的结果补 `OPL-*` 编号并逐条开批。

### 4.1 方向（按优先级）

| 顺序 | 方向 | 说明 | 承接/参考 |
|---|---|---|---|
| 1 | **恢复并完成暂停任务** | §2 的小地图导航排查（若 §3.3 合并未顺带解决，则回到 §2.3 恢复路径） | [../draft/CN_editor_refactor.md](../../draft/CN_editor_refactor.md) |
| 2 | **运行时性能** | 画布大图场景的绘制 / 快照 / 撤销重做性能；属性面板高频重绘 | `M_canvas_snapshot.md` 既有结论 + 实测基线 |
| 3 | **体验打磨** | 承接 `M_patchAB_rest.md` 的交互打磨残项（`PD-*` 未完成项） | [M_patchAB_rest.md](M_patchAB_rest.md) |
| 4 | **运行时代码拆分** | 清理阶段登记的 `webview_host` / `dom_web_client` 拆分（低优先级，谨慎做） | — |
| 5 | **测试资产完善** | 清理后补齐 `ce_repro` → 并入 `composite_selftest` 的回归断言；`CLN-15` 拆出的自检文件纳入 `check_all` | [check_all.ps1](../../../source/tools/check_all.ps1) |

### 4.2 打磨的执行纪律

- 与 §0.3 一致：**每批小步、每批验证**；性能项先立基线（`build/logs/` 存档）再动，动完对比只升不降。
- 打磨项若触发行为变更 ⇒ 走既有「待拍板」流程（`DevPlan.todo` 的「待定 · 未拍板」分类），不在本计划内自拍板。

---

## §5 验收标准（清理阶段）

| # | 验收项 | 判据 |
|---|---|---|
| `CLN-A1` | 全库可构建 | `source/build.ps1` **0 error / 0 warning**（含 `ce_repro` —— §2.3 的编译错误必须先修） |
| `CLN-A2` | 回归只升不降 | `api_probe --exec-selftest`（当前 **682/0**）· `--graph-selftest`（**151/0**）· `--selftest` 全组 PASS |
| `CLN-A3` | 一键自检 | `check_all.ps1` **16/16 PASS · exit 0**（当前口径） |
| `CLN-A4` | 文档链健康 | `check_links` broken **0**（新增文档引用全部有效） |
| `CLN-A5` | 重复归零 | 主画布 ↔ 上级编辑器的平行实现清点表：小地图 / 对齐辅助线 / 节点绘制 **无残留**（`CLN-09`~`CLN-11` 勾账） |
| `CLN-A6` | 台账归零 | §3.1 的 `TODO` / `FIXME` / `HACK` 台账清空或全部登记进 `DevPlan.todo` |
| `CLN-A7` | 登记完成 | AI_README §5（`CLN-*` 编号空间）· DocMAP · CHANGELOG · DevPlan.todo 四处登记（按库规） |
| `CLN-A8` | **删除前置审计闸门全过** | 每个删除批均有：审计报告（对象清单 + 引用证据 + 影响面/回滚点）+ 行动计划 → **用户人工审核通过结论**（留存批记录），方可勾账；无「未审核先删」 |

---

## §6 风险与「不做」清单

| # | 风险 / 边界 | 对策 |
|---|---|---|
| 1 | **删错「看着没用其实被测试/文档引用」的代码** | §0.3 第 2 条：先列引用证据再删；`--selftest` / `--exec-selftest` / `check_links` 兜底 |
| 2 | **合并小地图 / 吸附线时把上级编辑器差异主题弄丢** | §3.3 明确「只收敛机制、保留差异」；合并后用 GUI 目测（`CLN-V3`：上级编辑器打开 → 小地图蓝紫主题 + 导航可用） |
| 3 | **休眠栈被误伤**（Pydoll / Python / 管道 —— `M8-Q7` 拍板「不删」） | §0.2 写死边界；扫描脚本**排除** `source/python/**`、`web/pydoll_channel.*`、`pipe_client` |
| 4 | **拆分大文件引入行为漂移** | `CLN-15` 先跑基线存档再动；拆分批一律「纯搬移不改逻辑」+ 全量 selftest |
| 5 | **清理拖成「大爆炸」** | §0.3 第 1 条：一项一批；**超过 2 小时做不完的项拆小**，宁可多批 |
| 6 | **`ce_repro` 编译错误遗留** | §2.3 第 1 步 = 清理阶段**首个动作**（否则 `CLN-A1` 无法成立） |

---

## §7 变更记录

| 日期 | 版本 | 说明 |
|---|---|---|
| 2026-10-19 | v1 | **建立本文**：暂停任务状态登记（§2 · 上级编辑器小地图排查 + `ce_repro` mode 5 + 待修编译错误）· 清理清单 `CLN-01`~`CLN-15`（死代码 / 头文件一致性 / 重复实现合并 / 大文件拆分）· 后续优化打磨方向（§4）· 验收标准（§5）· 风险与不做清单（§6）。**零代码改动。** |
| 2026-10-19 | v1.1 | **增补「删除前置审计闸门」**（用户要求）：§0.3 新增第 6 条硬性原则（先审计报告 + 行动计划 → 人工审核通过才删）· 新增 §3.0 删除前置审计流程（六步：对象清点 / 引用取证 / 影响面 / 行动计划交审 / 审核闸门 / 落地登记）· §5 新增验收项 `CLN-A8` · §3 引言标注删除类项必走该流程。**零代码改动。** |
| 2026-10-20 | v1.2 | **恢复路径执行（清理批 0）**：修 `ce_repro.cpp` 编译错误（`std::fabs`→`std::abs` + 补 `<cmath>`）· 构建 0/0 · `ce_repro.exe 5` 复跑 = **FAIL**（`NavigateToCenter` 已调用但视图未移动，结论已登记 §2.2）· §0 状态与文末状态改为「已开工」。**代码改动：1 文件（`ce_repro.cpp`，仅编译修复、零行为改动）。** |
| 2026-10-20 | v1.3 | **`CLN-01` 审计完成（清理批 1）**：全库死代码扫描（163 文件，排除 third_party/python）→ **无删除项**（static 未引用 0 · `#if 0` 0 · 非 static 函数 3 项全核验非死代码）· 产出 [../audit/audit_cleanup_01.md](../../audit/audit_cleanup_01.md)（含扫描器 2 个 bug 修复记录：`//` 注释未剥除 / C++14 数字分隔符误判）· §3.1 勾账 **且人工已确认（2026-10-20）**。**代码改动：0（纯审计）。** |
| 2026-10-20 | v1.4 | **`CLN-02` 审计 + 删除（清理批 2）**：全库 include 符号级扫描（225 文件，排除 third_party/python；扫描器过合成阳性对照）→ **未用项目头 include 0** · **删除 `tools/api_probe.cpp` 2 行重复 include**（第 2 处 `paths.h` 原 L39 / 第 2 处 `webview_host.h` 原 L52，保留首处）· 构建 **0 error / 0 warning**（CLN-02 判据「靠构建日志」）+ **selftest 全量回归**（`--selftest` 13 组 PASS · `--exec-selftest` 682/0 · `--graph-selftest` 151/0）· `check_links` **1598 / 0** · 产出 [../audit/audit_cleanup_02.md](../../audit/audit_cleanup_02.md)（含扫描器 3 个 bug 修复记录：Python 关键字集误用 / 预处理行污染导出集 / 数字分隔符判定移位二次引入）· §3.1 勾账 **且人工已确认（2026-10-20）**。**代码改动：1 文件（`api_probe.cpp`，仅删 2 行重复 include）。** |
| 2026-10-20 | v1.5 | **`CLN-03` 审计完成（清理批 3）**：全库 225 文件扫描失效分支 / 注释掉的代码块（`#if 0` **0** · 注释代码块候选 **13 块全部核验为文档注释** · 单行注释语句 **0**）→ **无删除项** · 产出 [../audit/audit_cleanup_03.md](../../audit/audit_cleanup_03.md)（扫描器过合成正/负对照；修复行注释跨度闭合 bug 1 个）· §3.1 勾账 **且人工已确认（2026-10-20）**。**代码改动：0（纯审计）。** |
| 2026-10-20 | v1.6 | **`CLN-04` 台账完成（清理批 4）**：全库 163 个 `.cpp/.h` 扫描 `TODO` / `FIXME` / `HACK`（双扫描器互证 + 合成正例对照）→ **真标记 0**（11 处命中全为 `xxx-web` 占位符 / 正文「真 bug」误报；中文等价词全为功能状态与运行时文案，不入台账；`third_party` 上游标记属硬边界不碰）→ **台账初始即归零 · 无删除项 · 无登记项** · 产出 [../audit/audit_cleanup_04.md](../../audit/audit_cleanup_04.md) · §3.1 勾账 **待人工确认（2026-10-20）**。**代码改动：0（纯审计）。** |

| 2026-10-20 | v1.7 | **`CLN-05` 重复辅助函数收敛（清理批 5）**：5 组真重复 14 处定义收敛到 `utils/string_util.{h,cpp}` 单一来源（`findCi` / `trim` / `nowString` / `toLower` / `narrow`，驼峰命名先导）· 删 14 定义 + 改 55 调用点（12 文件）· 构建 **0/0** · selftest 全绿（13 组 PASS / 682 / 151）· `check_links` 1610/0 · 产出 [../audit/audit_cleanup_05.md](../../audit/audit_cleanup_05.md) · §3.1 勾账。**按用户指示新开待决：全库函数/变量统一驼峰化（暂记 `CLN-06` 扩项）。**
| 2026-10-20 | v1.8 | **`CLN-09` + `CLN-10` 小地图/对齐吸附共享化（清理批 9+10）+ `CLN-11` 复核（清理批 11）**：小地图 4 函数 + `MinimapState`、对齐吸附 3 函数 + `SnapResult` 收敛到 `canvas_view.{h,cpp}` 单一来源 · 删上级 `ce_minimap_rect/hit/block/draw` / `ce_find_dragged_node` / `ce_apply_alignment_snap` / `ce_draw_snap_lines` / `CeSnapResult` + 主画布本地重复实现 7 符号 · `g_*_minimap_*` 8 全局收敛 2 个 `MinimapState` · 保留两套主题 / 节点块色回调 / 辅助线色宽 / Alt 互斥 / 网格 16px 吸附 / `kCeSnapDistance=8` / 开关 · 产出 [../audit/audit_cleanup_09_10.md](../../audit/audit_cleanup_09_10.md)（§⑤ 人工审核通过后落地）· §3.3 勾账 · `CLN-11` 无残留勾账 · **构建 0 error / 0 warning · selftest 全绿**（13 组 PASS / 682 / 151）· `check_links` 1613/0。**代码改动：4 文件（`canvas_view.h` / `canvas_view.cpp` 增共享层 · `node_canvas.cpp` / `composite_editor.cpp` 删重复 + 改调用点）。** |
| 2026-10-20 | v1.9 | **`CLN-06`~`CLN-08` 纯审计勾账 + `CLN-12`~`CLN-15` 拆分可行性审查勾账（清理批 6~8 审查 + §3.4 审查）**：§3.2 三项零改造勾账（头文件纯声明 / `ce_*` 无泄漏 / 告警 0 且 `/W4` 全开）· §3.4 四项拆分可行性全部审查完成**未落地**（`property_panel` 可拆 · `composite_editor` 可拆收益递减 · `main` 可拆但**方向修正**：主循环已不在 main.cpp → 改按 CLI 功能区拆 · `api_probe` 可拆含 3500 行单函数）· **`CLN-06` 扩项（全库驼峰化）= 无法决断，跳过留 ⛔ 人工审查标记** · 产出 [../audit/audit_cleanup_06_07_08.md](../../audit/audit_cleanup_06_07_08.md) · **代码改动：0（纯审查 + 文档登记）。** |
| 2026-10-20 | v1.10（当前） | **两项待决拍板登记（零代码改动）**：`asyncMode()`/`eventsPushed()` 观察项 **用户拍板 = 保留**（公共契约 · 观察项关闭 · [../audit/audit_cleanup_01.md](../../audit/audit_cleanup_01.md) ②-E/④/⑤ 更新）· mode 5 导航排查 **用户拍板 = 仅文档推进**（§2.2 补导航链路全拆解 + 4 根因候选 H1~H4 · 待真实 GUI 复核）· 剩余待决：⛔ `CLN-06` 扩项驼峰化 · §3.4 拆分落地批次。**代码改动：0（纯登记）。** |
| 2026-10-20 | v1.16（当前） | **驼峰化（`CLN-06` 扩项）批 2~8 全部落地**：批 2 = engine 域 48 函数 → camelCase（315 替换 / 35 文件）· 批 3 = nodes 域无公共 snake_case 跳过 · 批 4 = ai 域 123 函数（918 替换 / 62 文件 · `join_names` 命名空间隔离无冲突）· 批 5 = web 域 72 函数（544 替换 / 32 文件）· 批 6 = ui 域 145 函数（734 替换 / 48 文件）· 批 7/8 = app/tools 域无公共头跳过 · **合计 469 个函数 · 3061 次替换 · 237 文件** · 每批构建 **0/0** · selftest **682/151/13 组全绿**（输出零内容差异）· `check_links` 1684/0。**代码改动：237 文件（5 域头/实现 + 全库调用点）。**
| 2026-10-20 | v1.15（当前） | **驼峰化（`CLN-06` 扩项）批 1 = utils 域落地**：81 个 snake_case 公共函数 → camelCase（asset_store 12 / config 7 / credential 11 / diagnostics 6 / file_dialog 4 / log 4 / output_archive 1 / paths 23 / settings_validation 1 / text_export 8 / webview2 2 / window_geometry 1）· 全库定义 + 调用点 **550 次替换 / 60 文件** · 脚本 = C++ 感知状态机（跳过注释 / 字符串 / raw string + **C++14 数字分隔符** `60'000`）· **构建 0 error / 0 warning（4 目标）· selftest 全绿**（682 / 151 / 13 组 PASS · 输出零内容差异）· `check_links` 1684/0。**代码改动：60 文件（utils 头/实现 + 全库调用点）。** |
| 2026-10-20 | v1.14（当前） | **`CLN-15` 拆分落地（清理批 §3.4 · 最后一拆）**：`api_probe.cpp`（6937 行 / ≈390 KB → **321 行 / ≈14 KB**）**按自检分节拆** —— `api_probe_selftest.cpp`（协议/网络/CLI 单测 · 1866 行 / ≈101 KB）· `api_graph_selftest.cpp`（图模型 · 736 行 / ≈37 KB）· `api_executor_selftest.cpp`（执行器 P3-1 · 393 行 / ≈20 KB）· `api_execution_selftest.cpp`（执行核心 P3-2 · `execution_selftest` 单函数 · 3624 行 / ≈220 KB）· `api_diag.cpp`（诊断/断言 · 482 行 / ≈24 KB）· `api_internal.h`（跨 TU 共享声明）· 匿名块 A（含嵌套 B）+ C 符号提升全局命名空间（内部头 extern · **不进公共头**）· **基线存档先跑再动**（`build/_cln15_baseline/`）· **构建 0 error / 0 warning（4 目标）· selftest 全绿**（682 / 151 / 13 组 PASS · 输出逐行一致，差异仅日志时间戳）· `check_links` 1679/0 · 产出 [../audit/audit_cleanup_15.md](../../audit/audit_cleanup_15.md) · §3.4 勾账（拆分全部落地）。**代码改动：5 新 cpp + 1 新头 + 2 改（CMakeLists / api_probe.cpp 重建）。** |
| 2026-10-20 | v1.13（当前） | **`CLN-14` 拆分落地（清理批 §3.4 · 方向修正后）**：`main.cpp`（2359 行 → **661 行**）**按 CLI 功能区拆** —— `cli_selftest.cpp`（自检域 · 1201 行 / ≈57 KB）· `cli_diag.cpp`（诊断/杂项域 · 564 行 / ≈30 KB）· `cli_internal.h`（三 TU 共享声明 · 40 行）· 匿名块 17 符号提升全局命名空间（内部头 extern · **不进公共头**）· 冲突排查无（join_names 命名空间隔离 · print_usage 独立 exe 目标）· **构建 0 error / 0 warning（4 目标）· selftest 全绿**（682 / 151 / 13 组 PASS）· `aiwrite.exe --help` 正常 · 产出 [../audit/audit_cleanup_14.md](../../audit/audit_cleanup_14.md) · §3.4 勾账。**代码改动：2 新 cpp + 1 新头 + 2 改（CMakeLists / main.cpp 重建）。** |



---

**文档结束。**
**状态：🟡 已开工（2026-10-20）· 恢复路径完成 + `CLN-01`~`CLN-05` 均勾账 + `CLN-09`/`CLN-10` 共享化落地 + `CLN-11` 复核勾账（[../audit/audit_cleanup_09_10.md](../../audit/audit_cleanup_09_10.md)）+ `CLN-06`~`CLN-08` 纯审计零改造勾账 + `CLN-12`~`CLN-15` 拆分可行性审查勾账（未落地 · [../audit/audit_cleanup_06_07_08.md](../../audit/audit_cleanup_06_07_08.md)）；**已拍板（2026-10-20）**：asyncMode = 保留（观察项关闭 · [../audit/audit_cleanup_01.md](../../audit/audit_cleanup_01.md)）· mode 5 = 仅文档推进（导航链路 + 4 根因候选登记 §2.2 · 待真实 GUI 复核）；**剩余待人工审查**：⛔ `CLN-06` 扩项全库驼峰化（§3.4 拆分完成后执行）；**拆分落地进度**：`CLN-12`（property_panel → panel_widgets/panel_web/panel_internal.h · [../audit/audit_cleanup_12.md](../../audit/audit_cleanup_12.md)）+ `CLN-13`（composite_editor → ce_state/ce_session/ce_run/ce_pack/ce_draw + ce_internal.h · [../audit/audit_cleanup_13.md](../../audit/audit_cleanup_13.md)）+ `CLN-14`（main → cli_selftest/cli_diag + cli_internal.h · 按 CLI 功能区拆 · [../audit/audit_cleanup_14.md](../../audit/audit_cleanup_14.md)）+ `CLN-15`（api_probe → api_probe_selftest/api_graph_selftest/api_executor_selftest/api_execution_selftest/api_diag + api_internal.h · 按自检分节拆 · 基线存档 682/151/13 组输出逐行一致 · [../audit/audit_cleanup_15.md](../../audit/audit_cleanup_15.md)）均 0/0 + selftest 全绿；**§3.4 拆分全部完成** → **驼峰化（CLN-06 扩项）推进中**：批 1（utils 域）已落地（81 函数 → camelCase · 550 替换 / 60 文件 · 构建 0/0 · selftest 682/151/13 全绿 · 输出零内容差异）+ 批 2（engine 域）已落地（48 函数 → camelCase · 315 替换 / 35 文件 · 构建 0/0 · selftest 682/151/13 全绿）+ 批 3（nodes 域）无公共 snake_case 跳过；**剩余**：批 4 ai → 批 5 web → 批 6 ui → 批 7 app → 批 8 tools。**
