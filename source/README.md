# AIwrite 源码工程（M1 骨架 + M2 节点系统 P1/P2）

> 设计文档：`../docs/ai_writer_nodes.md`　Action Plan：`../docs/actionPlan/`（[M1](../docs/actionPlan/M1.md)、[M2](../docs/actionPlan/M2.md)…）
> 验证报告：`../docs/M1_技术验证报告.md`　节点编辑器手册：`../docs/节点编辑器使用说明.md`
> 变更记录：`../docs/CHANGELOG.md`　文档总览：`../docs/README.md`
> 技术栈：C++20 / MSVC 14.50（VS 2026 Insiders）/ CMake **4.4.3**（系统安装）/ VS 2026 生成器 / vcpkg / Dear ImGui 1.90.7(docking) / imgui-node-editor v0.9.3 / nativefiledialog-extended 1.3.0（overlay port）

---

## 1. 目录结构

```
AIwrite/
├── .vscode/                    VS Code 构建/调试配置（launch.json / tasks.json / settings.json）
├── build/                      ★ 唯一的构建目录（Visual Studio 2026 生成器）
│   ├── aiwrite.slnx            可用 Visual Studio 2026 直接打开
│   └── bin/                    产物：aiwrite.exe / api_probe.exe / webview2_login.exe
├── source/                     工程根（CMakeLists.txt 所在）
│   ├── src/                    main.cpp、ui/、engine/、utils/（nodes/、ai/ 为 M4/M5）
│   ├── tools/                  api_probe.cpp（V-04/05/06 + 图模型自检）、webview2_login.cpp（V-03）
│   ├── ports/                  overlay port（nativefiledialog-extended 1.3.0；vcpkg 快照缺此包）
│   ├── third_party/            imgui-node-editor v0.9.3（源码集成 + 最小补丁）
│   ├── cmake/                  构建脚本片段（运行时 DLL 拷贝）
│   ├── build.ps1               一键配置 + 编译
│   ├── dev.ps1                 会话环境（vcpkg 变量 + CMake 解析）
│   ├── install-deps.cmd        vcpkg 依赖安装（写入 F: 盘缓存）
│   ├── CMakePresets.json       preset：default（配置）/ debug、release（编译）
│   ├── vcpkg.json              依赖清单（manifest 模式）
│   └── vcpkg-configuration.json overlay-ports: ["./ports"]
├── third_party/cmake-3.31.6/   工程携带的 CMake 3.31（仅供 vcpkg 端口构建使用）
├── vcpkg-cache/                vcpkg 下载/二进制缓存/buildtrees（全部在 F: 盘）
└── vcpkg-installed/            vcpkg 依赖安装树（x64-windows/include、lib、bin）
```

