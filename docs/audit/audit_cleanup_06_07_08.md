# CLN-06 / CLN-07 / CLN-08 审计报告（头文件与结构一致性 · 纯审计批次）

> 行动计划：`docs/actionPlan/M_code_cleanup.md` §3.2（`CLN-06`~`CLN-08`）
> 审计日期：2026-10-20 · 方式：**纯审计，零代码/零结构改造**（用户指示「继续审查，暂时都不进行改造」）
> 范围：`source/**`（排除 `third_party` / `vcpkg-installed` / `build`）
> 附带：§3.4（`CLN-12`~`CLN-15` 大文件拆分）**现状登记**（只登记，不拆分）

---

## ① 审查对象与证据

### CLN-06 公共头只放声明 / 头文件自包含

| 头文件 | 行数 | 内容构成 | inline 大函数 | 非 constexpr 静态数据 | 判定 |
|---|---|---|---|---|---|
| `canvas_view.h` | 201 | `#pragma once` + include + 结构体（`PinRef` / `CanvasTheme` / `CanvasContext` / `MinimapState` / `SnapResult` / `NodeResultView`）+ 函数声明 + `constexpr` 常量 | **无** | **无** | ✅ 符合 |
| `composite_editor.h` | 102 | 同上（`CompositeEditorState` 结构 + 函数声明） | **无** | **无** | ✅ 符合 |
| `node_canvas.h` | 30 | 同上（`CanvasOptions` 结构 + 3 函数声明） | **无** | **无** | ✅ 符合 |
| `property_panel.h` | 36 | 同上（`PropertyEditResult` 结构 + 函数声明） | **无** | **无** | ✅ 符合 |

- `constexpr` 常量（`kNodeContentWidthMin` 等）为编译期常量，放头文件是 C++ 惯例，**不属于**「实现体」。
- `CanvasContext` 内 `std::function` 成员为数据成员（声明），非函数实现体。
- **自包含静态证据**（每个头 include 覆盖其使用类型）：

| 头文件 | include 列表 | 覆盖使用类型 |
|---|---|---|
| `canvas_view.h` | `functional` / `string` / `unordered_map` / `executor.h` / `graph.h` / `imgui.h` / `imgui_node_editor.h` | `ImU32`/`ImVec2`/`ImDrawList`（imgui.h）· `ed::NodeId`（imgui_node_editor.h）· `engine::Node`/`Graph`/`PortDirection`（graph.h）· `engine::RunSnapshot`（executor.h）· `std::function`/`string`/`unordered_map` ✅ |
| `composite_editor.h` | `array` / `string` / `composite.h` / `executor.h` / `undo_stack.h` | `engine::CompositeSpec`（composite.h）· `engine::UndoStack`（undo_stack.h）· `engine::Executor`/`RunSnapshot`（executor.h）· `std::array`/`string` ✅ |
| `node_canvas.h` | `string` | `std::string`（自身 `CanvasOptions` 本头定义）✅ |

- **编译佐证**：构建 **0 error / 0 warning**，所有真实包含场景编译通过。

**结论：CLN-06 无实现体需移入 .cpp、无删除项 → 勾账（无改造）。**

### CLN-07 命名与分层一致性

**`ce_*` 前缀泄漏检查**（全 `source/ui/**` + 全库 TU）：

| 文件 | `ce_` 命中数 | 性质 |
|---|---|---|
| `composite_editor.cpp` | 88 | 本 TU 定义/使用（`ce_apply_snap` / `ce_snap` / `ce_show_grid` / `ce_minimap` 等）✅ 合法 |
| `canvas_view.cpp` | 1 | **imgui 控件 ID 字符串** `"##ce_inline_"`（非 C++ 符号，见下） |
| 其余全部 TU | 0 | 无泄漏 |

