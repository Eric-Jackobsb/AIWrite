# 仓库整理与瘦身 · 执行计划（S0~S5）

> 状态：**范围已拍板（2026-10-24）** —— 执行 `S0~S5`；**Pydoll/Python 退场（S6）明确不做**，单独立项再议。
> 扫描物证日期：2026-10-24；测试数字引用 2026-10-23 的文档实测，执行前由 S0 复跑校准。
> 纪律：按 `AI_README.md` §3 取证（输出重定向留 txt）、§4 伴随义务、§6 不擅自 commit / tag / push、§7 红线；文档归档走 `git mv` + `Archive/README.md` 索引同步。

---

## 一、背景（扫描结论摘要）

- 文档 80 个、约 3.4 MB；`CHANGELOG` 630 KB / 2641 行；`DevPlan.todo` 298 KB / 5492 行；`audit/` 34 份（22 份是同一轮重构的分批记录）；`Archive/actionPlan` 1.29 MB。
- 自有 C++ 254 文件，生产代码无 >800 行文件（最大 `graph.cpp` 766）；工具侧 `source/tools/api_execution_selftest.cpp` **3888 行 / 241 KB**为全仓最大单文件。
- `third_party/imgui-node-editor` 约 70 个入库文件、约 3.5 MB **不参与编译**（`CMakeLists.txt:363-367` 还明确禁止其 `external` 进 include）。
- 仓库根有 `windowstack_crash.txt`（120 行一次性取证）；`source/python/_probe/out/**` 56 个已退场 M7B 探针输出。
- 当前基线数字多副本漂移：`check_links` 2094 / 2096 / 1988 / 1974；`AI_README.md` §2 的 `check_all` 仍写 17/17（现 21/21）。
- 线索登记欠账：`DBG-CN-02`（打包必失败）与 `POL-E-05a/b/c`（字数范围）只在 `DevPlan.todo`，未同步 `M_polish.md` / `CHANGELOG`。

---

## 二、批次与任务（按序执行，每批单独验证、单独汇报）

### S0 · 基线取证 + 引用排查（只读 + 构建）

1. 跑全量 `source/tools/check_all.ps1`，日志落 `build/logs/`；记录：`check_all` 项数、`check_links` checked/broken、exec / graph / upload 数字、`dup_scan` 重复率。
2. 引用排查（命中即停止该删除项，改为登记）：
   - `rg -n "windowstack_crash" .`（排除 `build/`）
   - `rg -n "_probe/out|_probe\\\\out" source docs`
   - `rg -n "imgui-node-editor/(examples|external|docs|misc)|\bDXSDK\b|\bgl3w\b" source docs`
3. 产出：本批的 `build/logs/slim_s0_baseline_<ts>.log`（作为后续每批对照锚点）。
4. 验收：日志含退出码；所有待删项「0 引用」结论有 rg 输出物证。

### S1 · 仓库根与探针证据（无代码行为变更）

1. `windowstack_crash.txt`：S0 确认无引用后，`git rm`（一次性调试 dump，内容已在历史提交里）；若发现引用 ⇒ `git mv` 到 `docs/Archive/evidence/`。
2. `source/python/_probe/out/**`：整体 `git mv` → `docs/Archive/evidence/m7b_probe_out/`（保留历史证据，不进源码树）；其中 **0 字节 `*.err`** 直接 `git rm`。
3. `docs/Archive/README.md` 增索引行（证据目录）；`CHANGELOG` `[Unreleased]` 追加本批条目。
4. 验收：`git status` 只有预期变更；`check_links` broken **0**；不触发构建（无代码改动）。

### S2 · third_party 减负（无编译面变更）