数据目录（运行程序时自动创建）：`C:\Users\<用户>\.brain-ai\`
`outputs/`、`workflows/`、`snapshots/`、`logs/app.log`、`config.toml`、`imgui.ini`、`node_editor.json`、`webview2/`

---

## 2. 环境说明

| 组件 | 说明 |
|---|---|
| CMake | **系统安装**（`winget install Kitware.CMake` → `C:\Program Files\CMake\bin`，已在机器 PATH 中） |
| 生成器 | `Visual Studio 18 2026`（x64），由 CMake 自动准备 MSVC 环境，**不需要** Developer Prompt / vcvars64 |
| vcpkg | `C:\dev\vcpkg`（2024-04 版）；依赖与缓存都在 F: 盘的 `vcpkg-installed/`、`vcpkg-cache/` |
| 工程自带 CMake | `third_party/cmake-3.31.6`，**只用于 vcpkg 端口构建**（旧 ports 与 CMake 4.x 不兼容） |
| 依赖 | imgui 1.90.7(docking)、glfw3 3.4、cpp-httplib 0.15.3、openssl 3.3.1、spdlog 1.14.1、nlohmann-json 3.11.3、toml++ 3.4.0、stb、webview2 1.0.2277.86、**nativefiledialog-extended 1.3.0（overlay port，见下）** |
| overlay port | vcpkg 快照缺 `nativefiledialog-extended`：`source/ports/nativefiledialog-extended/` + `source/vcpkg-configuration.json`（`overlay-ports: ["./ports"]`），随 `install-deps.cmd` 一起构建安装（实测约 17 秒），工程侧 `find_package(nfd CONFIG REQUIRED)` + `nfd::nfd` |

---

## 3. 构建（三选一）

**① VS Code 任务（推荐）**：`Ctrl+Shift+B` 执行默认任务 **build**；`F5` 会先自动构建再调试；
`Terminal → Run Task` 还有：`build (release)`、`build: 重新配置`、`vcpkg: 安装/更新依赖`、`run: aiwrite`、`verify: api_probe 自检`、`log: 查看 app.log`。

**② 命令行（无需任何开发者环境）**：

```powershell
cd F:\GameDao\Tools\AIwrite\source
.\build.ps1                      # Debug（默认）
.\build.ps1 -Config release      # RelWithDebInfo
.\build.ps1 -Reconfigure         # 清空 build 后重新配置
.\build.ps1 -Target aiwrite      # 只编译主程序
```

等价的纯 CMake 命令（脚本只是帮你设好 vcpkg 环境变量）：

```powershell
cd F:\GameDao\Tools\AIwrite\source
cmake --preset default           # 配置（VS 2026 x64）
cmake --build --preset debug     # 编译 Debug
cmake --build --preset release   # 编译 RelWithDebInfo
```

**③ Visual Studio 2026**：直接打开 `F:\GameDao\Tools\AIwrite\build\aiwrite.slnx`，选 `aiwrite` 为启动项目后 F5。

产物统一在 **`F:\GameDao\Tools\AIwrite\build\bin\`**（exe 同级已自动拷贝所需 vcpkg DLL，可直接双击运行）。

> 依赖变更（改过 `source/vcpkg.json`）后：先跑 `Terminal → Run Task → vcpkg: 安装/更新依赖`（或 `source\install-deps.cmd`），
> 再执行 `build: 重新配置`。CMake 配置阶段不会自动触发 vcpkg 安装（`VCPKG_MANIFEST_INSTALL=OFF`），以免误写 C 盘。

---

## 4. 运行与验证

程序只有一个版本（无分阶段参数），产物在 `build\bin\`：

```powershell
cd F:\GameDao\Tools\AIwrite\build\bin
.\aiwrite.exe                  # 主程序（停靠界面 + 节点画布 + Console + 工作流信息）
.\aiwrite.exe --console        # 额外分配控制台，日志同时输出到 stdout
```

M1 验证工具：

```powershell
.\aiwrite.exe                 # 主程序（GUI）：节点画布 + 参数 + 网页版登录
.\aiwrite.exe --console       # 同上，并额外分配控制台窗口显示日志
.\aiwrite.exe --login-selftest --timeout 30
                              # 网页版登录自检（离屏）：退出码 0=通过 / 1=失败 / 2=超时
.\api_probe.exe --selftest                  # V-04 HTTP + V-05 SHA3 自检
.\api_probe.exe --graph-selftest            # 图模型/注册表/撤销栈/序列化 自检（95 项断言，无需网络）
.\api_probe.exe --sha3 "abc"                # 单次 SHA3-256
$env:DEEPSEEK_API_KEY="sk-..." ; .\api_probe.exe --chat "你好"   # V-06（需 Key）
.\webview2_login.exe --selftest --timeout 30   # V-03 自检：离屏跑「导航 → 提取 Cookie」，退出码 0=PASS/1=FAIL/2=超时
.\webview2_login.exe                        # V-03 交互：打开站点手动登录（Ctrl+Alt+C 提取 Cookie）
.\webview2_login.exe --ephemeral            # 使用临时 profile（退出丢弃登录态）
```

> 面板可见性由 `~/.brain-ai/config.toml` 的 `[ui]` 段控制（`show_node_library` / `show_property_panel` /
> `show_console`，设计 §7.2 默认隐藏）；在菜单「视图」里勾选后会自动写回配置，下次启动生效。
> `webview2_login --selftest` 结果写入 `~/.brain-ai/logs/app.log`（`[V-03]` 行），控制台输出脱敏 Cookie 清单
> （名称 + 前4后4 + 属性）；Cookie 仅内存、不落盘（设计 §8.4）。
> 若手工强杀该工具，可能留下 `msedgewebview2.exe` 子进程：用
> `Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'"` 查看其命令行中的 `--user-data-dir`，
> **只清理指向 `\.brain-ai\webview2` 的那些**（`msedgewebview2.exe` 也被 Windows 小组件等使用，勿误杀）。