- `composite_editor.h` 对外 API 全部用 **长前缀** `composite_editor_*` / `composite_*`（`composite_editor_state` / `composite_editor_open` / `composite_pack_selection` …），与内部 `ce_*` 短前缀**分层清晰**。
- `ce_*` 短前缀**无 header 声明**，仅 `composite_editor.cpp` 内部可见 → 其他 TU 无法引用 → **无泄漏**。
- **命名观察点（非违规）**：`canvas_view.cpp` L582 的 `"##ce_inline_"` 是 `ImGui::InputTextMultiline` 的控件 ID 标签（`##` 为 imgui 隐式 ID 分隔符，`ce_inline_` + node.id 保证节点级唯一）。其「`ce_`」字样与 composite_editor 前缀同形，属**借用命名**——非 C++ 符号、不参与链接、不影响功能。**可选建议**：后续统一为中性前缀（如 `##inline_`）消除歧义；不属本批必须。

**文件级职责注释抽查**（与头部约定一致）：

| 文件 | 头部形式 |
|---|---|
| `canvas_view.cpp` / `node_canvas.cpp` / `composite_editor.cpp` / `property_panel.cpp` | 首行 `#include` 自身头 + 逐行 include 用途注释（`// CN_editor_refactor：共享只读绘制层` 等）✅ |
| `main.cpp` / `api_probe.cpp` | 独立可执行带 banner 职责注释（`// AIwrite 主程序入口` / `// api_probe —— M1 技术验证命令行工具`）✅ |

**结论：CLN-07 无命名/分层违规 → 勾账（含 1 个可选命名观察点，不强制）。**

### CLN-08 编译告警清零

| 判据 | 证据 | 达成 |
|---|---|---|
| 非 third_party warning 逐条清零 | 构建日志 `build/logs/build_debug_20261020_151017.log`：**warning: 0 / error: 0**（全量，含 third_party 在内亦 0） | ✅ |
| `/W4` 开启 | `source/CMakeLists.txt` L325（`aiwrite_core`）/ L389（`aiwrite`）/ L409（`api_probe`）/ L436（`webview2_login`）均 `target_compile_options(... PRIVATE /W4)` | ✅ |

**结论：CLN-08 判据已达成 → 勾账（无改造）。**

### §3.4 现状登记（CLN-12~CLN-15 · 只登记，不拆分）

> 改造型批次（拆分），按用户指示**本轮不执行**，登记现状供后续批次决策。

| # | 文件 | 现状 | <60 KB 判据 | 备注 |
|---|---|---|---|---|
| `CLN-12` | `ui/src/property_panel.cpp` | 95313 B（≈93 KB） | ❌ 超 | 按「参数控件类型」拆候选 |
| `CLN-13` | `ui/src/composite_editor.cpp` | 72977 B（≈71 KB） | ❌ 超 | **CLN-09/10 已从 ≈83 KB 降至 ≈71 KB**；按「画布/面板/状态/沙箱」拆候选 |
| `CLN-14` | `app/src/main.cpp` | 124939 B（≈122 KB） | ❌ 超 | 拆「窗口/输入/主循环」与「启动自检/CLI」候选 |
| `CLN-15` | `tools/api_probe.cpp` | 399049 B（≈390 KB） | ❌ 超 | 唯一「先跑基线 → 拆后逐字节不变」批次 |
| — | `webview_host.cpp` / `dom_web_client.cpp` | ≈159 KB / ≈138 KB | — | 计划明确「本轮不拆」（§3.4 注） |

---

## ② 影响面评估

- 本批 **零代码改动** → 无行为变化、无回滚点需求、无契约影响。
- §3.4 现状登记不改变任何文件 → 无影响面。
- 审计结论如经人工确认勾账，后续仅做文档标记（`M_code_cleanup.md` §3.2 勾账 + §7 changelog）。

---

## ③ 行动计划（审查后勾账）

| 项 | 审查结论 | 处置 |
|---|---|---|
| `CLN-06` | 无改造项（头文件纯声明，见 §①） | ✅ **勾账** |
| `CLN-07` | 无违规（`##ce_inline_` 命名观察点**可选**后续统一） | ✅ **勾账** |
| `CLN-08` | 判据已达成（构建 0/0 + `/W4` 四 target 开启） | ✅ **勾账** |
| `CLN-12`~`CLN-15` | 拆分可行性已审查（见 §⑤），方案就绪 | ✅ **勾账（审查项）** · 落地拆分=改造批次，留待人工批准执行 |
| `CLN-06` 扩项（驼峰化） | **无法决断**（见 §⑤-c） | ⛔ **跳过 · 留人工审查标记** |

