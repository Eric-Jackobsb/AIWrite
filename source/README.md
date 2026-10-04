# AIwrite 源码工程（M1 骨架 + M2 节点系统 P1/P2）

> 设计文档：`../docs/ai_writer_nodes.md`　Action Plan：`../docs/actionPlan/`（进行中：M3–M6 + 补丁；已完成归档 → [../docs/Archive/actionPlan/](../docs/Archive/actionPlan/)）
> 验证报告：`../docs/Archive/M1_技术验证报告.md`　节点编辑器手册：`../docs/节点编辑器使用说明.md`
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
.\aiwrite.exe --pydoll-selftest
                              # 【新通道 · step 6】起 Python 守护进程 → hello → ready{proto=1} → shutdown（**不开浏览器**）
                              # 退出码 0=通过 / 1=失败 / 2=依赖问题（无 Python / 无浏览器）；只验通道，不影响生产路径
.\aiwrite.exe --pydoll-login deepseek-web --timeout 300
                              # 【新通道 · step 6】起**独立** Edge 窗口，人工登录站点（不代填密码、不绕验证）
                              # 判据 = cookie_names 命中则关窗；未知 id → 退出码 2 + 可操作提示、**不开窗**
.\api_probe.exe --selftest                  # V-04 HTTP + V-05 SHA3 自检
.\api_probe.exe --graph-selftest            # 图模型/注册表/撤销栈/序列化 自检（110 项断言，无需网络）
.\api_probe.exe --exec-selftest             # 拓扑 + 加载/运行前校验 + 执行器 + 图片解码 自检（326 项断言，无需网络）
.\api_probe.exe --image-decode D:\a.webp   # 图片诊断：内容嗅探 / MIME / 解码(stb|wic) / 尺寸 / WIC 能力（M7）
.\api_probe.exe --sha3 "abc"                # 单次 SHA3-256
$env:DEEPSEEK_API_KEY="sk-..." ; .\api_probe.exe --chat "你好"   # V-06（需 Key）
.\webview2_login.exe --selftest --timeout 30   # V-03 自检：离屏跑「导航 → 提取 Cookie」，退出码 0=PASS/1=FAIL/2=超时
.\webview2_login.exe                        # V-03 交互：打开站点手动登录（Ctrl+Alt+C 提取 Cookie）
.\webview2_login.exe --ephemeral            # 使用临时 profile（退出丢弃登录态）
```

> ⚠️ **上述 `webview2_login` 三行计划退场（M7 第三轮 `M7B`，2026-09-28 · 见 §4.3）**：WebView2 退场后由
> **`pydoll_login --provider <id>`** 取代（退出码三档语义不变）；`webview2_login.cpp` 从 `tools/` 删除。
> 计划全文见 [../docs/actionPlan/M7B.md](../docs/actionPlan/M7B.md) §8.4 / `M7B-30`。

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

### 4.1 网页版登录与多站点（2026-09-26 已支持 · ⚠️ **当前仍走 WebView2**，Pydoll 版见 §4.3）

- **站点身份按「生效条目」**：登录页 / 窗口标题 / 探测路径 / Cookie 名全部取自条目的 `web.*`
  （`src/web/webview_host.h` 的 `interactive_login_request(site,id)` 等）；无参重载 = 旧常量（逐字一致，兼容保留）。
  ✅ **已落地（v9 · `D-22②` + `D-26`）**：**站点不再回落** —— 条目不是网页版条目（或表外 id / `web.login_url` 为空）时，站点区给**错误块 + 三条引导**（开窗按钮禁用）、运行前校验提示「**本次运行必定失败**」（**不阻断**）、运行期直接 `NodeError`；适配器未实现（如 `dom`）→ 运行期**明确报错**（可登录 / 可探测，生成待 L3）。不变量 `I14`。
- **加一个网页版站点**：把 `{"schema_version":1,"providers":[ … ]}` 写进 `~/.brain-ai/providers.d/<id>.json` 并**重启**（无 reload 入口 —— `PB2-24`）；`web` 段用**嵌套** `send{kind,value}` / `done_when{kind,selector}` —— 实测可用样例见 [../docs/actionPlan/M_patchAB_rest.md](../docs/actionPlan/M_patchAB_rest.md) **附录 D**（现行版本）。
- **内置站点入口（v10 / `PB2-26`）**：配置表已内置 **11 个 AI 的网页版登录入口**（Kimi / 通义千问 / Qwen 国际站 / 智谱清言 / 豆包 / 腾讯元宝 / 文心一言 / 讯飞星火 / ChatGPT / Claude / Gemini）+ 原有 `deepseek-web`；条目为**登录型**（只配 `login_url` + `window_title`）：**能登录 / 能探测**，**生成**需先补齐选择器（DOM 执行器 L3 v11 **已就绪**；选择器由 L4 `PB2-29` 逐站实测回填）→ 未填时点运行会**明确报错**（不静默、不回落 DeepSeek）。完整清单见 [../docs/actionPlan/M_patchAB_rest.md](../docs/actionPlan/M_patchAB_rest.md) **附录 E**（现行版本）。
- **L3（v11）：DOM 适配器已接线**（`src/ai/dom_web_client.{h,cpp}`）—— 条目补齐 **`web.input_selector` / `web.send` / `web.answer_selector`**（+ 建议 `web.done_when`）**重启即真正生成**（写入提示词 → 自动发送 → 轮询取答案；到上限/超时**如实返回已取文本 + 警告**）；不要求内存 `userToken`（登录态在浏览器 profile）；选择器不会写就用 **`--web-adapter-selftest --provider <id>`**（只读命中数 + 修复建议）。站点 = 纯数据，**无需改 C++**。
- **L4（v12 · 文档先行，代码待 `B2-e`）：登录层去 DeepSeek 化 + 站点数据落地 + 自助闭环** —— ①**登录态判据站点无关**（`web_session_state()` 纯函数：按该 origin 的 Cookie / `cookie_names` 判，**不看** `userToken`；`userToken` 行仅当条目配了 `token_expr` 才显示）②**探测「不适用」语义**（条目未配 `probe_paths` / `token_expr` / `endpoints` 时不再注入 DeepSeek 端点 → 只读诊断分支；`deepseek-web` 与无参路径**逐字不变**）③**内置站点选择器逐站实测回填**（`--web-adapter-selftest --provider <id>` → 填条目 → `verified:true`）④**自建站点 UI 闭环**（新建条目 / **重新加载配置表** / 测试选择器；`PB2-24` 并入）。计划：**现行** [../docs/actionPlan/M_patchAB_rest.md](../docs/actionPlan/M_patchAB_rest.md) §2.2 / §3.3–§3.5（承接 `PB-07` / `PB2-25` / `PB2-29` / `PB2-30②` / `PB2-24`）；历史 §9.7 见归档 [../docs/Archive/actionPlan/M_patchB.md](../docs/Archive/actionPlan/M_patchB.md)。

- **「模式」下拉恒两项**（`src/engine/provider_resolve.cpp` 的 `provider_mode_options`）：`official` / `web` **始终可选**
  （决策 `D-21`：**网页版与官方 API 同等优先级**，**不按条目 `kind` 裁剪**）；切换「提供商」只带出**建议值**
  （网页版条目建议 `web`、官方条目建议 `official`），不一致时给橙色**提示**：非网页版条目 + `web` → 用内置默认站点；
  网页版条目 + `official` → 该条目没有官方 API 通道（按表内 `api_base` 解析，缺失则明确报错）。
  > ✅ **已修复（2026-09-26 · 任务 `PB2-20`）**：候选**恒两项**、`kind` 与 `mode` 不一致**只提示不改写**（守 `I13`）；
  > 参数面板 / 运行前校验改按 `resolve_display_provider()`（**该节点自身条目**）解析 —— 站点条目 / 登录页 / 提示不再误回落 DeepSeek（`VB2-18②③`）。
- **会话按站点键控**（`src/web/session_store.h`，`site_key_of()` = origin）：`std::map<站点, Session>`，
  旧无键 API 保留为「默认槽」薄封装 → `--login-selftest` / `--web-probe` / `--web-chat` / `--web-session-selftest` 行为不变。
- **窗口串行复用**：同一时刻一个登录窗口；切站点时先关旧窗再按新站点开窗（页面内 PoW 求解依赖该站点页面）。
- **注销按站点**（`web::logout_site()`）：清该站点内存会话 + 删该 origin 的 Cookie + 清同源 `localStorage`；
  「删除整个 profile（所有站点）」在参数面板「高级」里，需二次确认。
- 内置配置表 `assets/providers.json` 共 **21 条**（official 9 / web 12），其中 `kind=web` 为 `deepseek-web`（**唯一已填选择器、可生成**）+ **11 条登录型站点**（v10 / `PB2-26`；**L4 `PB2-29` 逐站补选择器**）
  （Kimi 等站点已作为**登录型条目**内置，不再是「仅模板」）；要接入**新**站点，把站点 JSON 放进
  `~/.brain-ai/providers.d/`（照 `_example_web_dom` 抄）。
- **`config.toml` 多 provider 实例参数**（PB2-06）：`[providers.<id>]` 可多节（`deepseek` 旧节自动迁移，幂等）；
  保存前自动备份 `config.toml.bak`，写入采用 `.tmp` → 原子替换。**厂商元数据仍以配置表（JSON）为准**。
- 计划与验收：`PB2-17`/`PB2-18`/`PB2-19`/`PB2-06`/`PB2-20`（`D-21` 修订）、`I11`/`I12`/`I13`、`AB2-13`/`AB2-14`/`AB2-15`、`VB2-16`/`VB2-17`/`VB2-18`、`R14`/`R15`/`R16`，
  见归档 [../docs/Archive/actionPlan/M_patchB.md](../docs/Archive/actionPlan/M_patchB.md)（§9「L1 收口」实测基线；**L4 立项见 §9.7**）与**现行承接** [../docs/actionPlan/M_patchAB_rest.md](../docs/actionPlan/M_patchAB_rest.md)（§3 `B2` 残项）与
  [../docs/网页版协议实测记录.md](../docs/网页版协议实测记录.md) §7。

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
| 发给模型（多模态） | `src/ai/deepseek_official_provider.cpp::image_mime_from_path`（**内容嗅探优先**、扩展名回退） | 只读文件字节 + 判类型，本地不解码；失败与否取决于后端 |
| 本地预览 / 尺寸 | `src/utils/image_decode.cpp` | stb：PNG/JPEG/BMP/GIF/TGA/PSD/HDR/PNM；WIC：WebP/TIFF/ICO/JXR/HEIF/AVIF（需系统装了对应「图像扩展」） |

> ⇒ **本地预览失败 ≠ 模型读不到**。错误文案里会明确写这一点（「图片理解」按内容识别格式后发给模型）。

**冻结区（FROZEN · 不变量 `I17`）** —— `src/utils/image_decode.cpp` 的 WIC 相关代码**冻结**，改动前必须遵守：

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
定位过程与修复见 §6.1 与 [../docs/actionPlan/M7.md](../docs/actionPlan/M7.md) §1。

**计划中的不变量（`I18` / `I19` · 2026-09-27 会议立项 · 属 M7 第二轮 P7，见 [../docs/actionPlan/M7.md](../docs/actionPlan/M7.md) §15）**
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
（失败项保留原值，**不静默改写用户文件**）。细则见 [../docs/actionPlan/M7.md](../docs/actionPlan/M7.md) §11.1（`P7a-04`~`P7a-07`）。

---

### 4.3 「引擎唯一化」与 `I2` 解冻（M7 第三轮 `M7B` · 2026-09-28 立项 · **批 1 step 1~6 已落地 / 生产路径未切换**）

> 本节是**计划登记**。**批 1 step 1~6 已落地**（见下方「运行时通道归属」），但**生产路径刻意未切换**
> （`MB-D1` 先建后拆 / `B12-C1` 只新增不替换）。计划全文见 [../docs/actionPlan/M7B.md](../docs/actionPlan/M7B.md)。

#### ⚠️ 当前运行时通道归属（**2026-10-03 实测** · 防误判：**现在跑的仍是 WebView2**）

> **主程序（GUI）里所有网页功能仍然 100% 走 WebView2。** 批 1 step 1~6 建的是**旁路新通道**
> （`src/web/pydoll_channel.*`），调用方**零改动** ⇒ **看到 WebView2 是符合设计的状态，不是缺陷**。
> 逐入口实测归属表 + 分辨方法见 [../docs/actionPlan/M7B.md](../docs/actionPlan/M7B.md) §1.4。

| 入口 / 命令 | 现在**实际走** | 备注 |
|---|---|---|
| 面板「打开登录窗口（WebView2）」/「按站点注销」 | **WebView2** | `src/ui/property_panel.cpp:230,319,170`（按钮文案仍写死 WebView2） |
| 登录型节点 / 网页版文字生成（`adapter=dom`） | **WebView2** | `src/nodes/local_nodes.cpp:545`、`src/ai/dom_web_client.cpp:245,256,262,297` |
| `--login-selftest` / `--web-probe` / `--web-chat` / `--run-selftest --web` | **WebView2** | 同上 |
| **`--pydoll-selftest`**（新 · step 6） | **Pydoll** | 起守护进程 → `hello` → `ready{proto=1}` → `shutdown`；**不开浏览器**；exit 0 |
| **`--pydoll-login <id>`**（新 · step 6） | **Pydoll** | **独立** Edge 窗口 + profile `~/.brain-ai/pydoll-profile/`（WebView2 则是 `~/.brain-ai/webview2/`） |

**怎么分辨（Pydoll 底层也是 Edge，别比窗口长相）**：比 **profile 目录**（`pydoll-profile` vs `webview2`）、
**是否独立任务栏窗口**、**是否有 `~/.brain-ai/logs/pydoll_channel_daemon.log`**。

**切换前置（硬缺口 · 实测）**：Python 侧 `daemon.py` 只实现 `hello`/`shutdown`/`open_tab`/`login_state`，
`send_prompt`/`read_answer`/`upload_image` 如实回「尚未实现（批 3）」；C++ 侧 `logout_site`/`run_script`/
`current_tab_site`/`tab_on_site` 为如实占位（`src/web/pydoll_channel.h:39,42,46,47`）
⇒ **生产路径切换（`M7B-20`）排在批 3**。

**冻结区变更（解冻规则）**：

| 项 | 内容 |
|---|---|
| 解冻规则 | **`I2`** —— 原文「`--web-chat` / `--web-probe` / `--web-session-selftest` **行为不变**」（归档 `M_patchB.md:294`），实际被用来护住「无参 CLI = 内置 DeepSeek 常量」的一整套兼容装置 |
| 解冻原因 | 宿主 **WebView2 退场**（嵌入控件易被站点识别为非真实浏览器，`MB-D0-2`）；且站点「**不回落**」（`D-22②` / `I14`）此前对 CLI 路径存在豁免，需**贯彻到底**（`MB-D0-6`） |
| 替代物 | **`I20`（CLI 契约）**：命令名 / 参数形式 / **退出码语义**在 `--help` + 文档 + 断言三处一致；缺 `--provider` → **列候选 + 退出码 2**（不得静默取第一个）；`--provider auto` 为**显式保留字**；自检命令不得残留状态 |
| 回归基线数字 | `--exec-selftest` **251/0 →（先降后升）→ 实测回填**（删 `VB2-17` 前半 + `VB2-25①③④`；新增 `VB2-28`~`VB2-36`）；路径见 [../docs/actionPlan/M7B.md](../docs/actionPlan/M7B.md) §9.3。**现状（2026-10-03 · step 6）= 326 / 0**（批 1–4 **只升不降**，删除类动作集中批 5） |

**新增不变量（拟进冻结区 · 与 `I18` / `I19` 同批登记）**：

1. **`I21`（不静默降级）**：Python / 浏览器 / 守护进程 / 登录态任一缺失 → **报错 + 可操作引导 + 退出码 1/2**；
   **绝不换通道、绝不换身份、绝不假装成功**（明确禁止「回落 WebView2」与「改用另一套登录态」两种行为）。
2. **`I22`（流式降级必须显式）**：CDP 增量不可用 → 退回 DOM 轮询，但 UI 与日志**必须标注「非流式（轮询）」**。
3. **`I23`（登录态持久化 · 2026-09-28 前置验证后新增）**：退出必须**先 `Browser.close` 并等进程退出**
   （超时才强杀），**禁止「close 后立刻 kill」**；持久 Cookie 的存续不得依赖单一机制（干净退出 + **DPAPI 加密快照**双保险）；
   快照**不得明文落盘 / 不得外传**；恢复失败必须**显式提示**。配套决议 **`MB-D0-8`**（四层：L1 干净退出 /
   L2 加密快照 / L3 启动自愈 / L4 异常退出可见），断言 `VB2-38` / `VB2-39`，见
   [../docs/actionPlan/M7B.md](../docs/actionPlan/M7B.md) §2 / §5 / §9。

**将退役 / 改写的实现（计划 · 见 `M7B-30` / `M7B-31` / `M7B-37`）**：

| 对象 | 处置 |
|---|---|
| `src/web/webview_host.{h,cpp}`（58 + 6 处） | **删除** → `src/web/pydoll_channel.{h,cpp}` + `web/pipe_client.{h,cpp}` |
| `tools/webview2_login.cpp`（60 处） | **删除** → `pydoll_login`（V-03 记录标注「已被 `M7B` 取代」） |
| `ai/web_pow.{h,cpp}`、`ai/deepseek_web_client.cpp` 协议栈部分 | **退役**（PoW 由真实页面自行完成）—— **保留** `delta_text_of()` 与 `web_session_failure_hint()` 复用（数据源改为 CDP） |
| `vcpkg.json` 的 `webview2` + `CMakeLists.txt` 的 `unofficial-webview2` | **移除**（`PM-01` WebView2 Runtime 检测**作废**；改 `M7B-14` 浏览器检测：Chrome 缺失 → Edge 兜底） |
| `paths::webview2_profile()` | 退役 → `pydoll_profile()`（`~/.brain-ai/pydoll-profile`）；旧目录**保留为遗留、不自动删** |

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
- 排查结论写进 `CHANGELOG.md` 与本手册；影响架构的写进 `docs/actionPlan/M7.md`（含判据与不变量）。

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
> 有既有实例时只能 attach、**禁止强杀**）见 [../docs/actionPlan/M7B.md](../docs/actionPlan/M7B.md) §5
> （`M7B-09` 结论块）与 §13。

---

## 7. 源码文件速览

| 文件 | 作用 |
|---|---|
| `src/main.cpp` | 入口：解析参数 → 初始化日志/目录 → 启动 UI |
| `src/ui/app.cpp` | GLFW + OpenGL3 + ImGui、深色主题、中英文字体、DockSpace 布局、菜单栏、工具栏行、状态栏 |
| `src/ui/editor_state.cpp` | 编辑器状态：唯一持有 `Graph` + `UndoStack`；创建/删除/复制/粘贴/清空/示例工作流（一律先压快照） |
| `src/ui/node_canvas.cpp` | 节点画布：渲染 Graph（节点卡片/端口/连线/参数预览）、全部鼠标交互与三个右键菜单、视图导航 |
| `src/ui/node_library.cpp` | 节点库面板：按分类列出 8 个节点，点击在画布中心创建 |
| `src/ui/toolbar.cpp` | 工具栏：撤销/重做/复制/粘贴/删除选中/新建/示例 + 计数（无快捷键，纯鼠标） |
| `src/ui/property_panel.cpp` | 参数面板：9 类参数控件 + 校验提示 + 文件/目录选择 + 密码框 |
| `src/ui/theme.h` | 分类 / 端口类型 / 节点状态配色（设计 14.3 / 4.5 / 4.6） |
| `src/ui/console_panel.cpp` | Console 面板（级别过滤 + 搜索 + 虚拟化 + 自动滚动，设计文档 12.4） |
| `src/engine/graph.cpp` | 数据模型：Node/Port/Param/Edge/Graph、类型兼容矩阵、参数校验、增删与复制 |
| `src/engine/node_registry.cpp` | 8 个 MVP 节点的注册表（类型/端口/参数/默认值与范围；M7 起 N-09 ImagePreview 已移除） |
| `src/engine/undo_stack.cpp` | 快照式撤销/重做（深度 50） |
| `src/utils/paths.cpp` | `~/.brain-ai` 数据目录与各子路径 |
| `src/utils/log.cpp` | spdlog 双 sink（`app.log` 10MB×5 + 控制台）+ 内存环形缓冲（10000 条） |
| `src/utils/config.cpp` | toml++ 读写 `config.toml`（字段与设计文档 20.2 一致，缺省自动生成） |
| `src/utils/crypto.cpp` | SHA3-256（OpenSSL EVP，供 PoW 与自检使用） |
| `src/utils/file_dialog.cpp` | 原生文件/目录对话框（nativefiledialog-extended，`NFD::Init/Quit` 配对） |
| `src/utils/image_decode.cpp` | **图片格式嗅探 + 解码（M7 · 冻结区）**：魔数嗅探（内容优先）/ stb 与 WIC 双后端 / WIC 能力枚举 / 可操作错误文案 |
| `src/utils/asset_store.cpp` | **统一资源目录（P7a-04）**：内容寻址归档 `<SHA3-256>.<ext>`（同内容一份）/ 令牌 `aiwrite-asset:<摘要>` 解析 / 缺失文案（`~/.brain-ai/assets/images/`） |
| `tools/api_probe.cpp` | V-04/V-05/V-06 命令行验证工具 + `--graph-selftest` 图模型自检 + `--exec-selftest` 执行器自检 |
| `tools/webview2_login.cpp` | V-03 WebView2 登录 + Cookie 提取（仅内存，脱敏打印）—— ⚠️ **计划删除**（`M7B-30`，改 `pydoll_login`） |