`webview2_login` 快捷键：**Ctrl+Alt+C** 提取 Cookie（脱敏打印），**ESC** 退出。
运行日志：`C:\Users\<用户>\.brain-ai\logs\app.log`（每 2 秒落盘，可直接 tail）。

**诊断行（`app.log` 中的 `[诊断]`）**：为排查"卡死 / 疑似内存泄漏 / 异常洪流"加入，正常运行时也保留（限流输出）：

| 行 | 含义 |
|---|---|
| `[诊断] 内存: 工作集 … / 私有 … / 峰值 …  |  节点 N / 连线 M / 撤销栈 K` | 每 30 秒一次；数值平稳即无泄漏 |
| `[诊断] 心跳 帧=… 最小化=… FPS=…` | 启动前几帧 + 之后约每 60 秒；用于确认渲染循环在跑 |
| `[诊断] 首异常累计 N 次；首个于 <模块>+0x…` | 出现 C++ 异常/访问冲突时输出，含**抛出模块**，用于定位第三方依赖问题 |

画布设置文件 `~/.brain-ai/node_editor.json` 保存节点坐标与视图；若被写坏会导致 CPU 打满 + 界面无响应，
程序启动时会自动校验并备份为 `node_editor_bad_<时间>.json` 后重建（详见 `docs/节点编辑器使用说明.md` §9）。

---

## 5. 在 VS Code 中调试（F5）

已提供 `.vscode/launch.json`（7 个配置）与 `.vscode/tasks.json`（9 个任务）：

1. 打开工作区 `F:\GameDao\Tools\AIwrite`
2. 按 **F5** → 选择配置：
   - `aiwrite（主程序）`：启动完整程序（先自动构建）
   - `aiwrite（带控制台）`：日志同时打到集成终端
   - `api_probe：M1 自检` / `api_probe：DeepSeek Chat`（后者需系统环境变量 `DEEPSEEK_API_KEY`）
   - `webview2_login：登录并提取 Cookie`
   - `附加到正在运行的进程`
3. `Ctrl+Shift+B` 只构建；`Terminal → Run Task` 可选其它任务（release、重新配置、装依赖、运行、看日志）。

调试符号：Debug 版自带完整 PDB，`justMyCode: false` 可进入 imgui / imgui-node-editor 内部。

---

## 6. 常见问题

