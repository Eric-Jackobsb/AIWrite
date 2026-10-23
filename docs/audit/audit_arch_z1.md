# audit_arch_z1：ARCH-Z1 第 1 步落地审计报告（`provider_spec` 五 TU 拆分）

> 日期：2026-10-23 · 批次：**阶段 4 第二批（ARCH-Z1）· 第 1 组** · 承接 [audit_codebase_20261023.md](audit_codebase_20261023.md) §2「超长文件」行 / §5 第 4 序
> 结论一句话：`provider_spec.cpp`（**1743 行**）按职责拆成 **5 个 TU（最大 679 行）**，**公共 API 零变动**
>（`provider_spec.h` 一行未改），跨 TU 只留 **3 个内部入口**（新增 `ai/include/provider_spec_detail.h`）；
> 顺带消除 **2 处逐字相同的拼接辅助** + 前缀判定的分散实现 ⇒ 全库重复率 **3.8 % → 3.7 %**；
> 构建 **0 error / 0 warning**、`check_all` **20/20**、`--provider-selftest` **52/0**、exec **713/0**、graph **151/0**、
> `check_globals` **211 文件 / 违规 0 / fn_static 50·50**、`check_includes` **211/689/0**。

---

## 1. 背景与目标

[audit_codebase_20261023.md](audit_codebase_20261023.md) §2 把 `provider_spec.cpp`（1609 行口径 / 现测 1743 行）列为
**Z1 三大首选之一**，原因不是「行数多」本身，而是三件事压在同一文件里、互相看不见：

1. **层间规则**（`builtin → source → user.d/* → user` 的优先级、`schema_version` / `replace_all`）与
   **条目级解析**（类型容错、`type_ok`、未知字段、明文密钥拒绝）写在同一个 1743 行里，
   改优先级时容易误碰解析路径（反之亦然）——§2 记录的「改一处漏一处」风险就指这里。
2. **站点语义**（`webSpecFor` 的旧回落 vs `strictWebSpecFor` 的**不回落**，D-22② / I14）与
   **表加载**混排，`I2` / `I14` 两条不变量的守护点不显眼。
3. 同一职责有两份**逐字相同**的实现：`joinNames`（`provider_spec` 内）↔ `join_list`（同文件另一处），
   以及 `starts_with` 在 3 个用途段各自可达 —— §4「provider 骨架」簇的一部分。

本批目标（**只动体量与重复实现，不动对象边界、不动公共 API**）：

