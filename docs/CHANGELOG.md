# Changelog

本文件记录 AIwrite 的全部重要变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循语义化版本 `MAJOR.MINOR`（设计文档 T-15）。

**相关文档索引**

| 文档 | 说明 |
|---|---|
| [ai_writer_nodes.md](ai_writer_nodes.md) | 项目设计文档（v1.0） |
| [actionPlan/milestone_plan.md](actionPlan/milestone_plan.md) | 里程碑总体计划（M1–M6） |
| [actionPlan/M1.md](actionPlan/M1.md) … [M6.md](actionPlan/M6.md) | 各里程碑 Action Plan |
| [M1_技术验证报告.md](M1_技术验证报告.md) | M1 实测环境、验证结果与问题记录 |
| [../source/README.md](../source/README.md) | 源码构建 / 运行 / 调试说明 |

---

## [Unreleased] — M2 节点系统（P1 数据层 + P2 画布交互）已落地

**M1 收尾（配置接线 + 遗留审计）**

- `src/ui/app.cpp`：面板可见性改为**读 `config.toml` 的 `[ui]` 段**——此前 `show_library` / `show_params` **硬编码 `true`（等于忽略配置）**，现改为 `config.ui.show_node_library` / `config.ui.show_property_panel`，落实设计 §7.2「节点库 / 参数面板默认隐藏、可切换」；启动日志新增 `界面可见性（config.toml [ui]）: 节点库=… 参数面板=… Console=… 网格=…`；「视图」菜单切换时**写回** `config.toml`（M6-06 设置面板落地前的过渡方案）
- 文档（M1 收尾审计）：`actionPlan/M1.md`（§三 目录树改为实际结构、偏差校正补「`.vscode` 8 配置 / 9 任务」、M1-04 Bootstrapper 对策注明顺延 M6、§八 验收表加状态列、§十一 4 项待确认标 ✅ + 遗留归属表）；`M1_技术验证报告.md` §4.3 增「配置字段应用」；遗留登记：`recent.json`→**M2-05**、`workflow.log` 独立 sink→**M4-13**、WebView2 Runtime 检测/安装引导→**M6-02/M6-08**
- 实测：全量构建 **0 error / 0 warning**；`config.toml` 置 `true/true` 与 `false/false` 各启动一次，日志分别输出「节点库=显示，参数面板=显示」与「节点库=隐藏，参数面板=隐藏」→ **配置确认生效**（两轮退出码均 0）

**交互补强：运行按钮 UI 占位 + 网页版登录（W1）**

- `src/ui/toolbar.cpp`：新增 **「▶ 运行」按钮**（右对齐 = 顶栏右上角，设计 §6.5 / §7.1）。**本步骤只渲染 UI、不接逻辑**（禁用态 + 悬停说明），执行引擎在 M2-03（拓扑排序）/ M2-04（单线程分帧执行）落地后接线，届时连同设计里的「停止」按钮与状态栏进度一起启用
- `src/web/session_store.{h,cpp}`：网页版会话存储（设计 §8.5：Cookie **只存内存、退出即销毁**；`mask_value()` 脱敏；线程安全快照 + `clear()` 注销）
- `src/web/webview_host.{h,cpp}`：**内嵌 WebView2 有头登录窗口**，跑在独立线程（WebView2 需要 STA + 自己的消息泵），登录期间主界面保持响应；页面加载完成与每 3 秒自动重取 Cookie → 写入内存会话；`Ctrl+Alt+C` 立即提取、`ESC` 关闭；退出前由 `stop_login_window()` 显式收尾（避免静态析构撞日志关闭）
- `src/engine/graph.{h,cpp}`：`Param` 新增**条件可见性** `visible_when_param` / `visible_when_value` + `engine::param_visible()`（未设置 = 始终可见，向后兼容）；`Graph::validateParams()` 跳过条件隐藏的参数
- `src/engine/node_registry.cpp`：ProviderConfig 的 `api_base / model / api_key / api_key_ref` 标记为仅 `mode == official` 可见 → **模式切到 web 时 official 专属参数自动隐藏**
- `src/ui/property_panel.cpp`：ProviderConfig 且 `mode == web` 时显示「网页版会话」区（状态 / 来源 / 更新时间 / `ds_session_id` 脱敏值 + 「打开登录窗口」「关闭登录窗口」「注销（清会话 + 删除登录 profile）」）；official 模式给切换提示；末尾提示被隐藏的参数条数
- `src/ui/node_canvas.cpp`：节点卡片参数预览同样按可见性过滤（与面板、校验共用 `param_visible()`）
- `src/ui/app.cpp`：状态栏新增 `推理模式: 官方 API / 网页版（已登录 N 条 Cookie）/ 网页版（未登录）/ 未配置`
- `src/main.cpp`：新增 `aiwrite.exe --login-selftest [--timeout N]`——GUI 程序自动挂父控制台（且尊重已有重定向），离屏跑「WebView2 环境 → 控制器 → 导航 → 提取 Cookie」，退出码 `0=通过 / 1=失败 / 2=超时`
- `tools/api_probe.cpp`：`--selftest` 新增 **V-09 会话存储 / 参数条件可见性**（写读 → 注销清空；official 显示 `api_key`、web 隐藏 `api_key`/`api_base`，两模式校验均通过）
- `CMakeLists.txt`：`aiwrite` 链接 `${AIWRITE_WEBVIEW2}`（此前仅 CLI 工具 `webview2_login` 链接），新增 `src/web/session_store.cpp`（core）与 `src/web/webview_host.cpp`（GUI）

