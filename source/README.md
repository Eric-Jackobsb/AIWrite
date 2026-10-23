# AIwrite 源码工程（M1 骨架 + M2 节点系统 P1/P2）

> 设计文档：`../docs/ai_writer_nodes.md`　Action Plan：`../docs/actionPlan/`（进行中：M3–M6 + 补丁；已完成归档 → [../docs/Archive/actionPlan/](../docs/Archive/actionPlan/)）
> 验证报告：`../docs/Archive/M1_技术验证报告.md`　节点编辑器手册：`../docs/节点编辑器使用说明.md`
> 变更记录：`../docs/CHANGELOG.md`　文档总览：`../docs/DocMAP.md`
> 技术栈：C++20 / MSVC 14.50（VS 2026 Insiders）/ CMake **4.4.3**（系统安装）/ VS 2026 生成器 / vcpkg / Dear ImGui 1.90.7(docking) / imgui-node-editor v0.9.3 / nativefiledialog-extended 1.3.0（overlay port）

---

## 1. 目录结构

```
AIwrite/
├── .vscode/                    VS Code 构建/调试配置（launch.json / tasks.json / settings.json）
├── build/                      ★ 唯一的构建目录（Visual Studio 2026 生成器）
│   ├── aiwrite.slnx            可用 Visual Studio 2026 直接打开
│   └── bin/                    产物：aiwrite.exe / api_probe.exe / webview2_login.exe / ce_repro.exe（临时调试）
├── source/                     工程根（CMakeLists.txt 所在）
│   ├── ai/ engine/ nodes/ ui/ utils/ web/
│   │                             ★ **每个模块自带 src/ + include/**（2026-10-09 · FEA-L05 起）
│   │                             ├─ src/     .cpp 实现；`ai/src/<站点>/`（deepseek/、doubao/、kimi/）= **站点专属只在这里**
│   │                             └─ include/ 该模块的头文件（**拍平**；引用写裸名：`#include "log.h"`）
│   ├── app/src/main.cpp        程序入口（无头文件 ⇒ `app/` 只有 `src/`）
│   ├── tools/                  api_probe.cpp（V-04/05/06 + 图模型自检 + CN 上级节点自检）、composite_selftest.cpp（CN · 51 断言）、ce_repro.cpp（临时复现 ImGui 窗口栈断言 · CN 调试）、webview2_login.cpp（V-03）
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
`outputs/`、`workflows/`、`snapshots/`、`logs/app.log`、`config.toml`、`imgui.ini`、`node_editor.json`、
`assets/images/`（**统一资源目录** —— P7a-04 起的图片归档根）、`providers.d/`、`webview2/`（遗留）

---

## 2. 环境说明

| 组件 | 说明 |
|---|---|
| CMake | **系统安装**（`winget install Kitware.CMake` → `C:\Program Files\CMake\bin`，已在机器 PATH 中） |
| 生成器 | `Visual Studio 18 2026`（x64），由 CMake 自动准备 MSVC 环境，**不需要** Developer Prompt / vcvars64 |
| vcpkg | `C:\dev\vcpkg`（2024-04 版）；依赖与缓存都在 F: 盘的 `vcpkg-installed/`、`vcpkg-cache/` |
| 工程自带 CMake | `third_party/cmake-3.31.6`，**只用于 vcpkg 端口构建**（旧 ports 与 CMake 4.x 不兼容） |
| 依赖 | imgui 1.90.7(docking)、glfw3 3.4、cpp-httplib 0.15.3、openssl 3.3.1、spdlog 1.14.1、nlohmann-json 3.11.3、toml++ 3.4.0、stb、webview2 1.0.2277.86、**nativefiledialog-extended 1.3.0（overlay port，见下）** —— ⚠️ **`webview2` 计划移除**（M7 第三轮 `M7B`，见 §4.3），并**新增运行期依赖 Python 3.12 + Pydoll** |
| overlay port | vcpkg 快照缺 `nativefiledialog-extended`：`source/ports/nativefiledialog-extended/` + `source/vcpkg-configuration.json`（`overlay-ports: ["./ports"]`），随 `install-deps.cmd` 一起构建安装（实测约 17 秒），工程侧 `find_package(nfd CONFIG REQUIRED)` + `nfd::nfd` |

---

## 3. 构建（三选一）

**① VS Code 任务（推荐）**：`Ctrl+Shift+B` 执行默认任务 **build**；`F5` 会先自动构建再调试；
`Terminal → Run Task` 还有：`build (release)`、`build: 重新配置`、`vcpkg: 安装/更新依赖`、`run: aiwrite`、`verify: api_probe 自检`、`log: 查看 app.log`、`log: 查看最新构建日志`。

**② 命令行（无需任何开发者环境）**：

```powershell
cd F:\GameDao\Tools\AIwrite\source
.\build.ps1                      # Debug（默认）
.\build.ps1 -Config release      # RelWithDebInfo
.\build.ps1 -Reconfigure         # 清空 build 后重新配置（保留 build/logs/）
.\build.ps1 -Target aiwrite      # 只编译主程序
.\build.ps1 -LogName my.log      # 自定义日志文件名（仍落 build/logs/）
.\build.ps1 -NoLog               # 本次不写日志
```

**构建日志统一落 `build\logs\`（2026-10-09 起）**：`build.ps1` 把**整段输出**（`dev.ps1` 信息 + `cmake` / MSBuild 原生输出 + 成功/失败汇总）逐行写入

```
F:\GameDao\Tools\AIwrite\build\logs\build_<config>_<yyyyMMdd_HHmmss>.log      # UTF-8 无 BOM，一次构建一个文件
```

- **不再散落在仓库根**（2026-10-09 迁移：仓库根 15 个 `build_*.log` + `build\loop_build_b2.txt` + `.venv\m7b_smoke.log` 一次性移入 `build\logs\`）；`build\` 已被 `.gitignore` 整体忽略 ⇒ **日志不入库**。
- `cmake` 的原生输出**同时**留在命令 stdout ⇒ `& .\build.ps1 > x.log` 重定向与 VS Code 任务的 `$msCompile` 问题匹配器照常工作（`[build]` 汇总行经 `Write-Host`，只上面板不进 stdout —— 与改动前一致）。
- 看最新一份：VS Code 任务 **`log: 查看最新构建日志`**，或 `Get-ChildItem build\logs\*.log | Sort-Object LastWriteTime | Select-Object -Last 1`。
- 实现缘由（踩坑留痕）：PS 5.1 的 `Tee-Object` **没有 `-Encoding`** 且默认写 **UTF-16LE**，直接 `| Tee-Object -FilePath` 会把日志写成 UTF-16 ⇒ 脚本改用 `StreamWriter`（UTF-8 无 BOM）。

- **构建报 `error MSB4018` + `FileTracker`（任务 `GetOutOfDateItems` / `SetModuleDependencies`）** ⇒ **不是代码坏了**：VS 18 Insiders 的 FileTracker 在 `build\` **生成态陈旧**时会崩溃(`System.IndexOutOfRangeException: FileIsUnderNormalizedPath`)。**重跑无效**（`-p:TrackFileAccess=false` / `-nodeReuse:false` / 换 `%TEMP%` 实测均无效）⇒ 清空重建：`.\build.ps1 -Reconfigure`（保留 `build/logs/`）。物证：崩 `build\logs\pb224_build6.txt` ↔ 愈 `build\logs\pb224_build_reconf.txt`；登记 [../docs/loop_dev.md](../docs/loop_dev.md) §10.1 **陷阱 9**（`check_all.ps1` 命中该特征时也会直接在 `FAIL` 行提示这条命令）。

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
.\aiwrite.exe --open "F:\GameDao\Tools\AIwrite\source\workflows\testAllAI.json"
                               # 启动即打开指定工作流（默认用内置示例工作流；失败只 WARN 并回退）
                               # 用途：①命令行 / 文件关联直接打开；②性能复现固定成一条命令（见下）
```

**画布性能诊断行（2026-10-09 · 个位数 FPS 事故后新增）**：主循环每 2 秒往
`%USERPROFILE%\.brain-ai\logs\app.log` 写一行 `[诊断] 画布 …`，形如

```
[诊断] 画布 节点=85 连线=84 缩放=1.33 帧=120 FPS=60 宽度重算=0.0ms/帧 节点循环=4.3ms/帧 连线自检=0.2ms/帧
       | 纹理 近2.0s: 请求=1089 命中=1089 未命中=0 解码=0 上传=0 淘汰=0（每帧请求 9.0 次 / 解码 0.0 次）缓存 9/64 条 30.9MB
```

