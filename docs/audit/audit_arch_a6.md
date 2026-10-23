# audit_arch_a6：ARCH-A6 落地审计报告（ui 层文件级可变状态逐文件对象化 · 60 条 → 0）

> 日期：2026-10-23 · 批次：**阶段 3 第六批（ARCH-A6）** · 承接 [audit_arch_a5.md](audit_arch_a5.md)（判据 2 🟡 · 基线 87 条）
> 结论一句话：**ui 层文件级 static 可变状态 60 条全部清零**（`check_globals` 87 → **27**，ui 层 60 → **0**），
> 每个面板/缓存都拿到一个**显式状态对象**，唯一实例由 `EditorController` 持有并注入；构建 0/0、`check_all` **21/21** PASS。

---

## 1. 背景与目标（承接 §1.1 规则 6 / §4 判据 2）

A5 把规则 6 变成机器判据（`check_globals.ps1` + 棘轮基线），并在同批清掉**跨 TU `extern`**；
剩余 **87 条**文件级 `static` 可变状态里，**ui 层占 60 条（69 %）**，且分布极不均：

* `settings_panel.cpp` 19 条 —— 整个 Config 工作副本 + 两个「新建条目」模态草稿；
* `texture_cache.cpp` 12 条 —— 纹理 LRU / 统计 / 帧号；
* `node_canvas.cpp` 11 条 —— imgui-node-editor 上下文、小地图、吸附、拖拽标记。

这批不做「机械搬进 struct」，而是**按归属落到真正的所有者的对象上**：UI 瞬时状态归 `EditorController`，
渲染资源归一个 `TextureCache` 类实例 —— 顺带把 A7（删基线切严格 0）的前置条件做掉 69 %。

## 2. 归属决策（为什么是 `EditorController`）

| 候选 | 结论 | 理由 |
|---|---|---|
| 全局单例（函数内 `static X& instance()`） | ❌ 否 | 只是把「文件级 static」换成「函数内 static」，规则 6 的机器判据绕得过去，人看得见 |
| 各面板自己 `new` 一份 | ❌ 否 | 菜单 / 首启引导 / 主循环三个入口必须看到**同一份**（打开态、草稿都跨入口） |
| **`EditorController` 持有 + 逐帧传入** | ✅ 采用 | 生命周期 = 窗口（启动构造 / 退出析构）；三个入口都能拿到 `state`；多窗口并按只需换实例 |

`EditorController` 本批新增 **7 个成员**（全部是「不拥有服务的界面临时状态 / 渲染资源」）：

```
SettingsPanelState     settings_panel_      // 19 条：窗口开关 + Config 工作副本/基线 + 两个新建模态草稿
OutputPanelState       output_panel_        //  2 条：大图预览弹窗请求 / 目标路径
TextureCache           textures_            // 12 条：纹理 LRU / 字节预算 / 帧号 / 统计
NodeCanvasState        node_canvas_         // 11 条 + 3 条函数内 static
CompositeDialogsState  composite_dialogs_   //  7 条：新建 / 打包上级节点对话框
ConsolePanelState      console_panel_       //  3 条：级别过滤 / 搜索框 / 自动滚动
NodeLibraryState       node_library_        //  6 条：右键菜单 / 删除·复制二次确认
```

## 3. 逐文件改动（60 条 / 7 文件）

| 文件 | 原状 | 现状 | 关键不变量（行为未变） |
|---|---|---|---|
| `settings_panel.cpp` | **19** 条（`g_open` / `g_loaded` / `g_edit` / `g_baseline` / `g_error` / `g_notice` / `g_site_*` 6 / `g_off_*` 7） | `SettingsPanelState`；`openSettingsPanel(state)` / `drawSettingsPanel(state, config)` | ① 只有 `draw()` 改 `edit`；② `open()` 重置 `loaded` ⇒ 每次打开重取调用方 Config；③ 「保存」仍走 `saveConfig` + 回写调用方 + `visibility_changed` 口径不变 |
| `texture_cache.cpp` | **12** 条（`g_cache` / `g_order` / `g_capacity` / `g_byte_budget` / `g_bytes` / `g_frame` / 6 个统计） | **`class TextureCache`**（`textureFor` / `beginFrame` / `releaseAll` / `setCapacity` / `setByteBudget` / `stats`） | 键 = 路径+修改时间+大小；LRU + **逐帧保护** + 4× 硬上限 + 256 MB 预算；失败结果也缓存；**析构不碰 GL**（调用方在销毁上下文前 `releaseAll()`） |
| `node_canvas.cpp` | **11** 条（`g_context` / `g_canvas` / `g_first_frame` / `g_drag_in_progress` / `g_alt_pan_active` / `g_show_minimap` / `g_snap_enabled` / `g_snap_distance` / `g_minimap` / `g_prev_positions`；`g_delete_in_progress` 声明但**从未使用**） | `NodeCanvasState` + 3 条函数内 static（右键落点 / 作用节点 id / 节点手柄）一并进去 | 拖拽只压一次快照；Alt 平移临时改绑定；对齐吸附阈值仍是**屏幕像素**；小地图默认开；`g_delete_in_progress` 直接删除（死状态） |
| `ce_draw.cpp` | **7** 条（新建/打包对话框开关 + 名称缓冲 + 错误文案 + 调试自动确认） | `CompositeDialogsState`；`compositeNewDialog(editor)` / `compositeEditorTestDriveNewDialog(editor)` 补上下文参数 | 名称缓冲打开时清空；创建失败**保留弹窗**报错；打包需 ≥2 选中（守 R8：不静默替换） |
| `node_library.cpp` | **6** 条（右键 id / 菜单开关 / 删除·复制 pending + 确认开关） | `NodeLibraryState`（`drawNodeLibrary` 已带 `editor`，无需改签名） | 右键命中 → `PushID` 作用域外 `OpenPopup`；删除/复制都要二次确认 |
| `console_panel.cpp` | **3** 条（`g_level_filter` / `g_search` / `g_auto_scroll`） | `ConsolePanelState` + `LevelFilter` 提到头文件；`drawConsolePanel(title, open, state, height)` | 上滚自动暂停、回到底部恢复；搜索/级别过滤仍走同一 `log::filter`（与 `consoleVisibleText` 同源） |
| `output_panel.cpp` | **2** 条（`g_preview_requested` / `g_preview_path`） | `OutputPanelState`（`drawOutputPanel` 已带 `state`，内部 helper 透传） | 点击缩略图 → 本帧请求 OpenPopup；弹窗只读放大，不吞 PD-07 |

