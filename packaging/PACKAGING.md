# AIwrite 打包与分发说明（`M6-08` · Inno Setup 一键安装包）

> 面向「要自己产出安装包 / 给他人分发」的人。普通使用者请看仓库根 [README](../README.md) §1「安装」。

## 一条命令

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\package.ps1 [-Version x.y.z] [-SkipBuild] [-SkipInstaller]
```

流程：①（可选）构建 → ② 收集 payload（`build\dist\payload`）→ ③ 调 `ISCC.exe` 编译 `packaging\aiwrite.iss`
→ 产出 `build\dist\AIwrite-Setup-<版本>.exe`。

**退出码**：`0` = payload + 安装包都成功 ｜ `2` = 只完成 payload（未装 ISCC，脚本已打印获取方式）｜ `1` = 失败。
产物与日志：payload 在 `build\dist\payload`（含 `manifest.txt`），日志在 `build\logs\package_<时间戳>.log`。

## 关键口径（刻意如此，别「顺手优化」）

| 口径 | 说明 |
|---|---|
| **不含 Python 运行时** | `build\bin\python\**` 被排除（M8 §0「零 Python 运行时」是长期方向）⇒ 安装版**没有** `--pydoll-*` 能力；开发态自检直接用 `build\bin` |
| **不下载 / 不静默安装 WebView2** | 安装包不装第三方依赖；由程序「首次使用引导」检查依赖并给官方下载页 |
| **版本号单一来源** | `source\utils\include\app_version.h` 的 `kAppVersion`；`-Version` 默认值与之字面一致。**改版本号 = 开发者明确指令动作**（`AI_README` §6） |
| **payload 离线可验证** | 不需要 Inno Setup 也能验证（产物 = 目录 + manifest.txt） |

## 安装包内容（`packaging\aiwrite.iss`）

- 主程序 + 诊断工具（`aiwrite.exe` / `api_probe.exe`）、运行依赖 DLL（glfw3 / spdlog / openssl / WebView2Loader 等）、`assets\`（`providers.json` 站点表等）。
- **可选** WebView2 Bootstrapper：把 `MicrosoftEdgeWebview2Setup.exe` 放进 `packaging\webview2\` 即随包
  （安装向导最后一步出现**可选**勾选项「安装网页版站点所需的 Edge WebView2 运行时（需联网）」，默认**不勾选**）；
  没有该文件 ⇒ `.iss` 里 `skipifsourcedoesntexist` **静默跳过**，界面回落到官方下载页。详见 [webview2/README.md](webview2/README.md)。
- 允许**非管理员**安装（装到用户目录）；`x64`。

## 与用户数据的关系

安装 / 卸载**都不碰** `%USERPROFILE%\.brain-ai\**`（配置 / 凭据 / 登录态 / 图片）；卸载只移除程序目录 ⇒
重装不丢数据，卸载也不删数据（想彻底清理请手动删该目录）。

## 干净机器冒烟（`TST-M6-01` · 人工）

1. 在**没装过本程序**的机器上双击 `AIwrite-Setup-<版本>.exe` → 按向导装完；
2. 首次启动应弹出「首次使用引导」：第 1 步**环境检查**（WebView2 缺失 → 官方下载页 / 随包安装 / 重新检测）；
3. 载入示例工作流 → 点「运行」跑通（不配 Key 的节点失败是**预期**，错误文案可操作）。

## 当前状态（2026-10-16）

- payload 阶段与 `.iss ↔ payload` 一致性锁（4 条 Source 通配逐条核对）**已实测**；
- ⚠ 本机**未编译出安装包**（开发机无 ISCC 且离线，winget `0x80072efd`）⇒ 上面「干净机器冒烟」挂在你的环境；
- 本机**未附带** WebView2 Bootstrapper（同上网络原因）⇒ 安装包界面回落到官方下载页（如实登记，不假装已附带）。