```powershell
# 复现 / 复核（GUI 子系统：用 Start-Process 起，读 app.log）
$log="$env:USERPROFILE\.brain-ai\logs\app.log"; $before=(Get-Item $log).Length
Start-Process .\aiwrite.exe -ArgumentList '--open','F:\GameDao\Tools\AIwrite\source\workflows\testAllAI.json'
Start-Sleep 16; Stop-Process -Name aiwrite -Force   # 随后读 $log 从 $before 起的增量
```

**判据**：稳态应是「`解码=0.0 次/帧`」+「`缓存 N/M` 的 N 稳定」+「`FPS≈60`」。
若出现 `[画布] 纹理缓存**整周期 0 命中**` WARN ⇒ **同屏图片卡片数 > 纹理缓存容量**（或图片文件每帧都在变），
必然每帧重解码 ⇒ 掉到个位数 FPS。处置：`ui/texture_cache.h` 的 `set_texture_capacity()` / `set_texture_byte_budget()`
（默认 64 条 / 256 MB，且本帧用过的条目不会被淘汰）。

M1 验证工具：

```powershell
.\aiwrite.exe                 # 主程序（GUI）：节点画布 + 参数 + 网页版登录
.\aiwrite.exe --console       # 同上，并额外分配控制台窗口显示日志
.\aiwrite.exe --login-selftest --timeout 30
                              # 网页版登录自检（离屏）：退出码 0=通过 / 1=失败 / 2=超时
.\aiwrite.exe --web-probe     # 网页版协议探测（脱敏）：userToken / PoW 挑战 / 端点校验
.\aiwrite.exe --web-chat "用一句话介绍你自己"
                              # 网页版端到端生成：PoW（页面内官方 worker）→ completion → SSE
.\aiwrite.exe --run-selftest  # 执行自检：示例工作流跑到底，打印各节点状态与统计（不开窗口）
                              # ⚠️ official 模式**依赖本机凭据**（`~/.brain-ai/config.toml` 的 `api_key_ref =
                              #    brain-ai/deepseek`，或环境变量 DEEPSEEK_API_KEY）—— 换机 / 重装后未重配
                              #    ⇒ n3 报「缺少 API Key」→ n4 跳过 → exit 1。本机 2026-10-03 实测即为此情形
                              #    （**环境依赖，非代码问题**；改用下面 `--web` 变体可绕开）
.\aiwrite.exe --run-selftest --web
                              # 同上，但 LLMGenerate 走网页版真实生成（需已登录过一次；实测 5/5 ≈10s）
.\aiwrite.exe --upload-selftest [--provider <id>] [--image <路径>] [--timeout <秒>]
                              # 站点上传模板自检（M8-14）：离线三组（图片准备 19 + 上传证据 32 + 站点上传模板 21
                              # ⇒ 实测 **72/0 · exit 0**，见 docs/CHANGELOG.md）；带 --provider 且已登录时追加**真机一次**
                              # （0=通过 / 1=失败 / 2=超时）；未给 --image 时只检查不上传
                              # ⚠️ 离线三组共用 `%TEMP%\aiwrite-upload\selftest` ⇒ **不要并发跑多个实例**（会互相删目录 → 假失败）
                              # ⚠️ aiwrite.exe 为 GUI 子系统：`>` 重定向捕不到 stdout，须
                              #    Start-Process -RedirectStandardOutput（见 ../AI_README.md §3）
.\aiwrite.exe --pydoll-selftest
                              # 【新通道 · step 6】起 Python 守护进程 → hello → ready{proto=4} → shutdown（**不开浏览器**）
                              # 退出码 0=通过 / 1=失败 / 2=依赖问题（无 Python / 无浏览器）；只验通道，不影响生产路径
.\aiwrite.exe --pydoll-login deepseek-web --timeout 300
                              # 【新通道 · step 6】起**独立** Edge 窗口，人工登录站点（不代填密码、不绕验证）
                              # 判据 = cookie_names 命中则关窗；未知 id → 退出码 2 + 可操作提示、**不开窗**
.\aiwrite.exe --pydoll-script-selftest
                              # 【新通道 · step 8】跨语言端到端（词表 v4）：open_tab（**本地 file:/// 夹具**）
                              # + run_script 读 DOM / 类型保真 / 空脚本报错 + shutdown ⇒ 实测 4/0 · exit 0
                              # **零外网零登录**；验的是 `--web-dom-dump` 换代后要依赖的「页面内执行 JS」能力
.\aiwrite.exe --pydoll-chat-selftest
                              # 【新通道 · step 15】**内容返回**端到端（词表 v4 · `M7B-54`/`M7B-56`）：
                              # open_tab（本地夹具：输入框 + 发送按钮 + 回答容器）+ **send_prompt**（真打字 + 点击发送）
                              # + read_answer（正文与夹具渲染**字节一致**）+ I18 拦截 / attach_unsupported 可操作拒绝
                              # ⇒ 实测 **6/0 · exit 0**；**零外网零登录**；验的是**生产内容路径**（注入 / 发送 / 取回答）
.\aiwrite.exe --web-dom-dump --provider kimi-web
                              # 【step 9 换代】选择器候选枚举（只读）：**已走新通道（Pydoll）**——
                              # 自己起常驻会话 → open_tab 到站点 → read DOM 候选 + 建议选择器；跑完自动收尾
.\aiwrite.exe --web-adapter-selftest --provider kimi-web
                              # 【step 9 换代】选择器**命中数**诊断（只读）：**已走新通道（Pydoll）**
.\api_probe.exe --selftest                  # V-04 HTTP + V-05 SHA3 自检
.\api_probe.exe --graph-selftest            # 图模型/注册表/撤销栈/序列化 自检（110 项断言，无需网络）
.\api_probe.exe --exec-selftest             # 拓扑 + 加载/运行前校验 + 执行器 + 图片解码 自检（464 项断言，无需网络；2026-10-10 实测）
.\api_probe.exe --image-decode D:\a.webp   # 图片诊断：内容嗅探 / MIME / 解码(stb|wic) / 尺寸 / WIC 能力（M7）
.\api_probe.exe --sha3 "abc"                # 单次 SHA3-256
$env:DEEPSEEK_API_KEY="sk-..." ; .\api_probe.exe --chat "你好"   # V-06（需 Key）
.\webview2_login.exe --selftest --timeout 30   # V-03 自检：离屏跑「导航 → 提取 Cookie」，退出码 0=PASS/1=FAIL/2=超时
.\webview2_login.exe                        # V-03 交互：打开站点手动登录（Ctrl+Alt+C 提取 Cookie）
.\webview2_login.exe --ephemeral            # 使用临时 profile（退出丢弃登录态）
```

> ⚠️ ~~**上述 `webview2_login` 三行计划退场（M7 第三轮 `M7B`，2026-09-28 · 见 §4.3）**~~ 📦 **已作废（2026-10-05 · 方案 E）**：**WebView2 不退场**（文字生成回归），`webview2_login` **保留** —— 见 [../docs/actionPlan/M8.md](../docs/actionPlan/M8.md)：WebView2 退场后由
> **`pydoll_login --provider <id>`** 取代（退出码三档语义不变）；`webview2_login.cpp` 从 `tools/` 删除。
> 计划全文见 [../docs/actionPlan/M7B.md](../docs/Archive/actionPlan/M7B.md) §8.4 / `M7B-30`。

> 面板可见性由 `~/.brain-ai/config.toml` 的 `[ui]` 段控制（`show_node_library` / `show_property_panel` /
> `show_console`）；**默认显示节点库与参数面板**（`P7a-12`，2026-09-28 起；**显式写了 `false` 的老配置仍保持隐藏** —— `P7a-13`，
> 按「键是否存在」判定）；在菜单「视图」里勾选后会自动写回配置，下次启动生效。
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

### 4.0 M6（非技术用户可安装化）：设置面板 / 诊断 / 依赖自检 / 一键自测 / 打包（2026-10-10）

```powershell
# —— 主程序内的新入口（GUI，纯鼠标）——
#   菜单「文件 → 设置…」         = 设置面板（`config.toml` 可调项；「保存」才落盘，原文件自动备份 `.bak`）
#   菜单「帮助 → 复制诊断信息」  = 一键把环境报告复制到剪贴板（版本/系统/依赖/路径/配置/站点表；**不含密钥**）
#   菜单「帮助 → 首次使用引导」  = 重开三步引导（首次启动自动弹一次，可跳过；走过就写 `[ui] first_run_done`）
#   菜单「帮助 → 打开日志目录 / 打开数据目录」

