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

## 当前状态（2026-10-17）

- payload 阶段与 `.iss ↔ payload` 一致性锁（4 条 Source 通配逐条核对）**已实测**（2026-10-17 复核：**19 文件 / 34.6 MB** · 含随包 Bootstrapper · `*.py=0`）；
- **打包流水线已入 `check_all` 第 ⑦ 项**（2026-10-17 · 批 ③）：`package.ps1 -SkipBuild -SkipInstaller` 判据 = 退出码 ∈ {0, 2} 且 manifest 存在 ⇒ 一键自检 **16 项**门禁；
- ✅ **安装包已编译**（2026-10-17 批 ⑥ · winget 装 Inno Setup **6.7.3** → `package.ps1` 全流程 exit 0）：`build\dist\AIwrite-Setup-0.2.0.exe` **9.1 MB**（payload 19 文件 / 34.6 MB · `.iss↔payload` 一致性锁 4/4 PASS）⇒ 上面「干净机器冒烟」待新机执行（`TST-M6-01` · Human ①-2）；
- ✅ 本机**已附带** WebView2 Bootstrapper（2026-10-17 批 ⑤ · 网络恢复后自动下载 1.78 MB ⇒ 安装向导出现**可选**「安装 WebView2 运行时（需联网）」勾选项，默认不勾选 · `PQ-02` 可选件口径不变）。
