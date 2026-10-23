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
| 单文件 < 800 行 | **provider_spec 家族全部达标**（最大 679）· 全库仍有 **13 个**生产文件 > 800（§8） |
| 公共 API 零变动 | ✅ `provider_spec.h` 未改；5 个 TU 共享同一份声明 |
| 重复率 ≤ 3.0 % | 🟡 **3.7 %**（本步 −0.1 个百分点：−1 克隆块）—— 余下靠「provider 骨架模板方法化」（§8 第 2 组） |
| 依赖单向 / 零全局 / 断链 | ✅ 见 §6 三行门禁 |

## 8. 遗留与风险（Z1 队列）

已完成：`provider_spec.cpp` ✅（1743 → 5 TU）· `image_decode.cpp` **758** ✅（已 < 800，无需拆）。
**剩余 13 个生产文件**（按「先大后小、先纯逻辑后 GUI」排序，下一组从 `webview_host` 起）：

| 顺序 | 文件 | 行数 | 拆分预判 |
|---|---|---|---|
| 1 | `web/src/webview_host.cpp` | **3514** | WebView2 宿主 / 登录 / PoW / 上传桥接四段 → 至少 4 TU（本批最大风险面） |
| 2 | `ai/src/dom_web_client.cpp` | **2397** | DOM 适配器（探测 / 发送 / 取答案 / 上传）→ 3 TU |
| 3 | `ui/src/app.cpp` | **1454** | 主循环 → `AppFrame`（§3 原定口径） |
| 4 | `web/src/pydoll_channel.cpp` | **1282** | 守护进程逻辑两处 23 行同构（§4 集群）→ 合并后再拆 |
| 5 | `nodes/src/local_nodes.cpp` | **1242** | 两个节点函数结构同构（§4 集群）→ 抽公共骨架 |
| 6 | `engine/src/composite.cpp` | **1171** | 复合执行 ← 与 `executor` 同簇 |
| 7 | `ui/src/node_canvas.cpp` | **1126** | 交互段移入 interactor（A6 已把 11 条 static 收进 `NodeCanvasState`） |
| 8 | `ui/src/settings_panel.cpp` | **944** | 分区渲染 → 每区一函数（`SettingsPanelState` 已在 A6 就位） |
| 9 | `ui/src/canvas_view.cpp` | **938** | 视图装配 |
| 10 | `engine/src/node_registry.cpp` | **899** | 注册表 + 内置节点声明 |
| 11 | `engine/src/executor.cpp` | **886** | 校验块两处同构（§4 集群 14 行） |
| 12 | `ui/src/property_panel.cpp` | **855** | 每类节点一函数 |
| 13 | `ui/src/ce_draw.cpp` | **846** | Z2 后仍 846：窗口函数按「规格 / 模型 / 试运行」三段切 |

**风险**：

1. `webview_host` / `dom_web_client` 含 WebView2 与线程/管道交互，**没有单元级等价证据**：拆分必须只搬函数、不改调用顺序，
   并以 `--upload-selftest`（72/0）+ 真机一次网页版运行作旁证 → 建议单独一组、单独提交。
2. 「provider 骨架模板方法化」（`anthropic↔gemini` 29 行 / `doubao↔kimi` 26 行 / `official_template↔site_template` 20 行）
   属**逻辑改造**，不是搬文件；与「单文件 < 800」是两条判据，**不要合并在同一提交**里。
3. 工具类（`api_execution_selftest.cpp` 3888 行等）**不计入 Z1**（审计口径只算生产模块）；若要收，另立批。

## 9. 回滚方式

* 整批回滚：`git revert <Z1-a 提交>`（恢复单文件 + `CMakeLists.txt` 4 行 + 删 `provider_spec_detail.h`）。
* 只回滚站点段：把 `provider_spec_web.cpp` 内容并回 `provider_spec.cpp` 并删除该 TU 的 CMake 行（其余 4 TU 不受影响）。
* 公共 API 未变 ⇒ **调用方无需任何回滚**（`provider_spec.h` 一字未改）。