# —— 诊断与依赖（控制台工具：stdout 可直接重定向）——
.\build\bin\api_probe.exe --diag-dump        # 打印诊断报告（8 分节）
.\build\bin\api_probe.exe --diag-assert      # 诊断报告离线断言（单行 PASS/FAIL；exit 0/1）
.\build\bin\api_probe.exe --dep-check        # 依赖自检：Edge WebView2 运行时（缺失时打印安装指引；exit 0/1）
.\build\bin\api_probe.exe --settings-assert  # 设置面板校验纯函数断言（默认通过 + 越界 11 例全拦截；exit 0/1）
.\build\bin\api_probe.exe --site-assert      # 自建站点闭环断言（PB2-24：模板纯函数 47 项 + 真实写盘→重载→进表/进下拉/进枚举→新节点校验/旧实例刷新/清理）
.\build\bin\api_probe.exe --autosave-assert  # 自动保存/崩溃恢复断言（PC-01/02：原子写 28 项场景；全程在系统临时目录内，不碰用户快照）
                                             # ⚠ 会**临时**写并删除 ~\.brain-ai\providers.d\aiwrite-selftest-site.json（自清理）

# —— 一键跑完所有离线自检（构建 + 离线自检/夹具 + GUI 自检 + 站点台账 + 依赖单向 + 文档断链 + 打包 ⇒ `PB2-24` = 13 项、`PC-01/02` = 14 项、`M_patchAB` 批 5 = 15 项、**`ARCH-A4` = 现行 20 项**）——
powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_all.ps1
#   参数：-SkipBuild / -SkipGui / -SkipLoop / -OnlyBuild；打印汇总表 + exit 0/1
#   物证：build\logs\check_all_<时间戳>.log（各项明细与 build 输出同目录）
#   构建时也可顺带跑一遍（`PM-03`）：`cd source; .\build.ps1 -SelfCheck`（编译成功后调 check_all，失败指向 check_all 日志）

# —— 依赖单向自检（`ARCH-A4` 收口 · 判据 = 能力层不含 ui/app 头 · 已入 `check_all` 第 ⑥ 项）——
powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_includes.ps1
#   R1：engine/nodes/ai/web/utils 禁止 include 归属 ui/app 的头 ｜ R2：ui 禁止 include app 的头
#   违规 ⇒ 逐条打印 `文件:行 #include "x" ⇒ 归属层` + exit 1；跨层同名头 = 归属不明同样判失败
#   实测（2026-10-23）：checked files 201 / include lines 656 / violations 0

# —— 文档断链自检（`DBG-L01` 收口 · 判据 = **全库 0** · 已入 `check_all` 第 ⑦ 项）——
python source\tools\check_links.py            # checked 1864 / broken 0（含仓库根 *.md；剔除代码块与行内代码；退出码 0/1）

# —— 打包「一键安装包」（Inno Setup · `M6-08`）——
powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\package.ps1 [-Version 0.2.0] [-SkipBuild] [-SkipInstaller]
#   ① 构建 → ② 收集 payload `build\dist\payload`（**Python 摘钩断言**：payload 内 `*.py` 必须为 0）
#   → ③ 调 `ISCC.exe` 编译 `packaging\aiwrite.iss` → 产出 `build\dist\AIwrite-Setup-<版本>.exe`
#   退出码：0 = payload + 安装包都成功 ｜ 2 = 只完成 payload（未装 ISCC，已打印获取方式）｜ 1 = 失败
#   ⚠ 本机 2026-10-10 **未编译验证**（无 ISCC 且 winget 无网络 `0x80072efd`）；payload 阶段与
#     `.iss ↔ payload` 一致性锁（4 条 Source 通配逐条核对）已实测 —— 见 `docs/CHANGELOG.md` `[Unreleased]`
#   ⚠ 附带 WebView2 Bootstrapper（`PQ-02`）为**可选件**：把 `MicrosoftEdgeWebview2Setup.exe` 放进
#     `packaging\webview2\` 即随包；没有则该行 `skipifsourcedoesntexist` 静默跳过（见该目录 README）
#   📦 完整打包与分发说明（口径 / 安装包内容 / 干净机器冒烟）：见 [../packaging/PACKAGING.md](../packaging/PACKAGING.md)

