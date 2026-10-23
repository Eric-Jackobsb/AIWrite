# audit_arch_z2：ARCH-Z2 落地审计报告（`CeSession` 对称拆分 + ce 渲染只读视图化）

> 日期：2026-10-23 · 批次：**阶段 4 第一批（ARCH-Z2）** · 承接 [audit_codebase_20261023.md](audit_codebase_20261023.md) §2 / §3.2（判据 7 只覆盖一半）与 §5 第 3 序
> 结论一句话：上级节点编辑器从「一个 `CompositeEditorState` 单例 + 渲染直读可变对象」改为**五个对象对称拆分 + `CeReadOnlyView` 只读渲染**；
> **判据 7 由 🟡 转 ✅**，`ce_name_buffers()` 函数内 static 收进 `CeSpec` ⇒ 棘轮 **51 → 50**；
> 构建 0 error / **0 warning**、`--run-selftest` PASS、exec **713/0**、graph **151/0**、`ce_repro` 全帧 OK、`check_globals` **206 文件 / 违规 0 / fn_static 50·50**。

---

## 1. 背景与目标

两条判据同时压在同一改动面上（[audit_codebase_20261023.md](audit_codebase_20261023.md) §3.2）：

| 项 | 现状（本批前） | 目标 |
|---|---|---|
| 判据 7（ReadOnlyView 落地） | 主画布 4 个绘制函数已收 `const ReadOnlyView&`；**ce 侧 `drawCompositeEditor` 仍直读 `CompositeEditorState` 单例** | ce 渲染只经 `CeReadOnlyView` 读取，写路径经会话 API |
| 结构（双编辑器对称） | `CompositeEditorState` 一个结构兼：工作副本 + 撤销栈 + 选中 + 规格标记 + 文件 + 试运行 + UI 开关 | 按主编辑器一侧的既有对象（`WorkflowModel` / `WorkflowFile` / `SelectionState`）对称拆成 5 个对象 |
| 规则 6 棘轮 | `ce_draw.cpp` 的 `ce_name_buffers()` = 函数内 static 缓冲（跨帧状态藏在函数里） | 收进 `CeSpec` 成员 ⇒ 棘轮 51 → **50**（只许减） |

> 为什么两件事同一批做：判据 7 要求「渲染看不见可变对象」，而 ce 侧要读的数据正好都在待拆的那一个单例里 ——
> 先拆对象再补视图会改两遍调用点；同一批做只需在每帧构建一次视图，绘制层的改法一次到位。

## 2. 拆分口径（字段 → 对象）

| 原 `CompositeEditorState` 字段 | 去向 | 对称对象 | 说明 |
|---|---|---|---|
| `working`（`CompositeSpec` 工作副本）· `undo` / `redo` 栈 | **`CeModel`** | `WorkflowModel` | 撤销粒度 = **整份 `CompositeSpec` 快照**、深度 **30**（与原实现一致）；主画布 `engine::UndoStack` 固定绑 `Graph` 且深 50，故 ce 自带同角色小栈 |
| `selected_node` | **`CeSelection`** | `SelectionState` | ce 画布是「局部单选」语义（多选/无选 → `single()` 为空），`sync()` 内置变化检测 |
| 对外输入 / 输出（唯一）/ 暴露参数三处标记 · `ce_name_buffers()` 显示名缓冲 | **`CeSpec`** | 无（ce 独有域） | 标记查询/增删 + 显示名写回 + `drop_node_refs()`（删节点后三处引用清理）；只读 `model_`，不压快照、不置脏 |
| 打开 / 保存 / 热重载（原 `open` / `save` 函数体） | **`CeFile`** | `WorkflowFile` | 打开：注册表 → 工作副本（内置预设只读拦截 / 规格缺失 → 空白草稿，行为不变）；保存：名称描述同步 → 校验 → 注册表 + 落盘 → **主画布同 id 实例热重载** |
| `active` / `ask_close` / `error` / `name_buf` / `desc_buf` / 试运行执行器与读模型 | **`CeSession`** | `EditorController`（编排）+ `RunSession`（会话部分） | 会话门面 = 唯一实例 `ceSession()`；对外 API（`compositeEditorOpen` / `RequestClose` / `ForceClose` / `Save`）签名不变，全部转发到对象 |

对象图（无环，全部单向依赖 `CeModel`）：

```
CeSession ── 持有 ──> CeModel（唯一真相）· CeFile(→CeModel) · CeSelection · CeSpec(→CeModel)
    └─ 访问器：model() / file() / selection() / spec_marks()
```

> `compositeEditorState()` 全局单例**已删除**；`ceSession()` 以函数内 static 承载对象（C++11 magic static，单一定义点，
> 与 `ceUiState()` / `appConfig()` 同一形态），符合规则 6「状态是对象成员」的口径。