**实测（2026-09-23）**：全量构建 **0 error / 0 warning**；`api_probe --selftest` → `SHA3 / HTTP / 请求构造 / 配置往返 / 会话与可见性` **全 PASS**（退出码 0）；`--graph-selftest` **95 通过 / 0 失败**；`aiwrite.exe --login-selftest --timeout 30` → **PASS，Cookie 5 条（脱敏），5~7 秒，退出码 0**；GUI 冒烟首帧正常、退出码 0。


### Added（本轮）

**P1 数据层（`api_probe --graph-selftest`：初版 76 通过 / 0 失败；加入序列化断言后 95 通过 / 0 失败）**

- `src/engine/graph.{h,cpp}`：Node / Port / Param / Edge / Graph 数据模型（设计 §5.1–5.6）；端口类型兼容矩阵（同类型 ✅ / `any` 双向 ✅ / 其他 ✗）、参数校验（必填为空 / 超范围 / 枚举非法 / 文件与目录存在）、节点与连线增删、节点复制、稳定 id（`n1` / `e1`）
  - 创建类接口统一**返回 id**（而不是 `Node*`）：自检时实测到「返回 `&nodes.back()` 后被 `push_back` 重分配打悬空」的缺陷，改为返回稳定 id + `findNode(id)` 访问
- `src/engine/node_registry.{h,cpp}`：9 个 MVP 节点注册表（分类 / 端口 / 参数 / 默认值 / 范围 / 枚举 + 说明文案）
- `src/engine/undo_stack.{h,cpp}`：快照式撤销/重做，深度上限 50，新操作丢弃重做分支
- `src/ui/property_panel.{h,cpp}`：参数面板（String / Text / Int / Float / Bool / Enum / File / Directory / Color 九类控件、校验提示、密码框、重置为默认值）；控件激活时上报 `begin_edit`，保证撤销快照落在值变化**之前**
- `src/utils/file_dialog.{h,cpp}`：nativefiledialog-extended 原生文件/目录对话框（`NFD::Init/Quit` 配对封装）
- `tools/api_probe.cpp`：新增 `--graph-selftest`（注册表 / 增删 / 兼容矩阵 / 端口替换 / 可变长 / 删除连带 / 复制 / 参数校验 / 撤销重做，共 76 项断言；后续加入序列化断言 → 95 项）

**P2 画布与编辑器**