# —— M6 新增配置键 / 新接线项 ——
#   [ui] first_run_done            = false  # 首启引导是否已看过（**缺键 = false** ⇒ 老用户也会看一次，可跳过）
#   [output] auto_open_on_complete = false  # ✅ **已接线**：归档成功后用资源管理器打开本次归档目录
#   [advanced] log_ttl_days        = 30     # ✅ **已接线**：启动清理 logs/ 下超期日志（app.log / workflow.log 永不删）
#   ⇒ `unwired_config_fields()` 由 6 项降为 **4 项**（余 `ui.grid_size` / `timeout.*` / `error.*` / `providers.deepseek.*`）
```

### 4.1 网页版登录与多站点（2026-09-26 已支持 · ⚠️ **当前仍走 WebView2**，Pydoll 版见 §4.3）

- **站点身份按「生效条目」**：登录页 / 窗口标题 / 探测路径 / Cookie 名全部取自条目的 `web.*`
  （`web/include/webview_host.h` 的 `interactive_login_request(site,id)` 等）；无参重载 = 旧常量（逐字一致，兼容保留）。
  ✅ **已落地（v9 · `D-22②` + `D-26`）**：**站点不再回落** —— 条目不是网页版条目（或表外 id / `web.login_url` 为空）时，站点区给**错误块 + 三条引导**（开窗按钮禁用）、运行前校验提示「**本次运行必定失败**」（**不阻断**）、运行期直接 `NodeError`；适配器未实现（如 `dom`）→ 运行期**明确报错**（可登录 / 可探测，生成待 L3）。不变量 `I14`。
- **加一个网页版站点**：把 `{"schema_version":1,"providers":[ … ]}` 写进 `~/.brain-ai/providers.d/<id>.json` 并**重启**；`web` 段用**嵌套** `send{kind,value}` / `done_when{kind,selector}` —— 实测可用样例见 [../docs/actionPlan/M_patchAB_rest.md](../docs/actionPlan/M_patchAB_rest.md) **附录 D**（现行版本）。
  * **配置表重新加载入口（2026-10-10 · `M6`）**：设置面板 → 「提供商与站点表」→ 「**重新加载配置表**」会调 `ai::reload_provider_specs()`（`utils`/`ai` 侧只加载表，参数面板每帧按当前表渲染）⇒ **不必重启**即可看到新条目计数与摘要。
  * ⚠ **仍未收口**（`PB2-24`：自建站点**闭环** = 新增站点 → 节点/参数即时可用 → 真机逐站验证，`RM-D4` 必做项）—— 本批只落地了「手动 reload 入口」，**闭环未验证**。
- **内置站点入口（v10 / `PB2-26`）**：配置表已内置 **10 个 AI 的网页版登录入口**（Kimi / Qwen 国际站 / 智谱清言 / 豆包 / 腾讯元宝 / 文心一言 / 讯飞星火 / ChatGPT / Claude / Gemini）+ 原有 `deepseek-web`；条目为**登录型**（只配 `login_url` + `window_title`）：**能登录 / 能探测**，**生成**需先补齐选择器（DOM 执行器 L3 v11 **已就绪**；选择器由 L4 `PB2-29` 逐站实测回填）→ 未填时点运行会**明确报错**（不静默、不回落 DeepSeek）。完整清单见 [../docs/actionPlan/M_patchAB_rest.md](../docs/actionPlan/M_patchAB_rest.md) **附录 E**（现行版本）。
- **L3（v11）：DOM 适配器已接线**（`ai/src/dom_web_client.cpp` + `ai/include/dom_web_client.h`）—— 条目补齐 **`web.input_selector` / `web.send` / `web.answer_selector`**（+ 建议 `web.done_when`）**重启即真正生成**（写入提示词 → 自动发送 → 轮询取答案；到上限/超时**如实返回已取文本 + 警告**）；不要求内存 `userToken`（登录态在浏览器 profile）；选择器不会写就用 **`--web-adapter-selftest --provider <id>`**（只读命中数 + 修复建议）。站点 = 纯数据，**无需改 C++**。
- **L4（v12 · 文档先行，代码待 `B2-e`）：登录层去 DeepSeek 化 + 站点数据落地 + 自助闭环** —— ①**登录态判据站点无关**（`web_session_state()` 纯函数：按该 origin 的 Cookie / `cookie_names` 判，**不看** `userToken`；`userToken` 行仅当条目配了 `token_expr` 才显示）②**探测「不适用」语义**（条目未配 `probe_paths` / `token_expr` / `endpoints` 时不再注入 DeepSeek 端点 → 只读诊断分支；`deepseek-web` 与无参路径**逐字不变**）③**内置站点选择器逐站实测回填**（`--web-adapter-selftest --provider <id>` → 填条目 → `verified:true`）④**自建站点 UI 闭环**（新建条目 / **重新加载配置表** / 测试选择器；`PB2-24` 并入）。计划：**现行** [../docs/actionPlan/M_patchAB_rest.md](../docs/actionPlan/M_patchAB_rest.md) §2.2 / §3.3–§3.5（承接 `PB-07` / `PB2-25` / `PB2-29` / `PB2-30②` / `PB2-24`）；历史 §9.7 见归档 [../docs/Archive/actionPlan/M_patchB.md](../docs/Archive/actionPlan/M_patchB.md)。

- **「模式」下拉恒两项**（`engine/src/provider_resolve.cpp` 的 `provider_mode_options`）：`official` / `web` **始终可选**
  （决策 `D-21`：**网页版与官方 API 同等优先级**，**不按条目 `kind` 裁剪**）；切换「提供商」只带出**建议值**
  （网页版条目建议 `web`、官方条目建议 `official`），不一致时给橙色**提示**：非网页版条目 + `web` → 用内置默认站点；
  网页版条目 + `official` → 该条目没有官方 API 通道（按表内 `api_base` 解析，缺失则明确报错）。
  > ✅ **已修复（2026-09-26 · 任务 `PB2-20`）**：候选**恒两项**、`kind` 与 `mode` 不一致**只提示不改写**（守 `I13`）；
  > 参数面板 / 运行前校验改按 `resolve_display_provider()`（**该节点自身条目**）解析 —— 站点条目 / 登录页 / 提示不再误回落 DeepSeek（`VB2-18②③`）。
- **会话按站点键控**（`web/include/session_store.h`，`site_key_of()` = origin）：`std::map<站点, Session>`，
  旧无键 API 保留为「默认槽」薄封装 → `--login-selftest` / `--web-probe` / `--web-chat` / `--web-session-selftest` 行为不变。
- **窗口串行复用**：同一时刻一个登录窗口；切站点时先关旧窗再按新站点开窗（页面内 PoW 求解依赖该站点页面）。
- **注销按站点**（`web::logout_site()`）：清该站点内存会话 + 删该 origin 的 Cookie + 清同源 `localStorage`；
  「删除整个 profile（所有站点）」在参数面板「高级」里，需二次确认。
- 内置配置表 `assets/providers.json` 共 **20 条**（official 9 / web 11），其中 `kind=web` 为 `deepseek-web`（**唯一已填选择器、可生成**）+ **10 条登录型站点**（v10 / `PB2-26`；**L4 `PB2-29` 逐站补选择器**）
  （Kimi 等站点已作为**登录型条目**内置，不再是「仅模板」）；要接入**新**站点，把站点 JSON 放进
  `~/.brain-ai/providers.d/`（照 `_example_web_dom` 抄）。
- **`config.toml` 多 provider 实例参数**（PB2-06）：`[providers.<id>]` 可多节（`deepseek` 旧节自动迁移，幂等）；
  保存前自动备份 `config.toml.bak`，写入采用 `.tmp` → 原子替换。**厂商元数据仍以配置表（JSON）为准**。
- 计划与验收：`PB2-17`/`PB2-18`/`PB2-19`/`PB2-06`/`PB2-20`（`D-21` 修订）、`I11`/`I12`/`I13`、`AB2-13`/`AB2-14`/`AB2-15`、`VB2-16`/`VB2-17`/`VB2-18`、`R14`/`R15`/`R16`，
  见归档 [../docs/Archive/actionPlan/M_patchB.md](../docs/Archive/actionPlan/M_patchB.md)（§9「L1 收口」实测基线；**L4 立项见 §9.7**）与**现行承接** [../docs/actionPlan/M_patchAB_rest.md](../docs/actionPlan/M_patchAB_rest.md)（§3 `B2` 残项）与
  [../docs/网页版协议实测记录.md](../docs/Archive/网页版协议实测记录.md) §7。

### 4.2 图片诊断与「图片解码冻结区」（M7 · 2026-09-27）

**诊断命令（只读、不联网）**：

```powershell
.\api_probe.exe --image-decode D:\a.png      # 也支持 .webp / .jpg / .tif / .heic
```

输出 8 行结论：内容嗅探（**不看扩展名**）→ 内容 MIME（发给模型用）→ 扩展名是否与内容一致 →
文件头十六进制 → WIC 能力 → 本地解码（解码器 `stb` / `wic` + 尺寸）→ 只读尺寸 → 结论。
退出码 `0` = 本地可预览；`2` = 不可预览（附可操作提示）。示例（真实 WebP 却叫 `.png`）：

```
内容嗅探    : WebP（扩展名 .png；按扩展名的 MIME image/png）
内容 MIME   : image/webp（发给模型时使用）
扩展名一致  : 否 —— 扩展名与实际格式不一致
本地解码    : OK（解码器 wic，2048×2048，RGBA8 16777216 字节）
```

**两条读取路径**（互不影响，排查时别混为一谈）：

| 路径 | 实现 | 覆盖 |
|---|---|---|
| 发给模型（多模态） | `ai/src/deepseek/deepseek_official_provider.cpp::image_mime_from_path`（**内容嗅探优先**、扩展名回退） | 只读文件字节 + 判类型，本地不解码；失败与否取决于后端 |
| 本地预览 / 尺寸 | `utils/src/image_decode.cpp` | stb：PNG/JPEG/BMP/GIF/TGA/PSD/HDR/PNM；WIC：WebP/TIFF/ICO/JXR/HEIF/AVIF（需系统装了对应「图像扩展」） |

> ⇒ **本地预览失败 ≠ 模型读不到**。错误文案里会明确写这一点（「图片理解」按内容识别格式后发给模型）。

**冻结区（FROZEN · 不变量 `I17`）** —— `utils/src/image_decode.cpp` 的 WIC 相关代码**冻结**，改动前必须遵守：

1. **绝不把「合成 / 截断 / 未识别格式」的数据交给 WIC**。能力探测**只允许**用「解码器元数据枚举」
   （`IWICImagingFactory::CreateComponentEnumerator` + `IEnumUnknown` + `IWICComponentInfo::GetFriendlyName`）；
   **不得**用伪造文件头去调 `CreateDecoderFromStream` / `CreateDecoderFromFilename` 来"试"能力。
2. **解码分派固定**：嗅探结果 ∈ {PNG, JPEG, BMP, GIF, PSD, HDR, PNM, 未知} → **只走 stb**（stb 自己按内容嗅探）；
   ∈ {WebP, TIFF, ICO, JXR, HEIF, AVIF} → **先 WIC、失败再 stb**。**不得**给 stb 覆盖的格式"兜底"送 WIC。
3. 失败文案（`decodeErrorMessage`）**不得依赖**能力探测结果（探测细节只能进括号说明），
   否则离线断言会随机器变化而红。

**事故记录（2026-09-27 · 必须知道）**：M7-04 初版用「30 字节合成 WebP 头」探测系统解码器。
本机装有 `Microsoft.WebpImageExtension 1.2.31.0` → WIC 的 WebP 解码器**接受**该假头后去解析比特流 →
CRT assert / abort：**退出码 3（`-2147483645` / `0x80000003` STATUS_BREAKPOINT）**；
`api_probe --exec-selftest` 与「参数面板渲染 WebP 图片」两条路径都会踩到。
定位过程与修复见 §6.1 与 [../docs/Archive/actionPlan/M7.md](../docs/Archive/actionPlan/M7.md) §1。

**计划中的不变量（`I18` / `I19` · 2026-09-27 会议立项 · 属 M7 第二轮 P7，见 [../docs/Archive/actionPlan/M7.md](../docs/Archive/actionPlan/M7.md) §15）**
> ⚠️ 下面两条**尚未实现**（P7-b 为「先验证后实现」），先登记以免后续改动无意破坏设计意图。

1. **`I18`（网页版图片上传证据）**：网页版图片流程中，**没有「上传完成证据」时不得自动发送**提示词
   （证据 = 页面状态 + 网络回执**双证据**，见 `P7b-05`/`P7b-11`）。仅当用户在**本次运行**里
   显式选择「不含图片继续」才允许降级，且输出面板 / Console **必须记录「本次未含图片」**
   （沿用「不假装成功」的既有原则）。上传失败必须给**可操作**原因（四类可区分），不得静默。
2. **`I19`（`image` 端口语义兼容）**：`image` 端口从「单值路径字符串」升级为
   **variadic 多值 + 资源引用**（`P7-a`）时，**必须向后兼容**旧工作流文件：
   旧绝对路径可加载、可运行，并给**迁移提示**（可一键迁移到统一资源目录）。

**统一资源目录（✅ P7a-04 已落地，2026-09-28）**：`~/.brain-ai/assets/images/<摘要>.<ext>`（**内容寻址**，天然去重；
摘要 = **SHA3-256**，复用 `utils/crypto.h`）；工作流里存**令牌** `aiwrite-asset:<摘要>` 而非绝对路径（`utils/asset_store.*`）。
**向后兼容**（不变量 `I19`）：旧绝对路径照旧可加载 / 可运行 → Console 给迁移提示，参数面板「**迁移到资源目录**」一键归档
（失败项保留原值，**不静默改写用户文件**）。细则见 [../docs/Archive/actionPlan/M7.md](../docs/Archive/actionPlan/M7.md) §11.1（`P7a-04`~`P7a-07`）。

---

### 4.3 「引擎唯一化」与 `I2` 解冻 —— 📦 **已归档（2026-10-05 · 路线退场）**

> 📦 **本节已归档（2026-10-05）**：`M7B` 的 Pydoll 路线（WebView2 退场 → 全部网页版走 Pydoll · Python 守护进程 + 命名管道）已被 **方案 E** 取代 ⇒ **文字生成回退 WebView2**、**图片上传改走「站点上传模板」**（进程内 CDP + 双证据 · 原「C++ native HTTP」📦 2026-10-06 改判）、**Python / 命名管道 / 守护进程整体退场**（Phase 3 📦 **2026-10-11 改判：栈休眠保留 · 不删**）。本节原记「M7 第三轮 `M7B` · 2026-09-28 立项 · 批 3 step 1~14 已落地 / 词表 v4 / 全部命令已实现」。
> **现行计划** → [../docs/actionPlan/M8.md](../docs/actionPlan/M8.md)　**归档全文** → [../docs/Archive/actionPlan/M7B.md](../docs/Archive/actionPlan/M7B.md)　**摘录件** → [../docs/Archive/actionPlan/M7_P7b_pydollRoute.md](../docs/Archive/actionPlan/M7_P7b_pydollRoute.md)

> 本节是**计划登记**。**批 1~3 的 step 1~14 已落地**（见下方「运行时通道归属」）：诊断工具（step 9 · `M7B-18`）、
> **生产文字链路 `dom_chat`**（step 10 · `M7B-20`）、**会话族**（step 11 · `M7B-20b`：登录窗口 / 收尾 /
> 按站点注销 / tab 判定）与 **内容返回正式化**（step 14 · `M7B-54`~`M7B-56`：`send_prompt` / `read_answer` /
> `upload_image` · **词表 v4**）**都已切到新通道**；仍走 WebView2 的只剩 **`builtin:deepseek`
> 协议栈族**（随批 5 退役）。计划全文见 [../docs/actionPlan/M7B.md](../docs/Archive/actionPlan/M7B.md)。

#### ⚠️ 当前运行时通道归属（**2026-10-05 实测** · 防误判：**文字生成 + 登录窗口都已走 Pydoll**）

> **网页版「文字生成」（`adapter=dom`）与会话族（登录窗口 / 收尾 / 按站点注销 / tab 判定）都已走新通道
> Pydoll**（step 10 · `M7B-20`；step 11 · `M7B-20b` · **词表 v3**）；仍走 WebView2 的只剩
> **`builtin:deepseek` 协议栈族**（`--web-probe` / `--web-chat` / `--login-selftest` / `--run-selftest --web` /
> 面板「探测网页版协议（dev）」）—— **随协议栈在批 5 退役** ⇒ **在 GUI 里偶然看到内嵌 WebView2
> （协议探测）是符合设计的状态，不是缺陷**。
> 逐入口实测归属表 + 分辨方法见 [../docs/actionPlan/M7B.md](../docs/Archive/actionPlan/M7B.md) §1.4。

| 入口 / 命令 | 现在**实际走** | 备注 |
|---|---|---|
| 面板「打开登录窗口（**Pydoll**）」/「关闭浏览器会话」/「按站点注销」 | **Pydoll** ⚠️ **step 11 已从 WebView2 切走** | `ui/src/property_panel.cpp`：`start_web_task(Login/Logout/Shutdown)` → `channel::login_site` / `logout_site` / `shutdown_session`（**后台线程 + 缓存状态**）·`Q4`：阻塞函数不得在 UI 线程调 |
| ~~登录型节点 `ensure_session`~~ | **不切（订正）** | `nodes/src/local_nodes.cpp:545` 是 **`builtin:deepseek` → `web_chat()`** 的前置（要 `userToken` + PoW）⇒ **随协议栈在批 5 删除**（`M7B-42` 白名单收敛 `{dom}`）；**给它切新通道是白做**（2026-10-04 订正，原写「需单独切」有误） |
| 网页版**文字生成**（`adapter=dom`） | **Pydoll** ⚠️ **step 10 切走 · step 15 内容路径改走 v4 命令**（`M7B-20` / `M7B-56`） | `ai/src/dom_web_client.cpp`：`channel::ensure_session` + **`channel::send_prompt`（真打字）+ `channel::read_answer`（取正文）** ⇒ **生产路径已无 `run_script`**（DOM 脚本常量降级为诊断资产）；**已断** `webview_host.h` include；会话请求 = `visible_login_request`（**有头** · step 11）；会话收尾在 `app/src/main.cpp` |
| `--login-selftest` / `--web-probe` / `--web-chat` / `--run-selftest --web` | **WebView2**（**只有这一族仍是旧通道**） | 同上（协议栈族，批 5 退役） |
| **`--pydoll-selftest`**（新 · step 6） | **Pydoll** | 起守护进程 → `hello` → `ready{proto=4}` → `shutdown`；**不开浏览器**；exit 0 |
| **`--pydoll-login <id>`**（新 · step 6） | **Pydoll** | **独立** Edge 窗口 + profile `~/.brain-ai/pydoll-profile/`（WebView2 则是 `~/.brain-ai/webview2/`） |
| **`--pydoll-script-selftest`**（新 · step 8） | **Pydoll** | **跨语言端到端**（词表 `run_script` + 本地 `file:///` 夹具）⇒ 实测 **4 / 0 · exit 0**（`proto=4`）；`run_script` 已用于诊断（`--web-dom-dump` 换代的地基） |
| **`--pydoll-chat-selftest`**（新 · step 15） | **Pydoll** | **内容返回端到端**（`send_prompt` 真打字 + `read_answer` 取正文 + `I18` 拦截 + `attach_unsupported`）/ 本地 `file:///` 夹具 ⇒ 实测 **6 / 0 · exit 0**（`proto=4`）；验的是**生产内容路径** |
| **`--web-dom-dump` / `--web-adapter-selftest`**（**step 9 换代**） | **Pydoll** ⚠️ **已从 WebView2 切走** | `ai/src/dom_web_client.cpp` 的两个诊断函数 → `web::channel::ensure_session` + `web::channel::run_script`（自起常驻会话 · RAII 收尾）；真站点 `kimi-web` 实测 **exit 0**（读到 `div.chat-input-editor`） |