| 现象 | 处理 |
|---|---|
| `LNK1104 / LNK1168: cannot open aiwrite.exe` | 上一次运行的进程仍占用 exe：先 `taskkill /F /IM aiwrite.exe`；若因崩溃留下僵尸进程，可把 exe 改名（`Move-Item aiwrite.exe aiwrite_old.exe`）后重新编译 |
| 改了 `vcpkg.json` 后找不到新库 | 先跑 `install-deps.cmd`（任务 `vcpkg: 安装/更新依赖`），再执行 `build: 重新配置` |
| 在 VS Code「CMake Tools」里构建报 `Could not find a package configuration file provided by "imgui"`（或其它 vcpkg 包） | 说明这次配置没拿到 vcpkg 路径（缺少 `dev.ps1` 环境变量、或构建树缓存被清空）。现已把 vcpkg 路径写入 `source/CMakePresets.json` 的 `cacheVariables`，只需**重新配置一次**即可：VS Code 里执行任务 `build: 重新配置`；命令行 `cmake --preset default`。之后 `cmake --build` / CMake Tools 直接构建都不会再依赖环境变量 |
| CMake 警告 `Compatibility with CMake < 3.10 will be removed` / 报 `Compatibility with CMake < 3.5 has been removed` | 前者来自 vcpkg 工具链内部的 `cmake_policy(VERSION 3.7.2)`，已通过全局策略下限消除：本项目 `cmake_minimum_required(VERSION 3.25...4.6)` + `CMAKE_POLICY_VERSION_MINIMUM=3.10`。后者是 CMake 4.0 移除 `<3.5` 兼容所致，出现在**老端口源码**（glfw3 3.4 / nlohmann-json 3.1 等）——端口构建请走 `install-deps.cmd`（自动用自带的 CMake 3.31.6）；若必须用 CMake 4.x，可在 triplet 里加 `set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCMAKE_POLICY_VERSION_MINIMUM=3.5")` |
| 中文显示为方块 | 检查 `C:\Windows\Fonts\msyh.ttc` 是否存在（或用 `assets/fonts` 覆盖字体） |
| CMake 找不到 vcpkg 包 | 确认 `vcpkg-installed/x64-windows/share` 下有对应目录（依赖是否装全） |
| `cmake` 命令找不到 | CMake 已装到 `C:\Program Files\CMake\bin`；新开的终端/重启 VS Code 后生效；脚本会自动回退到工程自带的 `third_party/cmake-3.31.6` |
| `Could not create named generator Visual Studio 18 2026` | 说明用的 CMake 版本过低（< 4.2），请确认走的是系统 CMake 4.4.3，而不是 `third_party/cmake-3.31.6` |

---

## 7. 源码文件速览

| 文件 | 作用 |
|---|---|
| `src/main.cpp` | 入口：解析参数 → 初始化日志/目录 → 启动 UI |
| `src/ui/app.cpp` | GLFW + OpenGL3 + ImGui、深色主题、中英文字体、DockSpace 布局、菜单栏、工具栏行、状态栏 |
| `src/ui/editor_state.cpp` | 编辑器状态：唯一持有 `Graph` + `UndoStack`；创建/删除/复制/粘贴/清空/示例工作流（一律先压快照） |
| `src/ui/node_canvas.cpp` | 节点画布：渲染 Graph（节点卡片/端口/连线/参数预览）、全部鼠标交互与三个右键菜单、视图导航 |
| `src/ui/node_library.cpp` | 节点库面板：按分类列出 9 个节点，点击在画布中心创建 |
| `src/ui/toolbar.cpp` | 工具栏：撤销/重做/复制/粘贴/删除选中/新建/示例 + 计数（无快捷键，纯鼠标） |
| `src/ui/property_panel.cpp` | 参数面板：9 类参数控件 + 校验提示 + 文件/目录选择 + 密码框 |
| `src/ui/theme.h` | 分类 / 端口类型 / 节点状态配色（设计 14.3 / 4.5 / 4.6） |
| `src/ui/console_panel.cpp` | Console 面板（级别过滤 + 搜索 + 虚拟化 + 自动滚动，设计文档 12.4） |
| `src/engine/graph.cpp` | 数据模型：Node/Port/Param/Edge/Graph、类型兼容矩阵、参数校验、增删与复制 |
| `src/engine/node_registry.cpp` | 9 个 MVP 节点的注册表（类型/端口/参数/默认值与范围） |
| `src/engine/undo_stack.cpp` | 快照式撤销/重做（深度 50） |
| `src/utils/paths.cpp` | `~/.brain-ai` 数据目录与各子路径 |
| `src/utils/log.cpp` | spdlog 双 sink（`app.log` 10MB×5 + 控制台）+ 内存环形缓冲（10000 条） |
| `src/utils/config.cpp` | toml++ 读写 `config.toml`（字段与设计文档 20.2 一致，缺省自动生成） |
| `src/utils/crypto.cpp` | SHA3-256（OpenSSL EVP，供 PoW 与自检使用） |
| `src/utils/file_dialog.cpp` | 原生文件/目录对话框（nativefiledialog-extended，`NFD::Init/Quit` 配对） |
| `tools/api_probe.cpp` | V-04/V-05/V-06 命令行验证工具 + `--graph-selftest` 图模型自检 |
| `tools/webview2_login.cpp` | V-03 WebView2 登录 + Cookie 提取（仅内存，脱敏打印） |