- `src/ui/node_canvas.{h,cpp}`：真实 Graph 画布（替换 M1 演示）——节点卡片（分类色条 + 状态圆点 + 端口 + 参数预览 + 校验错误）、端口按类型着色、可变长端口双圈、右键画布分类新建、右键节点复制/删除/改名、右键连线删除、拖拽连线（类型不符变红拒绝 / 输入端口替换）、双击节点聚焦改名、Ctrl+单击与框选多选、中键平移、滚轮缩放 0.1x–4.0x、稳定手柄映射（节点 = 序号；端口 = `序号<<8 | 输出位 0x80 | 下标`）、`ed::EnableShortcuts(false)` 落实「无快捷键」
- `src/ui/editor_state.{h,cpp}`：Graph + UndoStack 唯一持有者，统一「修改前压快照」的编辑操作（创建 / 删除 / 复制 / 粘贴 / 清空 / 示例工作流）
- `src/ui/node_library.{h,cpp}`：左侧节点库（按分类，点击创建到画布中心，悬停显示端口与参数清单）
- `src/ui/toolbar.{h,cpp}`：工具栏（撤销 / 重做 / 复制 / 粘贴 / 删除选中 / 新建 / 示例 + 计数与撤销栈深度；全部鼠标操作）
- `src/ui/theme.h`：分类 / 端口 / 状态配色集中定义（设计 §14.3 / §4.5 / §4.6）
- `src/ui/app.cpp`：停靠布局改为「节点库 | 画布 | 参数 + 工作流信息 / Console」；新增工具栏行、菜单「编辑」（撤销/重做/复制/粘贴/删除选中）、状态栏显示节点/连线/选中数与操作提示；启动自动创建 4 节点示例工作流
- 删除 `src/ui/node_editor_demo.{h,cpp}`（M1 演示数据画布）

**依赖**

- 新增 overlay port `source/ports/nativefiledialog-extended`（v1.3.0）+ `source/vcpkg-configuration.json`（`overlay-ports: ["./ports"]`）+ `source/vcpkg.json` 依赖项；工程 `find_package(nfd CONFIG REQUIRED)` + `nfd::nfd`（nfd.dll 由既有运行时 DLL 同步逻辑拷贝）

**文档**

- 新增 `docs/节点编辑器使用说明.md`（操作手册 + 20 项人工验证清单 + 已知限制）
- 设计文档同步：§6.4（内置 32px 网格，20px 自绘作废）、§6.6 / §6.7 实现说明、§20.1 目录结构、§20.3 依赖状态
- `docs/actionPlan/M3.md` 增加「实施进度」对照表

### Added（本轮）

**M1-04 WebView2 自检能力（`tools/webview2_login.cpp`）**

- 新增命令行选项：`--selftest`（隐藏/离屏自动跑「导航 → 提取 Cookie」全链路，**退出码 0=通过 / 1=失败 / 2=超时**）、`--url <url>`（默认 `https://chat.deepseek.com/`）、`--timeout <秒>`（自检默认 30；交互模式也可用来自动关闭）、`--hidden`（离屏窗口，不抢焦点）
- 自检结果写入 `app.log`（`[V-03] WebView2 自检：Cookie N 条 → PASS/FAIL`），并在控制台输出脱敏 Cookie 清单（名称/前4后4/属性）
- 实测：`webview2_login.exe --selftest --timeout 30` → **PASS，Cookie 5 条，1.2~2.1 秒，退出码 0**（连续两次复现一致）

**M1-05/06/07 自检增强（`tools/api_probe.cpp`）**

- `--selftest` 现覆盖四组：`V-05 SHA3` / `V-04 HTTP` / **`V-06 请求构造`** / **`V-08 配置往返`**（后两组不需要 Key、不依赖用户配置）
- 新增 `--chat "<prompt>" --dry-run`：打印 `POST /chat/completions`、Authorization（已设置时显示 `Bearer ****`）、body JSON，并断言 `model 非空 / stream=false / messages[1]{role=user, content=prompt}`；不发起网络请求，无需 Key
- `test_http` 输出连接/读取超时与证书校验状态，并把 `HTTP 请求失败` 按错误类型区分 **SKIP（网络/代理不可达，不计失败，退出码 2）** 与 **FAIL（TLS/其它，退出码 1）**
- 新增配置往返自检：临时文件上 `load（生成默认）→ 改 language/show_grid/ttl_days/model → save → load` 断言相等
- 实测：`api_probe --selftest` → `SHA3 PASS / HTTP PASS / 请求构造 PASS / 配置往返 PASS`，退出码 0；`--graph-selftest` 95 通过 / 0 失败