**怎么分辨（Pydoll 底层也是 Edge，别比窗口长相）**：比 **profile 目录**（`pydoll-profile` vs `webview2`）、
**是否独立任务栏窗口**、**是否有 `~/.brain-ai/logs/pydoll_channel_daemon.log`**。
**谁在跑**：**独立窗口 + `pydoll-profile` = 新通道**（文字生成 / 面板登录 / 注销）；
**内嵌在主窗口里的小窗 = 旧通道**（只剩「探测网页版协议（dev）」）。

**切换进度（实测 · 2026-10-05 · 词表 v4 · 命令全部实现 · **生产内容路径已切换（step 15）**）**：Python 侧
`daemon.py` 已实现 **全部 10 条命令** —— `hello` / `shutdown` / `open_tab` / `login_state` / `run_script`（v2）/
`logout_site` / `current_tab`（v3）/ **`upload_image` / `send_prompt` / `read_answer`（v4）**；
`not_implemented` 码**保留但当前无使用点**。**内容返回**（注入 → 发送 → 取回答）= `send_prompt`
（pydoll **真打字** + 按 `send` 触发）+ `read_answer`（轮询 + **显式截断** `truncated`）；站点选择器 /
发送方式**由条目下发**（`I14`），`run_script` **降级为诊断专用**。C++ 侧 `login_site` / `logout_site` /
`current_tab_site` / `tab_on_site` 与 **`upload_image` / `send_prompt` / `read_answer`** 均已落地；
**`dom_chat`（网页版文字生成）生产路径已改走 `send_prompt` + `read_answer`**（`M7B-56` · 生产再无
`run_script`），并新增 CLI **`--pydoll-chat-selftest`（6 / 0 · exit 0）**。
⇒ **文字链路（`dom_chat`）与会话族（登录窗口 / 收尾 / 按站点注销 / tab 判定）都已切换**
（step 10 · `M7B-20`；step 11 · `M7B-20b`；step 14/15 · `M7B-54`~`M7B-56` · 全回归绿）；余下
**`M7B-21`**（CDP 流式增量）· **`M7B-24`**（多候选选择器；当前下发**单元素**）· **`P7b-11`**（上传网络回执
双证据）· **`P7b-16`**（网页版图片理解解禁）· **批 4 站点选择器回填**。