## 3. 文件与体量

| 文件 | 行数 | 角色 |
|---|---|---|
| `ui/include/ce_session.h` | **244** | 五对象声明 + `CeReadOnlyView`（对称 `readonly_view.h`） |
| `ui/src/ce_model.cpp` | **74** | 压栈 / 撤销 / 重做 / `clearHistory` / `load`（`kCeUndoDepth = 30`） |
| `ui/src/ce_selection.cpp` | **33** | `is_selected` / `sync`（变化检测）/ `clear` |
| `ui/src/ce_spec.cpp` | **191** | 规格标记查询与增删 + 显示名缓冲与写回 + 引用清理 |
| `ui/src/ce_file.cpp` | **98** | 打开 / 保存 / 主画布热重载回写 |
| `ui/src/ce_session.cpp` | **182** | `ceSession()` · open / requestClose / forceClose / save / syncNameBuffers · **对外 API 转发** · 内部图编辑 API（snapshot / addNode / removeNode / deleteNode / addEdge） |
| `ui/src/ce_state.cpp` | **57** | `ceUiState()`（画布域）· 撤销/重做薄转发到 `CeModel` |
| `ui/src/ce_draw.cpp` | 865 → **846** | 渲染读 `view`、写 `session`；`ce_name_buffers()` / `ce_name_buffer()` 删除 |
| `ui/src/ce_run.cpp` | 203 → **223** | 试运行域：`state.*` → `session.*`（`run_active` / `run_snapshot` / `run_status` 归会话） |
| `ui/src/composite_editor.cpp` | 139 → **162** | 改名域：`ceApplyRename` 等改走 `ceSession()` |
| `ui/include/ce_internal.h` | 66 | 删 `CeUndoEntry` / undo / redo 字段；`ceRemoveNode` 改单参 |

`CMakeLists.txt` 显式源文件列表新增 4 个 TU（`ce_model` / `ce_selection` / `ce_spec` / `ce_file`），并同步两行注释。

## 4. 渲染只读视图（判据 7 的落地形态）

```cpp
// app.cpp 主循环：每帧构建一次（只存引用、零拷贝、不跨帧存活）
const CeReadOnlyView ce_view(ceSession().model(), ceSession().selection(),
                             ceSession().run_snapshot, config);
drawCompositeEditor(kWindowCompositeEditor, &show_composite_editor, ce_view, state);
```

| 视图字段 | 内容 | 绘制层用法 |
|---|---|---|
| `nodes_for_render()` / `edges_for_render()` | `const std::vector<...>&`（内部图数组引用） | 节点/连线遍历（零拷贝） |
| `find_node` / `find_edge` / `graph()` | 只读查询（对齐吸附 / 小地图 / `findEdgeIntoInput`） | 交互与绘制的只读判定 |
| `id()` / `name()` / `dirty()` | 顶部状态栏与关闭确认 | 只读展示 |
| `selection`（`const CeSelection&`） | 参数编辑区选中的内部节点 | `view.selection.single()` |
| `snapshot`（`const engine::RunSnapshot&`） | 试运行读模型（空对象 = 未运行） | 运行态徽标 / 端口值 |
| `config`（`const Config&`） | 主题 / 网格 / 吸附等 | 主题与网格开关 |

> **写路径不进视图**：顶栏按钮、规格标记、参数编辑、连线/删节点、试运行 start/stop 仍走 `ceSession()`（或转发函数），

## 5. 行为等价保持点（逐条对齐原实现）

| 语义 | 保持方式 |
|---|---|
| 撤销粒度 / 深度 | 整份 `CompositeSpec` 快照、`kCeUndoDepth = 30`、压栈时丢重做分支 + 置脏（与原 `ce_state.cpp` 完全一致） |
| `ceRemoveNode` 不压快照 | 仍不压：右键菜单 / Delete 键**按批次**压一次（`interactor.on_delete_batch`），一次删除 = 一次撤销 |
| 关闭会话清场 | `forceClose` 先 `compositeEditorStopRun()`（不留后台线程）→ 清选场 / 历史 / 名称缓冲 / 运行读模型 |
| 未保存提示 | `requestClose`：`dirty()` ⇒ `ask_close`，否则直接关闭；弹窗「保存并关闭 / 放弃更改 / 取消」三态不变 |
| 显示名缓冲重置时机 | 打开会话（名称/描述 `copy_to`）与关闭会话（`spec_.reset_name_buffers()`），与原清缓冲行为对齐 |
| 规格标记的三处联动 | 输入（可多）/ 输出（**唯一**，置新清旧）/ 暴露参数（`param_id` 全局唯一，冲突禁用并提示）；删节点后 `drop_node_refs` |
| 保存后主画布同步 | `CeFile::save` 仍按同 id 回写主画布实例（热重载），日志文案不变 |
| 错误文案落点 | `session.error`（顶栏红字）/ `session.run_status`（运行状态），与原来同一字段语义 |