**工作流 JSON 序列化（设计 §4.7 schema，M2-05 核心部分）**

- `src/engine/workflow_io.{h,cpp}`：`graph_to_json` / `graph_from_json` / `save_workflow` / `load_workflow`
  - schema：`version / name / description / nodes[{id,type,title,position,params}] / edges[{id,from,to}] / viewport`
  - 反序列化以**节点注册表为准**重建端口与参数定义，参数值取自文件，缺失参数保持默认值；未注册类型与非法连线**跳过并告警**；坐标/缩放做合法性夹紧（防御损坏文件）
  - **密钥不落盘**：`is_secret` 参数（如 `api_key`）不写入、也不从文件恢复
- **默认启动工作流落盘**：启动时创建默认工作流并保存到 `~/.brain-ai/workflows/default.json`（设计 §11.3），日志记录路径
- **默认工作流新增「提供商配置」节点**：`n5 ProviderConfig(220,260)` 的 `provider` 输出接入 `n3 LLMGenerate.provider` 输入（默认工作流 = 5 节点 4 连线）
- `api_probe --graph-selftest` 增加 17 项序列化断言（结构/数量/密钥不落盘/往返一致/文件往返/非法输入被拒）→ 合计 **95 项**

### Fixed（本轮修复）

| 问题 | 根因与处理 |
|---|---|
| 参数面板下拉框（Enum）选了不生效 —— 例如「提供商配置 → 模式」无法从 `official` 切到 `web` | 根因：判定条件写成 `ImGui::Combo(...) && ImGui::IsItemDeactivatedAfterEdit()`，但 Combo 在**点击下拉项的那一帧**就返回 true，而被点击的选项是弹出层里的另一个 item，该帧 `IsItemDeactivatedAfterEdit()` 并不成立 → 值永远写不回去（`model` / `mode` 等所有枚举参数都受影响）。处理：改为**值变化即写回**（`Combo` 返回 true 时立刻写入，索引做了 `std::clamp` 保护） |
| 链接 `aiwrite.exe` 报 `LNK1163: invalid selection for COMDAT section` | 头文件里的 `inline constexpr const char* kWorkflowVersion` 触发 MSVC 链接期 inline 变量的 COMDAT 合并问题。处理：改为普通 `constexpr`（内部链接），并在文件内注明原因 |