**冻结区变更（解冻规则）**：

| 项 | 内容 |
|---|---|
| 解冻规则 | **`I2`** —— 原文「`--web-chat` / `--web-probe` / `--web-session-selftest` **行为不变**」（归档 `M_patchB.md:294`），实际被用来护住「无参 CLI = 内置 DeepSeek 常量」的一整套兼容装置 |
| 解冻原因 | 宿主 **WebView2 退场**（嵌入控件易被站点识别为非真实浏览器，`MB-D0-2`）；且站点「**不回落**」（`D-22②` / `I14`）此前对 CLI 路径存在豁免，需**贯彻到底**（`MB-D0-6`） |
| 替代物 | **`I20`（CLI 契约）**：命令名 / 参数形式 / **退出码语义**在 `--help` + 文档 + 断言三处一致；缺 `--provider` → **列候选 + 退出码 2**（不得静默取第一个）；`--provider auto` 为**显式保留字**；自检命令不得残留状态 |
| 回归基线数字 | `--exec-selftest` **251/0 →（先降后升）→ 实测回填**（删 `VB2-17` 前半 + `VB2-25①③④`；新增 `VB2-28`~`VB2-36`）；路径见 [../docs/actionPlan/M7B.md](../docs/Archive/actionPlan/M7B.md) §9.3。**现状（2026-10-04 · step 11）= 332 / 0**（批 1–4 **只升不降**：328 → 332 = +`VB2-43①~④`；删除类动作集中批 5） |

**新增不变量（拟进冻结区 · 与 `I18` / `I19` 同批登记）**：

1. **`I21`（不静默降级）**：Python / 浏览器 / 守护进程 / 登录态任一缺失 → **报错 + 可操作引导 + 退出码 1/2**；
   **绝不换通道、绝不换身份、绝不假装成功**（明确禁止「回落 WebView2」与「改用另一套登录态」两种行为）。
2. **`I22`（流式降级必须显式）**：CDP 增量不可用 → 退回 DOM 轮询，但 UI 与日志**必须标注「非流式（轮询）」**。
3. **`I23`（登录态持久化 · 2026-09-28 前置验证后新增）**：退出必须**先 `Browser.close` 并等进程退出**
   （超时才强杀），**禁止「close 后立刻 kill」**；持久 Cookie 的存续不得依赖单一机制（干净退出 + **DPAPI 加密快照**双保险）；
   快照**不得明文落盘 / 不得外传**；恢复失败必须**显式提示**。配套决议 **`MB-D0-8`**（四层：L1 干净退出 /
   L2 加密快照 / L3 启动自愈 / L4 异常退出可见），断言 `VB2-38` / `VB2-39`，见
   [../docs/actionPlan/M7B.md](../docs/Archive/actionPlan/M7B.md) §2 / §5 / §9。

**将退役 / 改写的实现（计划 · 见 `M7B-30` / `M7B-31` / `M7B-37`）**：

| 对象 | 处置 |
|---|---|
| `web/src/webview_host.cpp` + `web/include/webview_host.h`（58 + 6 处） | **删除** → `web/src/pydoll_channel.cpp` / `web/include/pydoll_channel.h` + `web/src/pipe_client.cpp` / `web/include/pipe_client.h` |
| `tools/webview2_login.cpp`（60 处） | **删除** → `pydoll_login`（V-03 记录标注「已被 `M7B` 取代」） |
| `ai/src/web_pow.cpp` · `ai/include/web_pow.h`、`ai/src/deepseek/deepseek_web_client.cpp` 协议栈部分 | **退役**（PoW 由真实页面自行完成）—— **保留** `delta_text_of()` 与 `web_session_failure_hint()` 复用（数据源改为 CDP） |
| `vcpkg.json` 的 `webview2` + `CMakeLists.txt` 的 `unofficial-webview2` | **移除**（`PM-01` WebView2 Runtime 检测**作废**；改 `M7B-14` 浏览器检测：Chrome 缺失 → Edge 兜底） |
| `paths::webview2_profile()` | 退役 → `pydoll_profile()`（`~/.brain-ai/pydoll-profile`）；旧目录**保留为遗留、不自动删** |

### 4.4 可插拔 Provider（L2 底座 · 2026-10-11 · `M_patchAB` 批 6）

> **一句话**：**换厂商 = 表里加一条 / 换协议 = 工厂加一个分支**；节点侧只认一份接口。

| 层 | 文件 | 职责 |
|---|---|---|
| 接口 | `ai/include/inference_provider.h` + `ai/src/inference_provider.cpp` | `GenerateParams` / `GenerateResult` / `class InferenceProvider{name, protocol, caps, generate}`；**能力门控** `capability_gate()`（纯函数，节点 / 界面 / 自检同一判据）与 `caps_for_call()`（**无表条目时守老行为**） |
| 传输 | `ai/include/http_json.h` + `ai/src/http_json.cpp` | 通用 JSON POST：**可取消**（`run_cancellable` ⇒ 取消立即返回）· 表驱动连接 / 读超时 · 4 MB 响应上限 · `raw_head` 诊断；纯函数 `append_query_param` / `parse_json_text` / `json_error_message` |
| 协议 | `ai/src/anthropic_provider.cpp`（Messages API）· `ai/src/gemini_provider.cpp`（generateContent）· `ai/src/deepseek/deepseek_official_provider.cpp`（openai 兼容，含多模态） | 各自的**请求体构造 / 响应解析 / 错误文案**（构造与解析都是纯函数 ⇒ `--selftest` V-13 离线断言） |
| 网页版 | `ai/src/deepseek/deepseek_web_client.cpp`（经 `DeepSeekWebProvider` 收编）· `ai/src/dom_web_client.cpp`（`protocol=dom`） | 网页版通道**不经工厂**的 `dom` 由 DOM 适配器承载（需要页面上下文） |
| 分派 | `ai/include/provider_factory.h` + `ai/src/provider_factory.cpp` | 按表 `protocol` 建 provider；**未知 `protocol` 明确报错**（`R9`，绝不静默回落） |
| 接线 | `nodes/src/local_nodes.cpp` | 门控 / 提示**全部来自表 `capabilities`**（不再有 `mode == "web"` 之类的硬编码分流） |