## 4. 顺带修掉的三处「隐式单实例」

1. **`CanvasContext::textures`**（`canvas_view.h`）：共享绘制层不再直接调全局 `textureFor`，改为
   上下文里的 `TextureCache*`（`nullptr` ⇒ 卡片不画图，绝不崩）。主画布与上级编辑器共用同一个缓存实例。
2. **`LevelFilter` 出匿名命名空间**：`console_panel.h` 公开（序号 0/1/2/3 与 `consoleVisibleText` 口径一致），
   面板状态才可能是一个对象。
3. **`drawConsolePanel` / `compositeNewDialog` / `compositeEditorTestDriveNewDialog` 补上下文参数**：
   原先这三个入口靠文件级状态「摸黑」拿面板/编辑器，现在参数是唯一来路（菜单 / 命令 / 主循环三处同源）。

## 5. 验证结果（本批实测）

```
[① 构建]            0 error / 0 warning
[⑥ check_includes]  201 文件 · include 行 666 · violations 0
[⑦ check_globals]   201 文件 · violations 27（R1 0 / R2 27 / R3 0）· baseline 27 · new 0 · fixable 0
                    （本批从 87 → 27；ui 层 60 → 0，剩余 27 条**全部是函数内 static**，属 A7 范围）
[⑧ check_links]     1897 链接 · broken 0
[② api_probe]       --selftest PASS · exec 713/0 · graph 151/0 · diag-assert PASS · dep-check PASS
                    --settings-assert PASS · --site-assert 50/50 · --official-assert 40/40 · --autosave-assert 32/32
[② CLI]             aiwrite --run-selftest PASS · ce_repro ALL FRAMES OK (mode=0)
[③④ 夹具/GUI]       poll_answer_gate 44/0 · send_chain 17/0 · dom_gate 15/0 · upload-selftest 72/0
[⑨ 打包]            payload 19 文件 / 35.4 MB PASS
=== check_all 汇总：共 21 项 · 失败 0 项 ===
```

## 6. 验收判据对照（§4 判据 2）

| 口径 | A5 后 | A6 后 |
|---|---|---|
| 跨 TU `extern` 变量 | 0 ✅ | 0 ✅ |
| **文件级** `static` 可变状态 | 87（ui 60 / web 15 / utils 12） | **27（ui 0 / web 15 / utils 12）** |
| 剩余形态 | 文件级 + 函数内 | **全部是函数内 `static`**（27 条 / 13 文件） |
| 判据 2 状态 | 🟡 | 🟡（**再进一大步**：ui 层已零；待 A7 清 27 条 + 删基线 ⇒ ✅） |

> 注：`check_globals.ps1` 的 R2 规则把**函数内** `static` 也算作「跨帧可变状态」并登记基线。
> 也就是说 A7 = 「把这 27 条函数内单实例也对象化，然后删掉基线文件、切严格零容忍」，
> 不再有「文件级 vs 函数内」的口径模糊地带。

## 7. 已知限制

1. **`TextureCache` 不带析构释放**：GL 上下文可能已销毁，析构里 `glDeleteTextures` 会崩 —— 仍由
   `app.cpp` 在上下文销毁前显式 `state.textures().releaseAll()`（与 A6 前一致，多实例化后这条契约更重要）。
2. **`NodeCanvasState::context` 无 `ed::DestroyEditor`**：与 A6 前相同（进程退出由 OS 回收），未在本批扩大范围。
3. **`EditorController` 成员变多**：7 个 UI 状态对象让它从「纯编排」略胖，但每个都是**被动状态容器**
   （无服务、无所有权、无跨线程），仍是 §1.8.10 的「UI 瞬时状态」定位；服务注入接口一个没加。

## 8. 回滚点

* 提交前工作树 = `e339a96`（A5 审计入库）。
* 本批单次提交，回滚 `git revert` 即可；`check_globals` 基线同时回到 87 条，仍能拦住新增。