| 问题 | 根因与处理 |
|---|---|
| **启动后界面无响应（CPU 单核打满、内存涨到 ~216 MB 后不动）——曾误判为"内存泄漏"** | **真凶**：`~/.brain-ai/node_editor.json`（imgui-node-editor 自己持久化的**节点坐标 + 视图 scroll/visible_rect/zoom**）里残留了**损坏数值**——`"node:5": {"location": {"x": -2147483520, "y": -2147483648}}` 与 `"zoom": 2.16e-07`、`visible_rect` 跨度 ±3e9。这些值正是更早"复制/粘贴"脏值事故（`GetNodePosition → FLT_MAX`）留下的；编辑器启动即恢复该视图，导致**每帧网格与剔除计算爆量** → CPU 打满、界面冻结（用临时阶段追踪定位到卡在 `ed::End()` 内部，再用 VEH/内存采样确认"CPU=1.0 秒/秒、内存平稳"以排除泄漏）。**处理**：① 删除/备份损坏的 `node_editor.json`；② 代码加**三层防护**——启动前 `sanitize_editor_settings()`（校验坐标/缩放，异常则备份改名重建）、运行期**视图自愈**（`GetCurrentZoom`/视图原点异常即 `NavigateToContent` 重置）、模型侧 `clamp_position()` 夹紧 ±100000 |
| **诊断能力补齐（新增）** | `src/utils/diagnostics.{h,cpp}`：VEH **首异常计数器**（只做原子计数，汇总时输出**抛出模块 + 偏移**，限流）+ **内存采样**（工作集/私有/峰值 + 节点/连线/撤销栈）+ 渲染循环**心跳**；全部以 `[诊断]` 行写入 `app.log`（启动前几帧 + 之后约每 30/60 秒一条），用于今后此类"卡死 / 疑似泄漏 / 异常洪流"问题的快速定位 |
| **复制/粘贴导致界面无响应（卡死）** | 根因链：粘贴会在"本帧节点绘制之后"新建节点，而同帧末尾的 `sync_positions()` 会读 `ed::GetNodePosition(新句柄)`——该手柄编辑器还不认识，函数返回 **`FLT_MAX`**（`imgui_node_editor.cpp:1676`）→ 旧代码把这个值当位置**回写进 Graph** → 下一帧 `ed::SetNodePosition(FLT_MAX)` 经 `Floor()` 变成 ~`-2.1e9`，而粘贴又是**唯一同时请求视图跟随**的操作（`request_navigate_to_content = true`）→ `NavigateToContent` 在 ~1e9 坐标上计算视图 → 绘制/断言数学溢出 → 无响应（日志实测：卡死点精确定位在粘贴那一帧）。**处理**：① 复制/粘贴 **UI 入口下线**（右键菜单 / 工具栏 / 编辑菜单，逻辑与剪贴板代码保留，M2/M3 重做）；② 加**位置守卫**（`sync_positions` 双向同步：编辑器不认识该节点时改为"模型 → 编辑器"推送，绝不回写脏值；`clamp_position()` 夹紧到 ±100000；`NavigateToContent` 前校验坐标） |
| 连线右键菜单（「删除连线」「选中该连线」）点了无效 | 第三层根因：`context_link_handle` 是**普通局部变量，每帧被重置为 0**，而右键菜单在**后续帧**才被点击 → 算出 `edge_id = "e0"` → 找不到连线（对照：`context_node` 是 `static`，所以节点菜单一直正常）。**处理**：按需求**移除该菜单**（含 `ShowLinkContextMenu` 分支与悬停兜底），右键菜单链回到「节点 > 背景」；`context_node_handle` 一并改为 `static` 消除同类隐患；连线删除保留已验证可用的「左键单击连线选中 → 工具栏【删除选中】」 |
| 节点端口不区分输入/输出、连线方向看不清（端口圆点位置混乱） | `draw_pin` 原先把「圆点 + 名称」和「名称 + 圆点」都按同一列左对齐排布，输出端口圆点紧跟在文字后面而不是节点右边缘。处理：重写为**真正的左右两列**——输入行 `[圆点][名称]` 贴节点左边缘、输出行 `[名称][圆点]` 右对齐到 `kNodeContentWidth = 215px`（节点内容宽度统一由标题行的占位 Dummy 保证），连线锚点用 `PinPivotAlignment(0,0.5)` / `(1,0.5)` 分别取端口矩形左中 / 右中 |
| 连线无法删除（右键「删除连线」点了没反应） | **两个叠加原因**：① 右键菜单写成三个**并列的 `if`** 且 `ShowBackgroundContextMenu()` 在最前，背景菜单先消费掉右键事件 → 节点/连线右键菜单根本打不开；② 即便菜单打开，官方做法 `ed::DeleteLink()` 也会**静默失效**——其内部 `DeleteItemsAction::Add()` 开头就是 `if (Editor->GetCurrentAction() != nullptr) return false;`，而右键菜单打开期间"上下文菜单动作"正是 current action（官方 blueprints 的 Delete 菜单项同样有此问题）。**处理**：① 改为官方 `blueprints-example` 的 **else-if 链（节点 > 连线 > 背景）**；② 放弃 `ed::DeleteLink()`，统一以 **Graph 为唯一真相直接删边**（画布随后不再提交该连线，编辑器会在本帧 `End()` 因 `!m_IsLive` 自动回收内部连线对象，见 `imgui_node_editor.cpp:1307`）。现在连线有 3 条鼠标删除路径：右键连线 →「删除连线」、左键点连线选中 → 工具栏「删除选中」、画布右键菜单 →「删除选中（节点 / 连线）」，均在修改前压撤销快照 |
| 拖动端口连线时弹出 imgui-node-editor 断言 `IM_ASSERT(false == m_InActive)`（`CreateItemAction::Begin()`） | 根因：`ed::BeginCreate()` 内部会执行 `CreateItemAction::Begin()`（`m_InActive = true`），**只有 `ed::EndCreate()` 复位**；原实现写成 `if (!ed::BeginCreate(...)) return;`，未成对调用导致 `m_InActive` 恒为 true，下一帧再进 `Begin()` 即断言失败。处理：改为无条件调用 `EndCreate()`/`EndDelete()`（与官方 widgets/blueprints 示例一致），并把该契约写进 `node_canvas.cpp` 注释 |
| CMake 警告 `Compatibility with CMake < 3.10 will be removed` / `Update the VERSION argument <min> value. Or, use the <min>...<max> syntax` | 来源是 vcpkg 自带工具链 `scripts/buildsystems/vcpkg.cmake:40,878` 的 `cmake_policy(VERSION 3.7.2)`。处理：本项目 `cmake_minimum_required` 改用区间写法 **`3.25...4.6`**（策略版本取 4.6，新版 CMake 不再因策略未设置告警），并设置全局策略下限 **`CMAKE_POLICY_VERSION_MINIMUM=3.10`**（策略版本 = max(声明值, 下限)，既消除该 deprecation 警告，也容忍仍声明老版本号的历史代码/端口） |
| 在 VS Code「CMake Tools」里构建报 `Could not find a package configuration file provided by "imgui"` | 根因：vcpkg 的 `VCPKG_INSTALLED_DIR` 由 `dev.ps1` 环境变量提供，一旦构建树的缓存缺失/被重建，CMake 自行重新配置时取不到该路径回退到 `<build>/vcpkg_installed`，于是找不到 imgui。处理：把 vcpkg 相关路径**写进 `CMakePresets.json` 的 `cacheVariables`**（`VCPKG_INSTALLED_DIR`、`VCPKG_TARGET_TRIPLET`、`VCPKG_HOST_TRIPLET`、`VCPKG_MANIFEST_DIR`、`VCPKG_MANIFEST_INSTALL=OFF`、`CMAKE_PREFIX_PATH`、`CMAKE_POLICY_VERSION_MINIMUM`），使「仅 cmake --build / 无 dev.ps1 环境」的配置也能成功（已实测干净环境 configure + build 通过） |
| `install-deps.cmd` 无法确认端口构建使用的 CMake | 增加 `where cmake` + `cmake --version` 输出（实测为工程自带 3.31.6），并在 `third_party/cmake-3.31.6` 缺失且系统 CMake ≥ 4.0 时给出提示与解决办法（triplet 注入 `VCPKG_CMAKE_CONFIGURE_OPTIONS=-DCMAKE_POLICY_VERSION_MINIMUM=3.5`） |