1. `git rm -r source/third_party/imgui-node-editor/{examples,external,docs,.github,misc}`。
2. 保留：`imgui_node_editor.cpp`、`imgui_node_editor_api.cpp`、`imgui_canvas.cpp`、`crude_json.cpp`、全部项目头、`LICENSE`、`AIWRITE_PATCHES.md`、`.gitignore`。
3. `third_party/imgui-node-editor/AIWRITE_PATCHES.md` 追加一节：「上游 examples/external/docs 已移除 —— 不参与编译；`external/` 为 ODR 红线区，永不入 include」。
4. 验收：构建 **0 error / 0 warning** · `check_all` **21/21** · `check_includes` / `check_globals` 违规 **0** · `--exec-selftest` 数字不变。
5. 回滚：`git restore --staged --worktree source/third_party`（本批不涉及其他文件）。

### S3 · 文档基线与登记面收口（纯文档）

1. `AI_README.md` §2：`check_all` 17/17 → **21/21**（补实测日期）；§2 `check_links` 只保留判据，不写历史值。
2. `docs/AI_Status.md`：把第 6 行「最近更新」里的多轮历史移入 `CHANGELOG` 对应批次；本页「最近更新」压到 ≤3 行，状态页回到「一页看清」。
3. 当前基线去重：`docs/refactor/FREEZE.md`、`docs/refactor/ARCHITECTURE.md`、`docs/audit/audit_codebase_20261023.md` 中**声称当前**的 `check_links` / `check_all` 值改为「见 AI_README §2」；历史批次值保留并显式标注「当时实测」。
4. `docs/DocMAP.md`：长行表格重排为「路径 | 用途 | 何时读」短表，不删条目、不改链接目标。
5. 登记欠账补齐（**只登记，不写验证数字**）：`docs/actionPlan/M_polish.md` §2.4 追加 `POL-E-05a/b/c` 行（含 `05c` 待拍板标记）；`CHANGELOG` `[Unreleased]` 追加 `DBG-CN-02` 条目（状态：已定位、未修，指向 DevPlan `id=598`）。
6. 验收：`check_links` broken **0**；`rg` 抽查当前基线不再出现 ≥2 个不同值。

### S4 · 审计与冻结文档归档（纯文档 · `git mv`）

1. 建 `docs/Archive/audit/`；`git mv` 迁入：
   - 22 份 `docs/audit/audit_arch_{r1..r4,e1..e6,a1..a7,z1,z1_web,z2}.md`
   - 11 份 `docs/audit/audit_cleanup_*.md`
   - `docs/audit/audit_architecture_20261021.md`（已被 `audit_codebase_20261023.md` 取代）
2. 新建 `docs/audit/refactor_summary.md`：19 批 × 1 行（批次 / 判据 / 结论 / 原始审计链接），指向 `ARCHITECTURE.md` §4 与归档件。
3. 新建 `docs/audit/cleanup_summary.md`：`CLN-01…15` 结论一览 + 原始审计链接。
4. `docs/refactor/FREEZE.md`：解除记录要点并入 `ARCHITECTURE.md` 附录（状态 🔵 已解除 + 8/8 判据 + 日期），随后 `git mv` → `docs/Archive/refactor/FREEZE.md`，顶部加「📦 已归档 · 现行 = ARCHITECTURE §4/§5」。
5. 索引 7 处同步：`Archive/README.md`（新增两行目录索引）· `DocMAP.md` · `CHANGELOG.md` · `DevPlan.todo`（新归档批节点）· `AI_README.md` §1/§8 · 引用过这些审计的现行计划 · 变量术语对照表（如涉及编号）。
6. 保留在 `docs/audit/` 的活文件：`audit_codebase_20261023.md` · `DEBT.todo` · `L1_checklist.todo` · 两份新 summary。
7. 验收：`check_links` broken **0**；每个被移动文件在 `Archive/README.md` 有索引；无 `docs/audit/audit_arch_*` 残留活链。

### S5 · `api_execution_selftest.cpp` 拆分（代码 · 行为等价）

前置事实：该文件是**单个 `execution_selftest()` 函数（107 行起，3888 行止）**，内部约 80 个 `// ----` 分节 ⇒ **不能按文件切分**，必须先把分节提为 helper 函数，再按域分 TU。