## 6. 验证结果（本批实测）

| 项 | 结果 |
|---|---|
| 构建（`source/build.ps1`，4 目标） | **0 error / 0 warning**（编译成功：`aiwrite.exe` / `api_probe.exe` / `ce_repro.exe` / `webview2_login.exe`） |
| `check_all.ps1 -SkipBuild`（20 项） | **20/20 PASS · 失败 0 项**（构建单列 0/0；含构建时为 **21/21**） |
| `aiwrite.exe --run-selftest`（CLI 自检） | **PASS**（断言零增减；含「ReadOnlyView」零拷贝 / 同源断言） |
| `api_probe --exec-selftest` | **713 通过 / 0 失败** |
| `api_probe --graph-selftest` | **151 通过 / 0 失败** |
| `ce_repro.exe 0`（窗口栈回归） | **ALL FRAMES OK (mode=0)** —— 11 帧逐一 `frame N ok` |
| `check_globals.ps1` | `checked files: 206 · violations: 0（R1 0 / R2 0 / R3 0）· fn_static: 50 / 50` · **PASS** |
| `check_includes.ps1` | `checked files: 206 · include lines: 679 · violations: 0` · **PASS** |
| `check_links.py` | `checked links: 1933 · broken: 0`（本批文档改动**之后**复测；改动前基线 1910） |
| `rg` 收尾 | `compositeEditorState(` / `CompositeEditorState` 代码行 **0**（仅注释留档）；`state.working` / `ce_push_snapshot` / `ce_name_buffers` **仅注释** |

> 等价证据口径：ce 侧没有 GUI 自动化断言，故以 **`ce_repro` 全帧对比**（mode 0 忠实模拟 `drawCompositeEditor` 的窗口栈）
> + 全库自检断言**零升降**作为「行为等价」证据 —— 与 [ARCHITECTURE.md](../refactor/ARCHITECTURE.md) §4 判据 1 的既有做法一致。

## 7. 验收判据对照（§4 判据 7）

| 判据 7 的分项 | 本批前 | 本批后 |
|---|---|---|
| 主画布渲染（`drawNodeCanvas` / `drawPropertyPanel` / `drawOutputPanel` / `app.cpp` 4 个文本函数） | ✅ `const ReadOnlyView&` | ✅ 不变 |
| ce 编辑器渲染（`drawCompositeEditor`） | ❌ 直读 `CompositeEditorState` 单例 | ✅ `const CeReadOnlyView&` |
| `CompositeEditorState` 单例 | 1 个（会话 + 工作副本 + 选中 + 标记 + 文件 + 运行） | **0**（拆成 5 对象，`ceSession()` 单一定义点） |
| 函数内 static 缓冲（`ce_name_buffers()`） | 1 条 | **0**（`CeSpec::name_bufs_`） |
| 棘轮上限 | 51 | **50** |

**判据 7 = 🟡 部分满足 → ✅ 满足。**

## 8. 遗留与风险

1. **`ce_draw.cpp` 仍 846 行**（> 700）：本批只做对象与视图改造，不切窗口函数；瘦身归 **ARCH-Z1**（含 `canvas_view` / `settings_panel` / `node_library` 等 15 个超长文件）。
2. **`ce_run.cpp` 行数上升（203 → 223）**：`state.` → `session.` 前缀变长所致，非逻辑增加；试运行域本身未拆分（Z1 视需要再切）。
3. **运行控制面仍读 `CeSession`**：`run_active` / `run_status` 是会话级瞬时状态（不是工作流数据），绘制层仍读 `ceSession()`；只读快照 `view.snapshot` 覆盖的是**读模型**部分。若后续要求「渲染零会话访问」，可把这两个字段也并入视图。
4. **`CeSpec` 与 `CeModel` 的一致性守护点**：标记写操作与显示名写回都在 `CeSpec`（不压快照 / 不置脏），调用方需先 `snapshot()` —— 与原实现同一约定，靠调用顺序保证，无编译期约束。

## 9. 回滚方式

* 回滚到本批之前：`git revert <Z2 提交>`（恢复 `CompositeEditorState` 单例 + `ce_draw` 直读 + 旧 `drawCompositeEditor` 签名；`CMakeLists.txt` 同时回退 4 个新 TU）。
* 只回滚判据 7 部分（保留拆分）：把 `drawCompositeEditor` 签名改回不带视图，函数体内 `view.*` 换回 `ceSession().model()` / `ceSession().selection()`（结构仍成立，判据 7 回落 🟡）。

> 视图本身没有非 const 访问器 —— 渲染函数从签名上就拿不到可变引用。