### Changed（本轮）

- **UI 入口下线（暂停，逻辑保留）**：复制/粘贴的 4 个入口（画布右键菜单 / 节点右键菜单 / 工具栏按钮 / 编辑菜单）改为禁用占位项 `复制/粘贴（暂停，M2/M3 重做）`；`EditorState::copy_selection/paste_clipboard/has_clipboard` 与剪贴板数据**原样保留**（含暂停原因与接回前置条件的注释）
- **连线右键菜单移除**：`ShowLinkContextMenu` 分支、「悬停命中」兜底与 `##link_menu` 弹窗全部删除；连线删除仅保留「左键单击选中 → 工具栏【删除选中】/ 画布右键【删除选中（节点 / 连线）】」
- CMake 工程版本 `0.1.0` → **`0.2.0`**（`project(VERSION)`、界面版本号与「关于」菜单同步）
- `source/install-deps.cmd`：CMake 版本策略注释与自检输出
- `source/CMakePresets.json`：新增 `VCPKG_HOST_TRIPLET`、`CMAKE_POLICY_VERSION_MINIMUM` 缓存项
- **M1 文档漂移校正**：`docs/M1_技术验证报告.md`（产物路径 `build/ninja-debug/bin/` → `build/bin/`、删除方式由 `Delete` 键改为鼠标菜单、launch 7 配置 / tasks 9 任务、产出物清单更新）、`docs/actionPlan/M1.md`（新增"实施偏差校正"）、`source/README.md`（7 配置 / 9 任务）