---

## ④ 审核闸门

- [x] ⛔ 待人工审核（**通过前不做任何代码/结构改造**）
- [x] ✅ 通过 → 在 `M_code_cleanup.md` 勾账 + changelog 登记（**2026-10-20 用户指示「直接审查剩下所有内容并勾账」——授权文档勾账；仍不落地任何代码/结构改造**）
- [ ] 需修改 → 修改后重新提交

*审核意见：* **2026-10-20 用户授权「继续审查下一批，直接审查剩下所有的内容并且审查完后进行勾帐；无法决断的内容列出并跳过、留人工审查标记」。§3.2 三项勾账；§3.4 审查项勾账（落地留待后续改造批次）；CLN-06 扩项（驼峰化）跳过留标记。**

---

## ⑤ CLN-12~CLN-15 拆分可行性审查（本轮延伸）＋ CLN-06 扩项待决

### ⑤-a CLN-12 `property_panel.cpp`（95313 B · ≈93 KB）

**结构分布**（函数/区块）：

| 区段 | 行范围 | 内容 |
|---|---|---|
| 基础工具 | 56~118 | `param_error` / `draw_input_port_summary` / `help_marker` / `param_label` |
| 网页版会话 | 119~466（≈350 行） | `is_web_mode` / `site_cookie_name` / `logout_web_session` / `delete_web_profile` / `draw_web_session_section` |
| 参数写入/文本 | 458~531 | `write_param` / `string_list_to_text` / `valid_utf8` |
| 控件分派核心 | 782~1239（≈460 行） | `draw_param_widget`（按 `ParamType` 分派各控件绘制的巨型函数） |
| 面板组装 | 1239~ | `draw_property_panel`（主入口） |

**结论：可拆 ✅** —— 拆分边界相对清晰（参数控件 widget 层 / 网页会话区 / 面板组装层）。`draw_param_widget`（460 行）为核心分派函数，拆分需把各控件类型绘制逻辑抽为独立子文件（如 `panel_widgets.*` / `panel_enum.*`），注意其引用参数（`begin_edit` / `changed_now`）传参设计。**风险中低。落地属改造批次。**

### ⑤-b CLN-13 `composite_editor.cpp`（72977 B · ≈71 KB）

**结构分布**（区块注释高度模块化）：

| 区段 | 行范围 | 内容 |
|---|---|---|
| 画布辅助（G2~G8） | 34~165 | 改名 / 小地图 / 对齐吸附 / 网格 |
| 会话状态 + 撤销 | 166~235 | 单例 + `ce_push_snapshot` / `ce_undo` / `ce_redo` |
| 会话操作 | 236~431 | open / save / add_node / delete_node / add_edge |
| 试运行「执行连线」 | 432~645 | start / stop / clear / tick / snapshot |
| 打包选区 | 647~791 | `composite_pack_selection` |
| 编辑器窗口 | 792~1521（`draw_composite_editor` ≈660 行） | 画布 + 规格面板 + Delete 键处理 |
| 新建/打包对话框 | 1522~1666 | run_new / run_pack / test_drive |

**结论：可拆、收益递减 ✅** —— 区块已高度自组织（§3.4 原始方向「画布/面板/状态/沙箱」与现状一致），`CLN-09/10` 已从 ≈83 KB 降至 ≈71 KB；进一步拆分主要是把 `draw_composite_editor` 660 行细化。**风险低、优先级低。落地属改造批次。**

### ⑤-c CLN-14 `main.cpp`（124939 B · ≈122 KB · 2359 行）