1. 在 `source/tools/api_internal.h` 增加内部契约：`void exec_selftest_nodes(Check&)`、`exec_selftest_media`、`exec_selftest_provider`、`exec_selftest_web`、`exec_selftest_text`、`exec_selftest_debt`（命名以现有 `Check` 类型为准；不改任何公共 API）。
2. 按域提取（顺序与现有打印顺序完全一致）：
   - **nodes**：L1-a~L1-f 参数面（122–438）
   - **core**：1~8 本地链路 / 失败传播 / 归档（439–1117）
   - **media**：M5-02 多模态与图片（1118–1947）
   - **provider**：M_patchB 参数化与配置表（1948–2243）
   - **web**：DOM 适配器 + 答案门 + 登录态 + M7B 管道 + M_patchC 归属/就绪/窗口（2244–3396）
   - **text**：L2 六文本节点（3397–3782）
   - **debt**：DEBT-L7 / L8（3783–3888）
3. 目标文件：`api_execution_selftest.cpp`（仅保留 `execution_selftest()` 编排：按原顺序调用各 helper）、`api_execution_selftest_nodes.cpp`、`_core.cpp`、`_media.cpp`、`_web.cpp`（provider + web 合并）、`_text.cpp`（text + debt 合并）—— 以最终每文件 < 900 行为准。
4. `source/CMakeLists.txt` 的 `api_probe` 源列表登记新 TU。
5. 验收（逐字等价，不是「差不多」）：
   - 拆分前先存 `api_probe.exe --exec-selftest > build/logs/exec_before.txt 2>&1`
   - 拆分后同样存 `exec_after.txt`，`Compare-Object`/`diff` 结果 **0 差异**（含断言数与打印顺序）
   - 构建 **0/0** · `check_all` **21/21** · exec **713/0** · graph **151/0** · `check_includes`/`check_globals` 违规 0
6. 风险与处理：若某分节内局部变量跨节复用 ⇒ 提升到该 helper 内局部，**不得改断言语义**；若拆不出干净边界 ⇒ 该域整块留在主 TU，先交付其余域（如实登记）。

---

## 三、全局验收（每批结束都要过）

- 构建 **0 error / 0 warning**
- `check_all.ps1` **21/21 PASS**
- `--selftest` 十三组 · `--exec-selftest` **713/0**（S5 要求逐字不变）· `--graph-selftest` 151/0 · `--upload-selftest` 72/0
- `check_links` 全库 broken **0**；`check_includes` / `check_globals` 违规 **0**；`dup_scan` ≤ **2.98 %**
- 文档改动同批更新 `CHANGELOG [Unreleased]`；归档同批更新 `Archive/README.md`
- 不执行 `commit` / `tag` / `push` / 版本号变更

---

## 四、明确不做（本计划出范围）

- **S6 Pydoll/Python 通道退场**：`pydoll_channel*` / `pipe_client` / `channel_frames` / `session_snapshot` / `source/python/brain_ai_browser` / `aiwrite_copy_python()` / `--pydoll-*` 全部保留。
- 不拆 `CHANGELOG.md`（大量链接指向它，拆分必制造断链）。
- 不裁剪 `DevPlan.todo` 结构（唯一勾账位）；不删历史 `Archive/**` 正文。
- 不删 `docs/draft/**` 三份草案；只标注「草案 · 未实施」。
- 不动 `source/third_party` 四个参与编译的 `.cpp` 与头文件、`source/assets/**`、`source/workflows/**`。
- 不修 `DBG-CN-02`（bug 修复走独立批；本计划只在 S3 补登记）。

---

## 五、汇报口径（每批结束）

- 改了什么（文件数 / 体积变化）
- 验证物证（命令 + 日志路径 + 数字 + 退出码）
- 遗留 / 跳过项及原因
- 不做「应该没问题」式结论