### 待完成（均需人工执行一次）

- ✅ **M1 实施项已全部完成**（2026-09-23 校正）：M1-01 ~ M1-07、V-01/V-02/V-04/V-05/V-07/V-08、A-01/A-02/A-03/A-07/A-08 均已落地或验证；文档漂移（产物路径 / 删除方式 / 任务与配置数量）已修正
- ⏳ **V-03**：运行 `build/bin/webview2_login.exe` 手动登录 DeepSeek → `Ctrl+Alt+C` 提取 Cookie（工具已就绪，已验证 WebView2 环境可创建）——**需您本人操作一次**
- ⏳ **V-06**：设置环境变量 `DEEPSEEK_API_KEY` 后执行 `api_probe.exe --chat "..."`，验证官方 API 生成文本——**需您的 Key**
- ⏳ **M1 收尾**：上述 V-03 / V-06 完成后，A-09（V-01 ~ V-08 全通过）即闭环
- ⏳ **M3 验收（原 A-03）**：按 `docs/节点编辑器使用说明.md` §8 的 **22 项**清单在界面中逐项确认（删除一律走鼠标菜单；复制/粘贴已暂停）
- ⏳ **M2 计划待审核**：`docs/actionPlan/M2.md` + 对话中整理的"剩余任务（M2-03 / M2-06 / M2-04 / M2-05）与 K1–K5 决策"

---

## [0.1.0] - 2026-09-22 — M1 技术验证 + 项目骨架

### Added

**环境与构建体系**

- 工具链：MSVC **14.50**（VS 2026 Insiders）+ 系统安装 **CMake 4.4.3** + `Visual Studio 18 2026` 生成器（无需 Developer Prompt / vcvars64）
- `source/build.ps1`：一键配置 + 编译；`source/dev.ps1`：会话环境（vcpkg 变量 + CMake 解析）；`source/install-deps.cmd`：vcpkg 依赖安装
- `source/CMakePresets.json`：配置预设 `default`，编译预设 `debug` / `release`
- vcpkg manifest 模式（`source/vcpkg.json`）：依赖安装树 `vcpkg-installed/`、下载与二进制缓存 `vcpkg-cache/`（均在 F: 盘，不占用 C 盘）
- 构建产物统一在 `build/bin/`（exe 同级自动拷贝所需 vcpkg 运行时 DLL）

**应用骨架（M1-02 / M1-07）**

- `src/main.cpp`：程序入口（`--console` 可选分配控制台）
- `src/ui/app.cpp`：GLFW + OpenGL3 + Dear ImGui **1.90.7 (docking)**；深色主题（设计文档 14.3/14.4）；微软雅黑 14px + Consolas 13px（运行时加载系统字体，不把字体文件放进仓库）；停靠布局（节点画布 / Console / 工作流信息）+ 菜单栏 + 状态栏
- `src/ui/console_panel.cpp`：Console 面板（级别过滤 + 搜索 + `ImGuiListClipper` 虚拟化 + 自动滚动，设计文档 12.4）
- `src/utils/paths.cpp`：`~/.brain-ai` 数据目录（outputs / workflows / snapshots / logs / webview2 / config.toml / imgui.ini）
- `src/utils/log.cpp`：spdlog 三级别 + 双 sink（`app.log` 10MB × 5 轮转 + 控制台）+ 内存环形缓冲 10000 条 + 2 秒周期落盘
- `src/utils/config.cpp`：toml++ 读写 `config.toml`（字段与设计文档 20.2 一致，缺省自动生成）
- `src/utils/crypto.cpp`：SHA3-256（OpenSSL EVP，M4 PoW 复用）

**节点画布（M1-03）**

