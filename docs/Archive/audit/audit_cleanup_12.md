# audit_cleanup_12：CLN-12 落地审计报告（property_panel 拆分）

> 批次：`CLN-12` · 日期：2026-10-20 · 计划：docs/actionPlan/M_code_cleanup.md §3.4
> 性质：**超大文件拆分（可读性）** —— 不改变任何行为，仅分文件。
> 判据：不改变行为；单文件 < 60 KB；构建 0/0 + selftest 全绿。

## 1. 拆分对象与方案

| 项 | 值 |
|---|---|
| 源文件 | `source/ui/src/property_panel.cpp`（1932 行 / ≈93 KB） |
| 拆分方向 | 按「功能域」拆（审计报告 audit_cleanup_06_07_08 §⑤ 建议按控件类型；落地时进一步细化为一域一文件） |
| 落地文件 | `panel_widgets.cpp`（参数控件域）· `panel_web.cpp`（网页会话 + 提供商下拉域）· `panel_internal.h`（三 TU 内部共享声明） |

### 拆分后文件

| 文件 | 行数 | 内容 |
|---|---|---|
| `source/ui/src/property_panel.cpp` | 814 | 面板主域：`draw_property_panel` + `param_error` / `draw_input_port_summary` / `help_marker` / `lower_copy` / `web_ignored_in_web` / `draw_prompt_generate_button` |
| `source/ui/src/panel_widgets.cpp` | 598 | 参数控件域：`draw_param_widget` + `filters_for` / `param_label` / `write_param` / `string_list_to_text` / `string_list_from_text` / `valid_utf8` / `import_text_file` / `kSliderLimit` 常量 |
| `source/ui/src/panel_web.cpp` | 476 | 网页会话 + 提供商域：`is_web_mode` / `web_site_context` / `draw_web_session_section` / `web_twin_of` / `is_merged_web_id` / `build_provider_choices` / `sync_provider_id_with_mode` |
| `source/ui/include/panel_internal.h` | 54 | 内部共享声明：`ProviderChoice` + 5 个跨 TU 函数 |

## 2. 依赖分析与共享接口

- **使用矩阵**（grep 统计 draw_property_panel / draw_param_widget 区域对辅助的调用）：
  - 仅 panel 用：`param_error` / `draw_input_port_summary` / `help_marker` / `lower_copy` / `web_ignored_in_web` / `draw_prompt_generate_button`
  - 仅 widget 用：`write_param`(17) / `string_list_*` / `valid_utf8` / `import_text_file` / `filters_for` / `kSliderLimit`
  - 跨 TU：`param_label`（panel 1 + widget 1）· `is_web_mode` · `draw_web_session_section` · `build_provider_choices` · `sync_provider_id_with_mode` → 全部经 `panel_internal.h` 声明。
- `ProviderChoice` / `WebSiteContext` 只在 web 域内部使用（web_twin_of / build_provider_choices 等）；`WebSiteContext` 保持 panel_web.cpp 匿名。

## 3. 落地方式与等价性保证

- **纯物理搬移**：新文件内容由原 property_panel.cpp 按行区间提取（PowerShell 数组切片，逐字节保留），未改动任何函数体 / 注释。
- **最小结构调整**：① 各新文件补 `using engine::Node / Param / ParamType`（原文件 L43-45，搬移后需在新 TU 显式声明）；② `ProviderChoice` 定义从 cpp 上移到 `panel_internal.h`（跨 TU 返回类型需要完整定义）；③ 命名空间归属调整 —— `is_web_mode` / `draw_web_session_section` / `build_provider_choices` / `sync_provider_id_with_mode` 由匿名提升为 `aiwrite::ui` 直接可见（跨 TU）。
- **CMakeLists**：`aiwrite` 目标新增 `ui/src/panel_widgets.cpp`、`ui/src/panel_web.cpp`。

## 4. 验证结果

| 验证 | 结果 |
|---|---|
| `build.ps1`（默认全目标） | ✅ **0 error / 0 warning**（aiwrite / api_probe / ce_repro / webview2_login 4 目标） |
| `api_probe --exec-selftest` | ✅ **682 通过 / 0 失败**（与基准一致） |
| `api_probe --graph-selftest` | ✅ **151 通过 / 0 失败**（与基准一致） |
| `api_probe --selftest` | ✅ **十三组全 PASS**（SHA3 / HTTP / 请求构造 / 配置往返 / 会话与可见性 / 执行器 / 工作流文件 / 无 Key / 取消与超时 / L2×3 / 网络与凭据） |
| 单文件体积 | property_panel.cpp ≈93 KB → 814 行（远低于 60 KB 目标） |

## 5. 回滚点

- 新文件均可删除 + `git checkout -- source/ui/src/property_panel.cpp source/CMakeLists.txt` 即回滚到拆分前单文件形态；函数名 / 语义零变化，无数据风险。
- ⚠️ 本次为纯拆分，**未改任何功能行为**；后续批（CLN-13/14/15）同法推进。

## 6. 备注

- 首轮构建失败（`Node`/`Param` 简名未解析）→ 补 `using engine::Node/Param/ParamType` 后 0/0；此为搬移类拆分的典型修补，非语义问题。
- 下一步：`CLN-13`（composite_editor.cpp）→ `CLN-14`（main.cpp，先重写方向描述为「按 CLI 功能区拆」）→ `CLN-15`（api_probe.cpp，先跑基线存档）。