**新增一家（openai 兼容）**：往 `~/.brain-ai/providers.d/xx.json` 写一条 `{id, display, kind:"official", protocol:"openai", api_base, chat_path, auth_style, env_names, capabilities{…}, models[…]}`（**嵌套**写法，见 `ai/include/provider_spec.h` 头注释）→ 界面「设置 → 提供商与站点表 → 重新加载配置表」→ 下拉即出现。
**新增一条协议**：实现 `InferenceProvider`（建议直接复用 `http_json`）+ 在 `provider_factory.cpp` 分发 + `provider_spec.cpp` 的 `implemented_protocols()` 同步登记 —— **`--selftest` 的 V-14 会替你核对**「表里每条 `protocol` 都有实现」。
**验证命令**：`api_probe.exe --selftest`（V-13 请求构造 / V-14 工厂分派与门控 / V-15 回环端到端）· `api_probe.exe --exec-selftest` · `aiwrite.exe --provider-selftest --provider anthropic|gemini`（需 Key）· `aiwrite.exe --provider-dump`（末行给出**已实现协议**）。

### 4.5 网络与凭据（代理 / 自签证书 / 凭据库 · 2026-10-11 · `M_patchAB` 批 7）

> **一句话**：出不去就填代理；证书不受信才开自签兼容（**默认严格**）；API Key 一律 DPAPI 加密落盘（**永不落明文**）。

| 项 | 配置（`config.toml`） | 行为 |
|---|---|---|
| 代理 | `[network].proxy` | 手填优先；例 `http://127.0.0.1:7890`（带 `user:pass@` 也支持；日志里口令**打码**） |
| 读环境变量 | `[network].proxy_from_env`（默认 **true**） | 按目标 scheme 取 `HTTPS_PROXY` / `HTTP_PROXY`，兜底 `ALL_PROXY`（大小写都试） |
| 排除清单 | `[network].no_proxy` | 非空则**覆盖**环境变量 `NO_PROXY`；支持 `*` · `host` · `.corp.com` 后缀 · `host:port` · 逗号分隔 |
| 自签证书 | `[network].insecure_tls`（默认 **false = 严格校验**） | 唯一开关；界面打开时红字「⚠ 危险」；**只在内网/离线环境使用** |
| 凭据位置 | `[credentials].dir` | 空 = `~/.brain-ai/credentials` |
| 凭据保留 | `[credentials].ttl_days` | 「即将过期」提示 + 启动惰性清理（`0` = 不清理） |
| 凭据后端 | `[credentials].backend` | 本版本**只支持** `dpapi`；其它值 ⇒ `save/load` **明确报错且不写任何文件** |

**优先级（好解释、可断言）**：`NO_PROXY` 命中 ⇒ **直达** → `[network].proxy` → 环境变量（按 scheme）→ `ALL_PROXY`。
**自检**：`api_probe.exe --selftest` 的 **V-16**（16 条）—— 含**回环证明**（把代理指到本机 `httplib::Server`：目标域名**不存在**但请求经代理成功，服务器侧回读**绝对 URI**）与**回环反面**（直达时代理**收不到**请求）；`api_probe.exe --settings-assert` 覆盖代理 / TTL / 后端三类非法值。
**诊断**：`aiwrite.exe --cred-list` / `--cred-purge` 都会打印「后端 / 保留天数」；`api_probe.exe --diag-dump` 的 `[凭据]` 节新增「后端 / TTL」一行（**不出条目名与密钥**）。

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
| CMake 配置输出被 `CMake Warning (deprecated) at .../vcpkg.cmake:40 (cmake_policy): Compatibility with CMake < 3.10 will be removed` 刷屏（看起来像报错，实际 Configure 能完成） | **已修复**：vcpkg 工具链内部 `cmake_policy(VERSION 3.7.2)` 在 CMake 3.31+/4.x 上必然触发该弃用警告，且它在项目文件之前执行，只能由 **preset 层**关掉。现已：① `CMakePresets.json` 增 `"CMAKE_WARN_DEPRECATED": "OFF"`；② 工具链改用 preset 的 `"toolchainFile"` 字段（不再作为 cache 变量 `CMAKE_TOOLCHAIN_FILE`，避免反被 `unused-cli` 警告）；③ `CMakeLists.txt` 里加同样的兜底（供不用 preset 的 `cmake -S source -B build`）。实测**全新配置与增量配置的警告/错误均为 0 条**。注意：`CMAKE_SUPPRESS_DEVELOPER_WARNINGS` **不要**写进 preset（它由 CMake 自身消费，会被 `unused-cli` 反过来警告一次） |
| 中文显示为方块 | 检查 `C:\Windows\Fonts\msyh.ttc` 是否存在（或用 `assets/fonts` 覆盖字体） |
| CMake 找不到 vcpkg 包 | 确认 `vcpkg-installed/x64-windows/share` 下有对应目录（依赖是否装全） |
| `cmake` 命令找不到 | CMake 已装到 `C:\Program Files\CMake\bin`；新开的终端/重启 VS Code 后生效；脚本会自动回退到工程自带的 `third_party/cmake-3.31.6` |
| `Could not create named generator Visual Studio 18 2026` | 说明用的 CMake 版本过低（< 4.2），请确认走的是系统 CMake 4.4.3，而不是 `third_party/cmake-3.31.6` |

### 6.1 崩溃 / 断言错误排查（CRT assert · 退出码 3 · `0x80000003`）

**症状**：进程突然结束；退出码 `3` 或 `-2147483645`（`0x80000003` = STATUS_BREAKPOINT）；
输出在某一处**戛然而止**（缓冲丢失）；`app.log` 里没有"结束"行；不弹"未处理异常"。
含义：**调试版（Debug）里 CRT / STL 的断言被触发**（`_CrtDbgReport` → 中断）。它**不是**普通 C++ 异常，
所以 `try/catch`、SEH（`__try/__except`）**都抓不到**。

**四步定位法**（本项目 2026-09-27 实测有效，约 20 分钟定位到单条语句）：

1. **让"死在哪一段"可见**：在可疑函数里插入**带 `fflush` 的追踪标记**——
   `std::printf("[TRACE] 步骤名\n"); std::fflush(stdout);`
   再 `.\api_probe.exe --exec-selftest *> out.txt`，看文件里**最后一个** `[TRACE]`。
   （重定向时 stdout 是块缓冲，**不 `fflush` 就会丢** —— 这正是"输出戛然而止"的原因。）
2. **二分缩小**：标记放密（每 1–2 条语句一个），必要时临时 `#if 0` 掉整段做反证。
   本项目据此把范围从 2000 行缩到 1 个函数、再到 1 个调用。
3. **看 `~/.brain-ai/logs/app.log`**：spdlog 每条都落盘，比 stdout 可靠；最后一条日志
   往往就是崩溃前最后一个动作（如 `[文件对话框] 选择文件: …`、`[纹理缓存] …`）。
4. **查外部依赖**（Windows 特有）：系统编解码器 / Store 扩展（`Get-AppxPackage *Webp*`、`*HEIF*`、`*AV1*`）、
   GPU 驱动、WebView2 Runtime 都可能**收到可疑输入**时中断进程（⚠️ WebView2 计划退场后该项收窄为「浏览器进程 / GPU 驱动」）。
   定位后必须把"可疑输入"**定义清楚并写进冻结区**（见 §4.2）。

**排错纪律（防止复发）**：

- 定位用的 `[TRACE]` 标记**提交前必须删净**（`git diff` 复查只能看到结论性改动）。
- 每个断言/崩溃修复必须同时留下四件套：① 可复现命令 ② 最后一条日志/最后一个标记
  ③ 冻结规则（不变量编号，如 `I17`） ④ 回归基线数字（本次：`--exec-selftest` **237/0 → 251/0**）。
- **Debug 崩溃 ≠ Release 正常**：Release 只是不做断言，同一路径仍可能算错或写坏数据；
  必须按**根因**修，修后 Debug / Release 一致通过。
- 排查结论写进 `CHANGELOG.md` 与本手册；影响架构的写进 `docs/Archive/actionPlan/M7.md`（📦 已归档 2026-10-11 · 只读契约 · 含判据与不变量）。

### 6.2 一次性探针脚本的纪律（`M7B` 前置验证教训 · 2026-09-28）