**⚠️ 前提修正（重要审查发现）**：CLN-14 原始拆分方向「拆『窗口/输入/主循环』与『启动自检/CLI』」**已过时**——窗口/主循环/绘制代码**不在 main.cpp**（经全库定位：承载于 `platform_glfw.cpp` / `platform_win32.cpp` / `webview_host.cpp` / `webview2_login.cpp`）。`main.cpp` 主体 = **CLI 参数解析 + 自检/诊断分派**：

| 区段 | 行范围 | 内容 |
|---|---|---|
| 基础工具 | 56~152 | 时间戳 / 文件读写 / UTF-8 口径 / usage |
| 自检/诊断区 | 236~1757 | `official_generate_selftest` / `run_selftest`（≈700 行）/ `web_chat_selftest` / `pipe_selftest` / `web_session_selftest` / `vlm_selftest` / `provider_*` |
| 入口 | 1758~2359 | `main()` = 参数解析 + 分派（GUI 启动最终落到 `ui::App`） |

**结论：可拆，但方向须修正 ✅（审查项）** —— 修正方向 = 按 **CLI 功能区**拆独立文件（如 `cli_selftest.*` / `cli_diag.*`），`run_selftest`（700 行）为最大块。**落地属改造批次，且拆前需重写 §3.4 的 CLN-14 描述。**

### ⑤-d CLN-15 `api_probe.cpp`（399049 B · ≈390 KB · 6729 行）

**结构分布**（selftest 分节边界清晰）：

| 区段 | 行范围 | 内容 |
|---|---|---|
| V-11~V-16 协议/网络 | 171~1199 | `test_nokey_request_shape` / `test_cancel_and_timeout` / `test_protocol_requests` / `test_factory_and_capability` / `test_network_and_credentials` / `test_protocol_loopback` |
| 基础/配置/会话 | 1227~1888 | `test_sha3` / `test_chat_request_shape` / `test_config_roundtrip` / `test_web_session_and_visibility` / `test_workflow_files` / `test_http` / `test_chat` |
| 图模型自检 | 1889~2524 | `graph_selftest`（≈600 行） |
| 执行器自检 | 2525~2816 | `executor_selftest` |
| **执行自检** | 2817~6338 | **`execution_selftest`（≈3500 行单函数！）** |
| 诊断/断言 | 6339~6717 | `image_decode_report` / `test_diag_report` / `test_dep_check` / `test_settings_assert` / `test_site_assert` / `test_autosave_assert` |
| 入口 | 6718~ | `main()` 分派 |

**结论：可拆 ✅** —— 分节边界清晰，与 §3.4 方向「selftest 各节拆独立文件」一致；`execution_selftest` 3500 行单函数为最大拆分对象（需再细分）。判据「行为/输出逐字节不变」需先跑基线存档（`--selftest` 13 组 / `--exec-selftest` 682 / `--graph-selftest` 151 输出为基线）。**风险中（基线保护）· 工作量大 · 落地属改造批次。**

### ⑤-e CLN-06 扩项（全库函数/变量统一驼峰化）—— ⛔ 无法决断，跳过留人工审查

- 来源：`CLN-05` 勾账行「按用户指示（2026-10-20）：全库函数/变量统一驼峰化为新一批（暂记 `CLN-06` 扩项），本批 5 函数已作先导样板」。
- 审查结论：**无法决断**。理由：
  1. **改动面未量化**：全库函数/变量驼峰化 = 全库 `.cpp/.h` 命名 + 全部调用点 + 文档同步，量级远超本批（CLN-05 仅 5 函数已涉及 55 调用点 / 12 文件）。
  2. **收益为主观决策**：命名一致性收益 vs 大改回归风险，属**用户偏好拍板项**（类似 asyncMode 待决），非技术可决断。
  3. **节奏未定**：一次大爆炸 vs 分函数批次推进（CLN-05 先导），需用户定范围/顺序。
- **处置：跳过 · 留 ⛔ 人工审查标记**（登记至 `M_code_cleanup.md` §3.2 待决清单）。

---

*（本文档为纯审计产出物；用户指示「继续审查，暂时都不进行改造」，故不含任何删除/移动/拆分。§③ 授权勾账仅限文档登记。）*
