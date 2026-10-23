# 审计报告 · audit_cleanup_04 —— CLN-04 TODO / FIXME / HACK 台账

> 批次：`CLN-04`（对应 [../actionPlan/M_code_cleanup.md](../Archive/actionPlan/M_code_cleanup.md) §3.1）· 日期：2026-10-20
> 性质：**文档级台账批次（无删除闸门）** —— 全库收集 → 逐条判定（已实现删标 / 仍有效登记进 `DevPlan.todo` / 已无意义删）→ **台账归零**。
> 范围：`source/**`（**163 个 .cpp/.h**，与 CLN-01 口径一致：229 全库 − 65 third_party − 1 python），排除 `third_party/**`（vendored，硬边界 §6）与 `source/python/**`（Pydoll 休眠栈）。
> 方法：自研静态扫描脚本（临时工具，不入库，见 §5）+ 合成正例对照验证 + 双扫描器互证 + 逐条人工核验。

---

## ① 对象清点（台账收集）

| 扫描维度 | 方式 | 结果 | 判定 |
|---|---|---|---|
| A. 注释内 `TODO` / `FIXME` / `HACK`（含 `XXX`/`TBD`/`BUG`/`FIX` 辅助词） | 逐字符状态机提取注释 → 词边界匹配（大小写不敏感） | **命中 11 处 → 全部核验为误报**（见 ②） | 真标记 **0** |
| B. 全仓库独立词 `TODO/FIXME/HACK/TBD`（大小写敏感，含代码/注释/字符串全文本） | 全文件正则 | 项目自有 source/** **0** 命中（命中全在 third_party） | 互证 **0** |
| C. 中文等价标记（待办/待实现/尚未实现/未实现/未完成/待定/遗留/后续优化） | 全文件正则 | source/** 内命中全为**功能状态 / 运行时文案**（见 ②-C） | 非债务标记，**不入台账** |
| D. 文件覆盖完整性 | 扩展名统计 vs 扫描文件数 | 163 = 83 `.cpp` + 80 `.h`（另 .json/.ps1/.js/.cmake/.md 等全类型已扫） | 无盲区 |

## ② 引用取证（逐项核验）

### A 类 11 处命中 → 全部误报（原文见 `build/tmp/cln04_out.txt`，临时）

| # | 位置 | 命中词 | 原文定性 | 判定 |
|---|---|---|---|---|
| A1 | ai/include/upload_supply_probe.h L35 | XXX | `get_path` 示例占位符 `xxx?a=1&b=2` | 误报 · 占位符 |
| A2 | app/src/main.cpp L1314 | BUG | "回归「窗口已打开但内存无凭证」真 bug（2026-09-26）" —— 历史 bug 描述 | 误报 · 正文 |
| A3 | tools/api_probe.cpp L1542 | BUG | "会话自动引导决策（2026-09-26 真 bug 的回归断言）" —— 测试断言标题 | 误报 · 正文 |
| A4 | ui/src/node_canvas.cpp L1193 | BUG | "static：避免每帧重置（历史 bug 根因，见 CHANGELOG）" —— 修复原因注释 | 误报 · 正文 |
| A5–A9 | ui/src/property_panel.cpp L632–636 | XXX | `xxx-web` 站点条目合并规则说明（5 处） | 误报 · 占位符 |
| A10 | ui/src/property_panel.cpp L669 | XXX | 同名后缀规则 `xxx ↔ xxx-web` 说明 | 误报 · 占位符 |
| A11 | web/include/site_ref.h L161 | BUG | "教训（2026-09-26 真 bug）：窗口已打开 ≠ 内存里有凭证" | 误报 · 正文 |

**A 类真标记 = 0。** 库内不存在 `TODO:` / `FIXME:` / `HACK:` 债务标注写法。

### C 类中文词逐类核验（source/** 命中抽样）

| 命中语境 | 数量 | 定性 | 是否债务标记 |
|---|---|---|---|
| "协议/适配器**未实现**"（provider_factory.h / provider_spec.cpp / upload_registry.cpp / local_nodes.cpp / main.cpp） | ~20 | **运行时错误文案**（用户可见）与契约行为（可登录/可探测、生成待 L3） | 否 |
| "**尚未实现**（已实现：…）" 提示串 | ~10 | 诊断/自检输出文案 | 否 |
| selftest 断言含"未实现"（`upload_registry_selftest` / `provider_spec_selftest`） | ~6 | **测试断言**（校验文案契约） | 否 |
| python/daemon.py / protocol.py 内 NOT_IMPLEMENTED 词表 | ~8 | Pydoll 词表码（休眠栈，硬边界 §6） | 否（且在 python/** 排除范围） |
| site_capability_evidence.json | 4 | 能力证据数据（`"tested": false, "why": "协议未实现"`） | 否（数据文件） |

**C 类判定：** 这些"未实现"是**功能状态的事实陈述**（哪些协议没实现、运行时如何如实报错），是**契约的一部分**，不是"此处欠了一笔账要补"的债务标记。误入台账会把契约行为误当技术债，**不入台账**。

### third_party 排除项（供档案备查，不登记）

vendored imgui-node-editor / stb_image / imgui 内含大量上游 `#TODO` / `#FIXME` / `@TODO`（crude_json.cpp、imgui_node_editor.cpp、imstb_truetype.h、stb_image.h、imgui.cpp 等）。属 §6 硬边界"不碰 third_party"（vendored），且为**上游第三方标记**，非本库债务，**不登记、不删改**。

## ③ 影响面 / 回滚点

- 本批**无删除、无登记**（台账初始即归零）⇒ 无影响面、无行为变化、无需回滚点。

## ④ 行动计划（交人工确认）

1. `CLN-04` 勾账为 **「台账归零 · 无删除项 · 无登记项」**（文档级）。
2. 库内注释纪律已确认：无 `TODO/FIXME/HACK` 债务标注残留；既有"未实现"类文案均为契约/运行时语义，建议保持（不动）。
3. 无代码改动 ⇒ 构建 / selftest 与基线一致，无需重跑；`check_links` 随本报告 + 计划文档更新后跑（见 ⑥）。

## ⑤ 审核闸门

- CLN-04 为**文档级台账批次**（§3.1 判据「台账归零（文档级）」），无正式删除 ⇒ 无需 §3.0 删除闸门。
- 待用户确认：`CLN-04` 勾账（台账归零 · 无删除项）。

## ⑥ 落地与登记

- 无删除、无登记发生。勾账：`M_code_cleanup.md` §3.1 `CLN-04` 待标记 ✅（台账归零），登记 v1.6。
- 证据存档：扫描输出 `build/tmp/cln04_out.txt` / 合成对照 `build/tmp/tst/pos_cln04.cpp`（临时，不入库；如需留存可移入本目录）。

---

## §5 方法学与已知局限（如实登记）

- **扫描脚本**（临时）：`build/tmp/cln04_scan.py`（逐字符状态机提取注释 → 词边界匹配 `TODO|FIXME|HACK|XXX|TBD|BUG|FIX`，大小写不敏感）· `build/tmp/cln04_cross.py`（全文本 `\b(TODO|FIXME|HACK|TBD)\b` 大小写敏感 + 中文等价词）。复用 CLN-01/03 验证过的状态机（字符串/字符/数字分隔符正确处理）。
- **可信度验证（合成正例）**：`build/tmp/tst/pos_cln04.cpp` —— 行注释 `// TODO:` `// FIXME:` `// HACK:` + 块注释内 `TODO` **全部命中**；字符串 `"TODO 这是字符串"` 与字符 `'X'` **正确忽略** ⇒ 扫描器不漏报真标记、不误报字符串/字符。
- **双扫描器互证**：注释内扫描（cln04_scan，大小写不敏感）11 命中全误报；全文本扫描（cln04_cross，大小写敏感）项目自有代码 0 命中 ⇒ 真标记 0 可信。
- **已知局限**：占位符 `xxx-web` / `xxx?a=…` 会撞 `XXX`（\b 边界在 `xxx` 后为 `-`/`?`），正文"真 bug"撞 `BUG` ⇒ 均靠逐条人工核验排除；本批 11 处已全部人工过目。
- **边界说明**：`third_party/**` 内上游 `TODO/FIXME` 明确排除（§6 硬边界 + 非本库债务）；中文"未实现"类词为功能状态/契约文案，界定为非债务标记（若后续需"协议未实现"的台账，属 `PQ-xx` 待定流程而非 CLN-04）。

**文档结束。**