`M7B` 的前置技术验证用**一次性脚本**（`source/python/_probe/`，不进主管道）在真实浏览器里取证。
> **目录口径（2026-10-03 迁移 · 代码 / 数据分离）**：Python **运行代码**（守护进程包 `brain_ai_browser/` + 探针 `_probe/` + `requirements.txt` + `.venv/`）统一在 **`source/python/`**（随源码树入库）；**运行期数据**一律落 `%USERPROFILE%\.brain-ai`（`logs` / `pydoll-profile` / L2 快照 `session/`）—— **代码入库、数据不入库**。命令口径：`cd source\python` → `.venv\Scripts\python.exe -m brain_ai_browser …`（`--pipe-selftest` 的包目录候选同步改为 `AIWRITE_SOURCE_DIR / "python"`）。
本轮**两条错误结论**（「页面 hook 未打通」「M7B-05 全 FAIL」）都不是机制问题，而是**探针自身**读错：

1. **读回必须自证**：任何"机制不可用 / 全 FAIL"的结论，**先排除探针**。写法：
   在目标读数**之前**先执行一次已知常量（例：`execute_script('return "probe-ok"')`）并断言其返回值。
   本项目踩到的具体坑：Pydoll `execute_script` 的返回是**两层 `result`**
   （`{'id':N,'result':{'result':{'type','value'}}}`）—— 只解一层会**静默拿到空串**，
   于是"页面没反应"与"我读错了"无法区分。
2. **判定逻辑要与语义对齐**：例：Cookie **按域名隔离、端口不参与**（`127.0.0.1:A` 与 `127.0.0.1:B` 同域），
   因此「注销 A 后 B 是否完好」**不能按 Cookie 名比对**（两域同名），必须按**各自视角**判定（A 视角为空 + B 视角完整）。
3. **读数有作用域**：`tab.get_cookies()`（库实现：无 `browser_context_id` 时走 `Network.getCookies` **不带 urls**）
   **只回「当前页面 URL」的 Cookie** —— 停在 `about:blank` 时**必然读空**。要读**全库**用 `Storage.getCookies`；
   要按站点读用 `Network.getCookies(urls=[...])`。本轮的"attach 只读到 0 条"就是这一条
   （脚本 `source/python/_probe/_diag_cookie_scope.py` 四步实证：空 / 导航后 13 条 / 全库 16 条 / 按 URL 13 条）。
4. **探针必须 `try/finally` 收尾**：崩在 `start()` 之前会**遗留孤儿浏览器实例占住 profile**，
   使**下一次 `start()` 直接 `FailedToStartBrowser`**（本轮遇到两次，现象像"机制坏了"，实为自己留的残骸）。
   收尾时清点/清理**归属该 profile** 的进程（`source/python/_probe/m7b09_common.py` 的 `stray_browsers()` / `kill_strays()`）。
5. **一因多果要逐个排除**：同一现象（"读到空"）先列假设（**不同上下文 / 不同 profile / 读时机 / 作用域**），
   再用**可证伪的证据**逐一筛 —— 例如用 `chrome://version` 的 `Profile Path` 反证"profile 被换"，
   用裸 `Storage.getCookies` 反证"上下文/时机"。**不要在第一层解释上收工**。

> 相关：`M7B` 关闭时序与登录态持久化的四轮实测（**干净退出只保住持久 Cookie**；会期 Cookie 需 L2 加密快照回灌；
> 有既有实例时只能 attach、**禁止强杀**）见 [../docs/actionPlan/M7B.md](../docs/Archive/actionPlan/M7B.md) §5
> （`M7B-09` 结论块）与 §13。

---

## 7. 源码文件速览

> **路径口径（2026-10-09 · `FEA-L05` · 现行）**：**每个模块自带 `src/` + `include/`** —— `.cpp` 在 `source/<模块>/src/**`、头文件在 `source/<模块>/include/**`（**模块内拍平**）；模块 = `ai` / `engine` / `nodes` / `ui` / `utils` / `web`；AI 专属实现按站点分目录（`ai/src/deepseek/`、`ai/src/doubao/`、`ai/src/kimi/`）；程序入口 = `app/src/main.cpp`。例：`ui/include/theme.h` ↔ `ui/src/theme.cpp`。规范全文见 [../AI_README.md](../AI_README.md) §11。


| 文件 | 作用 |
|---|---|
| `app/src/main.cpp` | 入口：解析参数 → 初始化日志/目录 → 启动 UI |
| `ui/src/app.cpp` | GLFW + OpenGL3 + ImGui、深色主题、中英文字体、DockSpace 布局、菜单栏、工具栏行、状态栏 |
| `ui/src/editor_state.cpp` | 编辑器状态：唯一持有 `Graph` + `UndoStack`；创建/删除/复制/粘贴/清空/示例工作流（一律先压快照） |
| `ui/src/node_canvas.cpp` | 节点画布：渲染 Graph（节点卡片/端口/连线/参数预览）、全部鼠标交互与三个右键菜单、视图导航 |
| `ui/src/node_library.cpp` | 节点库面板：按分类列出 14 个节点（含搜索高亮），点击在画布中心创建 |
| `ui/src/toolbar.cpp` | 工具栏：撤销/重做/复制/粘贴/删除选中/新建/示例 + 计数（无快捷键，纯鼠标） |
| `ui/src/property_panel.cpp` | 参数面板：9 类参数控件 + 校验提示 + 文件/目录选择 + 密码框 |
| `ui/include/theme.h` | 分类 / 端口类型 / 节点状态配色（设计 14.3 / 4.5 / 4.6） |
| `ui/src/console_panel.cpp` | Console 面板（级别过滤 + 搜索 + 虚拟化 + 自动滚动，设计文档 12.4） |
| `engine/src/graph.cpp` | 数据模型：Node/Port/Param/Edge/Graph、类型兼容矩阵、参数校验、增删与复制 |
| `engine/src/node_registry.cpp` | 8 个 MVP 节点的注册表（类型/端口/参数/默认值与范围；M7 起 N-09 ImagePreview 已移除） |
| `engine/src/undo_stack.cpp` | 快照式撤销/重做（深度 50） |
| `utils/src/paths.cpp` | `~/.brain-ai` 数据目录与各子路径 |
| `utils/src/log.cpp` | spdlog 双 sink（`app.log` 10MB×5 + 控制台）+ 内存环形缓冲（10000 条） |
| `utils/src/config.cpp` | toml++ 读写 `config.toml`（字段与设计文档 20.2 一致，缺省自动生成） |
| `utils/src/crypto.cpp` | SHA3-256（OpenSSL EVP，供 PoW 与自检使用） |
| `utils/src/file_dialog.cpp` | 原生文件/目录对话框（nativefiledialog-extended，`NFD::Init/Quit` 配对） |
| `utils/src/image_decode.cpp` | **图片格式嗅探 + 解码（M7 · 冻结区）**：魔数嗅探（内容优先）/ stb 与 WIC 双后端 / WIC 能力枚举 / 可操作错误文案 |
| `utils/src/asset_store.cpp` | **统一资源目录（P7a-04）**：内容寻址归档 `<SHA3-256>.<ext>`（同内容一份）/ 令牌 `aiwrite-asset:<摘要>` 解析 / 缺失文案（`~/.brain-ai/assets/images/`） |
| `ai/include/inference_provider.h` · `ai/src/inference_provider.cpp` | **L2 底座（`M_patchAB` 批 6）**：统一推理接口 + 能力门控（`capability_gate` / `caps_for_call`）+ openai 兼容与内置网页版实现 |
| `ai/include/provider_factory.h` · `ai/src/provider_factory.cpp` | **L2 分派**：按表 `protocol` 建 provider（`openai` / `anthropic` / `gemini` / `deepseek-web`）；未知 protocol 明确报错（`R9`） |
| `ai/include/http_json.h` · `ai/src/http_json.cpp` | **L2 传输**：通用 JSON POST（可取消 / 表驱动超时 / 响应上限 / 诊断）+ query 拼接 / JSON 解析 / 错误提取纯函数 |
| `ai/src/anthropic_provider.cpp` · `ai/src/gemini_provider.cpp` | **L2 协议**：Anthropic Messages API · Gemini generateContent（请求构造与响应解析均为纯函数） |
| `tools/api_probe.cpp` | V-04/V-05/V-06 命令行验证工具 + `--graph-selftest` 图模型自检 + `--exec-selftest` 执行器自检 + `--selftest` 十二组（含 V-11…V-15） |
| `tools/webview2_login.cpp` | V-03 WebView2 登录 + Cookie 提取（仅内存，脱敏打印）—— ⚠️ **计划删除**（`M7B-30`，改 `pydoll_login`） |