| 目标 | 判据 |
|---|---|
| 单文件 < 800 行 | [ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §3 ARCH-Z1 行 |
| 公共 API 零变动 | `provider_spec.h` 逐字节未改（`git diff --stat` 无该文件） |
| 重复率下降 | §4 目标 ≤ 3.0 %（本步只做「第一簇」的一部分） |

## 2. 拆分口径（职责 → TU）

| 新文件 | 内容（原行号） | 为什么切在这里 |
|---|---|---|
| `ai/src/provider_spec.cpp` | 层拼装 / `merge_provider_specs` / `loadProviderSpecs` / 快照与 `reload`（1096〜1309） | **唯一入口**：所有「跨层规则」只在这里；解析细节一律外置 |
| `ai/src/provider_spec_parse.cpp` | JSON 读取小工具 + 密钥双重检测 + 允许字段集（31〜244）· `parse_models/endpoints/web` / `applyEntry` / `validateFinal` / `fallbackSpecs`（651〜1087） | 解析层自成一段：**输入是 `json`、输出是 `ProviderSpec`**，只依赖 `utils::trim` |
| `ai/src/provider_spec_web.cpp` | `webSpecFor` / `strictWebSpecFor` · 站点不可用文案与字段缺失清单 · 登录态判定 · 探测适用性 · `webAdapterImplemented`（248〜505） | 站点语义（D-22② / D-26 / D-27 / D-28 / `I2` / `I14` / `I16`）集中一处，**全部纯函数、无 IO** |
| `ai/src/provider_spec_query.cpp` | `ProviderSpec::findModel` · `ProviderSpecs::find/ids/countKind` · `SpecLoadReport` 摘要 · 协议/adapter 清单与显示名 · `default*` · `resolve_*`（507〜643 + 1316〜1371） | 纯查询：节点 / 提示 / 自检共用，**不碰文件系统、不碰缓存** |
| `ai/src/provider_spec_selftest.cpp` | `CheckCounter` + 合成夹具 + `providerSpecSelftest`（1374〜1739） | 断言从实现里物理隔开：**审阅实现时不会被 380 行断言淹没**，反之亦然 |

`source/CMakeLists.txt` 显式源文件列表 +4 行（该库不用 `GLOB`，新增 TU 必须登记）。


## 3. 文件与体量

| 文件 | 行数 | 角色 |
|---|---|---|
| `ai/src/provider_spec.cpp` | 1743 → **256** | 加载 / 合并 / 缓存（层优先级 + `schema_version` + `replace_all` + 快照） |
| `ai/src/provider_spec_parse.cpp` | **679** | 解析 / 收尾校验 / 最小兜底表 |
| `ai/src/provider_spec_web.cpp` | **268** | 网页版站点派生 / 文案 / 登录态 / 探测适用性 |
| `ai/src/provider_spec_query.cpp` | **219** | 纯查询（模型 / 协议 / 默认值 / `resolve_*`） |
| `ai/src/provider_spec_selftest.cpp` | **384** | 离线断言（`--provider-selftest` 与 `--exec-selftest` 共用） |
| `ai/include/provider_spec_detail.h` | **63**（新增） | **模块内契约**：3 个入口 + 2 个内联小工具 + `kSchemaVersion` |
| `ai/include/provider_spec.h` | 288 → **288** | 公共 API：**未改一字**（本批零签名变动） |

拆分后总行数 1803（+60 = 5 份文件头 + `using` 声明 + 分区注释），**没有为降指标而合并语句或删注释**。

## 4. 内部契约（`provider_spec_detail.h`）

拆分最容易失控的地方是「匿名命名空间里的辅助函数要跨 TU 用」。本批的规则：

* **只有 3 个入口跨 TU 可见**：`applyEntry` / `validateFinal` / `fallbackSpecs`
  （原 `apply_entry` / `validate_final` / `fallback_specs`，按 §1 命名口径改驼峰——它们是**新增**的内部名字，公共 API 里的
  `snake_case` 一字未动）。
* 其余 `parse_*` / `is_implemented_*` / 允许字段集 / 密钥扫描**不进头文件**：它们只被 `provider_spec_parse.cpp` 使用，
  因此「内部契约全文」就是这 63 行，**没有第二处声明可与实现漂移**。
* 两个内联小工具收成单一来源（原先是重复实现）：
  * `startsWith` —— 原 `provider_spec.cpp` 匿名命名空间的 `starts_with`（3 个用途段共用：密钥值嗅探 / `builtin:` 前缀 / 层标签 `user`）；
  * `joinCn` —— 原 `joinNames`（web 段）与 `join_list`（解析段）**逐字相同**的两份实现合并（顿号拼接；
    注意 `site_template.h` 的 `joinNames` 是**逗号**语义，两者不可互换，故不跨文件复用）。
* `kSchemaVersion` 进头文件：加载层用它判层，避免「一处改版本、另一处仍写 1」。

## 5. 行为等价保持点（逐条对齐原实现）

1. **错误 / 警告的文本与顺序逐字保留**：`merge_provider_specs` 的报错串、`validate_final` 的 4 类警告、
   `webSiteError` 的三条可操作引导全部**原样搬运**（自检 `has_text(...)` 与 CLI 都在断言原文，见 §6）。
2. **层语义**：`replace_all` 仅对 `user*` 层生效、`schema_version` 必须是整数 1、空层跳过 —— 判断顺序未改。
3. **站点回落 / 不回落**：`webSpecFor`（回落表内第一个 web 条目，守 `I2`）与 `strictWebSpecFor`（不回落，守 `I14`）
   仍在**同一文件**，避免以后误把两套语义混用。
4. **兜底表**：`fallbackSpecs` 两条（`deepseek` / `custom-official`）字段逐字未改（含 `notes` 文案）。
5. **快照与 reload**：`specs_mutex()` / `specs_holder()` 两个函数内 static **原样保留**（`fn_static` 计数不变：50 / 50），
   `providerSpecsSnapshot()` 的日志文本与顺序未改。
6. **零新增可变状态**：本批只是搬函数，`check_globals` 从 206 文件 / 0 违规 → **211 文件 / 0 违规**（多出的 5 个是新文件）。

## 6. 验证结果（本批实测）

| 项 | 结果 |
|---|---|
| 构建（`source/build.ps1`） | **0 error / 0 warning**（4 目标：`aiwrite.exe` / `api_probe.exe` / `ce_repro.exe` / `webview2_login.exe`） |
| `check_all.ps1 -SkipBuild`（20 项） | **20/20 PASS · 失败 0 项** |
| `aiwrite.exe --provider-selftest` | **[配置表自检] 52 项通过 / 0 项失败**（合并优先级 / 未知字段 / 密钥拒绝 / 兜底 / web 两形态 / `resolve_*` / 视觉门控） |
| `aiwrite.exe --run-selftest` | **PASS**（CLI 自检断言零增减） |
| `api_probe --selftest` | **全组 PASS**（含「配置往返」「L2 工厂分派」） |
| `api_probe --exec-selftest` | **713 通过 / 0 失败**（含 `providerSpecSelftest` 并入计数） |
| `api_probe --graph-selftest` | **151 通过 / 0 失败** |
| `ce_repro.exe 0` | **ALL FRAMES OK (mode=0)**（与配置表无关，作为「未误伤」旁证） |
| `check_globals.ps1` | `checked files: 211 · violations: 0（R1 0 / R2 0 / R3 0）· fn_static: 50 / 50` · **PASS** |
| `check_includes.ps1` | `checked files: 211 · include lines: 689 · violations: 0` · **PASS**（新增头文件归属 ai 层，无反向依赖） |
| `dup_scan.py` | 克隆块 **91 → 90** · 重复行 **1093 → 1096** · 重复率 **3.8 % → 3.7 %**（ai 层 **5.8 % → 5.6 %**） |
| `check_links.py` | `checked links: 1951 · broken: 0`（本批文档改动**之后**复测；改动前基线 1933 —— +18 = 本文档 4 处被引 + 各索引表新增行） |

> 等价证据口径：配置表侧没有 GUI 自动化，故用 **`--provider-selftest` 52/0 + `--exec-selftest` 713/0**
>（同一套断言在拆分前后**项数不变**）+ 「错误/警告原文逐字保留」作为行为等价证据 —— 与 §4 判据 1 的既有做法一致。

**复现命令**：

```powershell
cd source; .\build.ps1
.\tools\check_all.ps1 -SkipBuild
..\build\bin\aiwrite.exe --provider-selftest
python ..\source\tools\dup_scan.py
python ..\source\tools\check_links.py
```

## 7. 判据对照（ARCH-Z1 目标）

| 目标 | 状态 |
|---|---|
| 单文件 < 800 行 | ✅ **14 个 ≥ 800 行的生产文件全部拆分完成**（**13 组**提交 · 2026-10-23 · 基线在 `6b2cb68` 实测 = 14 个）：生产模块现存最大 **766**（`engine/src/graph.cpp`，Z1 前已达标 · `image_decode` 758）· 唯一 ≥ 800 的剩余项 = 自检工具 `app/src/cli_selftest.cpp` **1429**（§8 风险 3 口径：工具类不计入 Z1） |
| 公共 API 零变动 | ✅ `provider_spec.h` / `inference_provider.h` / `upload_contract.h` 等公开头一字未改；新头全部 `*_detail.h` / `*_common.h`（模块内） |
| 重复率 ≤ 3.0 % | ✅ **2.98 %**（876 / 29425 · 去重专项 6 步，**3.8 % → 2.98 %**，见 §10） |
| 依赖单向 / 零全局 / 断链 | ✅ 见 §6 三行门禁 |

## 8. 队列收口（Z1 · **13 组 / 14 个文件** · 全部完成）

**基线**（`6b2cb68` 实测 · 生产模块 ≥ 800 行 = **14 个**；`cli_selftest.cpp` 1429 属工具类不计）：下表 = 收口后实测（`max` = 拆后最大 TU · 新 TU 按 `前缀_*.cpp` 同目录统计）。

| # | 文件（基线行数） | 组 | 拆后 | max |
|---|---|---|---|---|
| 1 | `ai/src/provider_spec.cpp` 1743 | 1/13 | 5 TU + `provider_spec_detail.h` | **679** |
| 2 | `web/src/webview_host.cpp` 3514 | 2/13 | 8 TU + `webview_host_detail.h` | **607** |
| 3 | `ai/src/dom_web_client.cpp` 2397 | 3‑4/13 | 4 TU + 2 脚本头 + `dom_web_client_detail.h` | **583** |
| 4 | `web/src/pydoll_channel.cpp` 1282 | 3‑4/13 | 3 TU + `pydoll_channel_detail.h` | **527** |
| 5 | `ui/src/app.cpp` 1454 | 5/13 | 4 TU + `app_detail.h` | **486** |
| 6 | `ui/src/node_canvas.cpp` 1125 | 6/13 | 2 TU + `node_canvas_detail.h` | **734** |
| 7 | `ui/src/settings_panel.cpp` 944 | 7/13 | 3 TU + `settings_panel_detail.h` | **487** |
| 8 | `ui/src/canvas_view.cpp` 938 | 8/13 | 2 TU | **551** |
| 9 | `ui/src/property_panel.cpp` 855 | 9/13 | 2 TU + `property_panel_detail.h` | **639** |
| 10 | `ui/src/ce_draw.cpp` 845 | 10/13 | 2 TU | **686** |
| 11 | `engine/src/executor.cpp` 886 | 11‑13/13 | 2 TU | **631** |
| 12 | `engine/src/node_registry.cpp` 899 | 11‑13/13 | 2 TU | **698** |
| 13 | `engine/src/composite.cpp` 1171 | 11‑13/13 | 3 TU | **431** |
| 14 | `nodes/src/local_nodes.cpp` 1242 | 11‑13/13 | 4 TU + `local_nodes_detail.h` | **389** |

> 组数 13 ≠ 文件数 14：第 3‑4 组与第 11‑13 组各一次提交覆盖多个文件（见 `git log --oneline | grep 'ARCH-Z1'`）。

**收口后全库**（生产模块 · 排除 `source/tools` 与 `third_party`）：**≥ 800 行 = 0**；现存前三 = `engine/src/graph.cpp` **766** · `utils/src/image_decode.cpp` **758** · `ui/src/editor_controller.cpp` **745**。工具类 `app/src/cli_selftest.cpp` **1429** 按下方风险 3 口径不计入 Z1。

**已收口的去重专项**：见 §10 —— 重复率 **3.8 % → 2.98 %**（判据 ≤ 3.0 % 达成）。

**风险**（保留原文 · 已按批执行）：

1. `webview_host` / `dom_web_client` 含 WebView2 与线程/管道交互，**没有单元级等价证据**：拆分必须只搬函数、不改调用顺序，
   并以 `--upload-selftest`（72/0）+ 真机一次网页版运行作旁证 → 建议单独一组、单独提交。
2. 「provider 骨架模板方法化」（`anthropic↔gemini` 29 行 / `doubao↔kimi` 26 行 / `official_template↔site_template` 20 行）
   属**逻辑改造**，不是搬文件；与「单文件 < 800」是两条判据，**不要合并在同一提交**里。
3. 工具类（`api_execution_selftest.cpp` 3888 行等）**不计入 Z1**（审计口径只算生产模块）；若要收，另立批。

## 9. 回滚方式

* 整批回滚：`git revert <Z1-a 提交>`（恢复单文件 + `CMakeLists.txt` 4 行 + 删 `provider_spec_detail.h`）。
* 只回滚站点段：把 `provider_spec_web.cpp` 内容并回 `provider_spec.cpp` 并删除该 TU 的 CMake 行（其余 4 TU 不受影响）。
* 公共 API 未变 ⇒ **调用方无需任何回滚**（`provider_spec.h` 一字未改）。

## 10. 去重专项（v1.14 · 2026-10-23）

Z1 拆分只搬文件、不改语义 ⇒ 拆分后**暴露**出一批逐字重复块（同簇函数被放进同一模块后更容易看见）。
本批 6 步按「先机械、后结构」顺序收敛，每步单独构建（0 error / 0 warning）并复测 `dup_scan`：

| 步 | 手段 | 去重对象 | 落点 | 重复行 | 累计 |
|---|---|---|---|---|---|
| 1+2 | 公共计数器 + lambda 工厂 | `CheckCounter` 3 份逐字复制 + 7 处内联 `auto check = [&](bool, const char*)` | **新** `utils/include/check_counter.h`（header-only · `make_selftest_check`） | **−90** | 1112 → **1022** |
| 3 | 同文件 helper | `workflow_io.cpp` 两段「恢复 id / 标题 / 参数」（18 行 ×2） | 同文件匿名命名空间 `apply_recorded_overrides()` | **−36** | → **986** |
| 4 | 模块内 helper | `local_nodes_llm / vlm` 的 18 行「重试计划」 | `nodes_detail::resolve_retry_plan()`（`local_nodes_detail.h` + `_helpers.cpp`） | **−28** | → **958** |
| 5 | 共用 TU | `anthropic / gemini` 的「图片 → data URL」12 行 + 「HTTP 失败归因」13 行 | **新** `ai/include/provider_common.h` + `ai/src/provider_common.cpp` | **−30** | → **928** |
| 6 | 共用 TU | `doubao / kimi` 上传单元的 `prepare`（17 行）+ `deliver`（20 行）骨架 | **新** `ai/include/site_upload_common.h` + `ai/src/site_upload_common.cpp` | **−52** | → **876** |

> 每步「累计」= 在该提交（`1a871fe` / `43f83c7` 及中间工作树）上跑 `dup_scan.py` 的实测值（`git worktree` 校验，非估算）。

**合计**：重复行 **1112 → 876** · 重复率 **3.8 % → 2.98 %**（`meaningful lines 29425`）· 克隆块 **60**（`clone runs`）。

**口径（为什么这样切）**：

1. **只抽「同口径」片段**：站点 / 厂商差异全部以参数或 `why_hint` 保留（例 Kimi 的「待侦察占位」提示文案、Anthropic/Gemini 的错误前缀），
   因此**行为逐字不变** —— 不引入「新分支」是这批能一次通过 `check_all` 的前提。
2. **公开头零改动**：4 个新头（`check_counter.h` / `provider_common.h` / `site_upload_common.h`）都写在模块内并明确标注
   「**不是**公开 API · 只允许 X include」，`check_includes.ps1` 仍 0 violations。
3. **新 TU 一律手工登记**：`CMakeLists.txt` 无 GLOB ⇒ `provider_common.cpp` / `site_upload_common.cpp` 各加一行注释（见 §3）。
4. **不做「为凑数」的合并**：`pydoll_channel_selftest` 内部两段 23 行、`image_decode` 双分支 15 行、
   `dom_web_client_probe ↔ selftest` 15 行等**仍留着** —— 它们是「不同输入口径的断言」，合并会牺牲失败定位精度（§8 风险 2 同口径）。

**验证**（本批实测 · 全绿）：

| 项 | 结果 |
|---|---|
| 构建 | **0 error / 0 warning** |
| `check_all.ps1` | **21/21 PASS**（`--upload-selftest` 72/0 · `--exec-selftest` 713/0 · `--graph-selftest` 151/0 · 夹具 44/17/15 · `ce_repro` ALL FRAMES OK） |
| `check_includes.ps1` | 254 files · 913 include lines · **0 violations** |
| `check_globals.ps1` | 254 files · **0 violations**（R1 0 / R2 0 / R3 0 · fn_static 50/50） |
| `check_links.py` | 1974 links · **0 broken** |
| `dup_scan.py` | 重复行 **876** · 重复率 **2.98 %**（≤ 3.0 % ✅） |

**回滚**：`git revert 1a871fe 43f83c7`（两步去重提交；拆分提交不受影响 —— 去重只删重复、不改调用契约）。

---

## 11. 8/8 复审（2026-10-23 · 冻结解除闸口）

> 依据：[ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §4 八条验收判据。逐条**当场复跑**取证，不引用历史结论。

| # | 判据 | 结论 | 复跑证据（本次实测） |
|---|---|---|---|
| 1 | 行为等价 | ✅ | `check_all.ps1` **21/21 PASS**（内含 `--selftest` 全组 = 含 `composite_selftest` **112/0** · `--upload-selftest` **72/0** · `--run-selftest` PASS）· exec **713/0** · graph **151/0** · 夹具 **44/17/15** · `ce_repro` **ALL FRAMES OK (mode 0)** · `check_links` **1974/0**（基线 1730 · 只升不降）；两个已知 bug 由**自动断言**覆盖 —— Ctrl+S = 命令路由 `dispatchCommand(Save)` 断言（R4）· 暴露节点保存 = `buildCompositeSpecFromSelection` + `validate_composite_spec` 断言 21 条（E6）。**GUI 单击复核**仍建议用户顺手点一次（非阻塞 · 无自动化通道） |
| 2 | 全局变量归零 | ✅ | `check_globals.ps1` = **254 文件 / 违规 0**（R1 0 / R2 0 / R3 0）· 函数内 static 棘轮 **50 · 50**（只许减） |
| 3 | 双画布单套交互 | ✅ | 关键谓词扫描：`interactor.cpp` 之外命中 **2 条 = 纯注释**（`ce_draw.cpp:313` / `node_canvas_impl.cpp:371`）· `BeginDelete` 实体只在 `interactor.cpp`（3 处） |
| 4 | 打包-校验同源 | ✅ | `ce_pack.cpp` 中 `spec.internal_graph` / `spec.inputs` / `spec.exposed_params` 直写 = **0** |
| 5 | 依赖单向 | ✅ | `check_includes.ps1` = **254 文件 / 913 include 行 / 0 violations** |
| 6 | `editor()` 退场 | ✅ | 全库 `\beditor\(\)` = **0** |
| 7 | ReadOnlyView 落地 | ✅ | 渲染签名仍全部 `const ReadOnlyView&` / `const CeReadOnlyView&`（13 组拆分**未改任何渲染签名** · Z2 已满足）· `ce_repro` 全帧 OK |
| 8 | `AppContext` 运行时不可访问 | ✅ | `ui` 下 `AppContext` 命中 **5 = 定义 + 启动装配**（`app_context.h:29-30` · `app_context.cpp:10` · `app.cpp:7/79`），面板 / 视图代码 **0** 处 |

**复审结论：8/8 ✅** ⇒ 触发 [FREEZE.md](../refactor/FREEZE.md) §3 解除动作（状态 🟢 已冻结 → 🔵 已解除 · DocMAP 同步 · 冻结组核对：`DevPlan.todo` 的 `📦 冻结待解冻` 组**从未建立**（冻结期内零新功能登记）⇒ 无需迁移，仅在此登记说明）。

**复审顺带修掉的文档偏差**（取证时发现 · 只改文档、零代码）：

1. §7 原写「13 个 > 800 的生产文件」→ 实为 **14 个**（基线 `6b2cb68` 逐文件实测；13 = **提交组数**，第 3‑4 与第 11‑13 组各一次提交覆盖多个文件）。
2. §8 原留「剩余 10 个生产文件」待拆表（与 §7「全部完成」自相矛盾）→ 改为收口后实测表（14 行 · 含拆后 `max` TU 行数）。
3. 行数口径统一为 `wc -l`：`image_decode` 757 → **758** · `ce_draw` 846 → **845** · `node_canvas` 1126 → **1125**（原值多算了末行）。

**复现命令**：

```powershell
cd source; .\build.ps1
.\tools\check_all.ps1 -SkipBuild
.\tools\check_globals.ps1 ; .\tools\check_includes.ps1
cd .. ; python source\tools\check_links.py ; python source\tools\dup_scan.py
```

**回滚**：本条只改文档 ⇒ `git revert <docs 提交>`；判据证据全部来自可复跑命令，无需回滚代码。