- `src/ui/node_editor_demo.cpp` + `third_party/imgui-node-editor` **v0.9.3**（源码集成）：2 个演示节点 + 1 条连线，支持拖拽 / 缩放 / 平移 / 新建连线 / 删除

**M1 验证工具**

- `tools/api_probe.cpp`：`--selftest`（V-04 HTTP + V-05 SHA3）、`--sha3`、`--http`、`--chat`（V-06，Key 只从环境变量读取）
- `tools/webview2_login.cpp`：WebView2 登录 + Cookie 提取（`Ctrl+Alt+C`，脱敏打印、仅内存、不落盘）

**工程化与文档**

- `.vscode/launch.json`（6 个调试配置）、`.vscode/tasks.json`（8 个任务）、`.vscode/settings.json`
- `source/README.md`（构建 / 运行 / 调试 / 常见问题）
- `docs/M1_技术验证报告.md`（环境基线、验证结果、问题与解决记录）
- 本 CHANGELOG

### Fixed

| 问题 | 处理 |
|---|---|
| 节点画布首帧崩溃 `0xC0000005`（进程假死、并留下僵尸进程占用 exe 导致后续 `LNK1104/LNK1168`） | 根因：`ed::GetStyle()` 实现为 `s_Editor->GetStyle()`，依赖"当前编辑器"；改为先 `ed::SetCurrentEditor()` 再应用样式 |
| imgui-node-editor 与 imgui 1.90.7 重复定义 `ImVec2 operator==/!=`（C2084） | 对 vendored `imgui_extra_math.inl` 打最小补丁（`#ifndef IMGUI_DEFINE_MATH_OPERATORS` 包裹，文件内已标注） |
| `Microsoft::WRL::Callback` 未定义 | 该符号声明在 `<wrl/event.h>`（不是 `wrl/implements.h`） |
| `LoadCursorW(nullptr, IDC_ARROW)` 类型不匹配 | 编译选项补充 `UNICODE` / `_UNICODE` |
| 运行中的进程占用 exe / DLL 导致链接或拷贝失败 | 运行时 DLL 同步改为逐文件、失败仅告警 |
| 日志长时间不落盘 | 增加 `spdlog::flush_every(2s)` |
| 依赖发现失败 | Windows 下 config 模式不搜索 `<prefix>/share/<pkg>`（显式追加 `share` 到 `CMAKE_PREFIX_PATH`）；`tomlplusplus` 端口无 CMake config（手工建导入目标）；`webview2` 目标名为 `unofficial::webview2::webview2` |

### Changed

- 构建体系统一为 **系统 CMake 4.4.3 + `Visual Studio 18 2026` 生成器**，**单一构建目录 `build/`**（含可用 VS 2026 打开的 `aiwrite.slnx`），产物统一在 `build/bin/`
- vcpkg 端口构建固定使用工程自带的 CMake `third_party/cmake-3.31.6`（本快照 ports 与 CMake 4.x 不兼容），仅作用于依赖安装
- M1 Action Plan 与设计文档中的环境/目录描述按实测结果同步修正

### Removed

- 调试期临时的分阶段启动参数及相关代码（`--minimal` / `--imgui-only` / `--no-node-editor`）：**只保留单一 `aiwrite.exe`**（`--console` 保留用于调试日志）
- 原 Ninja 构建预设与 `build/ninja-debug` 目录

### Verified（M1 验证项）

| 编号 | 验证项 | 结果 |
|---|---|---|
| V-01 | ImGui 窗口（中文显示） | ✅ 通过 |
| V-02 | 节点编辑器（渲染） | ✅ 通过（交互待人工确认） |
| V-03 | WebView2（登录 / 取 Cookie） | ⚠️ 环境创建成功，登录与 Cookie 待人工 |
| V-04 | HTTP | ✅ `HTTP 401 / 190 ms` |
| V-05 | SHA3-256 | ✅ 与 NIST 向量一致 |
| V-06 | DeepSeek API | ⏳ 待 API Key |
| V-07 | spdlog 写文件 | ✅ 通过 |
| V-08 | toml++ 读配置 | ✅ 通过 |
