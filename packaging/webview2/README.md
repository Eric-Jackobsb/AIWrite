# WebView2 Bootstrapper（**可选件** · `PQ-02`「安装包附带」）

本目录**刻意留空**。`PQ-02` 已拍板「安装包附带 Edge WebView2 Bootstrapper」，但该文件是
Microsoft 的第三方安装程序，**不随仓库分发**；需要附带时手动放进来即可：

```
packaging/webview2/MicrosoftEdgeWebview2Setup.exe
```

获取方式（任选其一）：

1. 官方短链（与程序「首次使用引导」里的下载页同址，约 2 MB）：
   <https://go.microsoft.com/fwlink/p/?LinkId=2124703>
2. 命令行（本机有网络时）：
   ```powershell
   Invoke-WebRequest -Uri 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' `
                     -OutFile 'packaging\webview2\MicrosoftEdgeWebview2Setup.exe'
   ```

放好之后的行为（都由 `source\tools\package.ps1` + `packaging\aiwrite.iss` 决定）：

- payload 会多出 `webview2\MicrosoftEdgeWebview2Setup.exe`，安装包装进 `{app}\webview2\`；
- 安装向导最后一步出现**可选**勾选项「安装网页版站点所需的 Edge WebView2 运行时（需联网）」
  （默认**不勾选** ⇒ 绝不静默安装第三方组件）；
- 程序「首次使用引导」在检测到依赖缺失时，会多出一个「安装 WebView2 运行时（随包附带）」按钮
  （判据 = 该文件确实存在于 `<exe 目录>\webview2\`，见 `utils/webview2_runtime.h`）。

**没有**这个文件时的行为：安装包照常生成（`.iss` 里那行带 `skipifsourcedoesntexist`），
程序与文档都只给官方下载页 —— 不会假装「已附带」。

> ⚠ 现状（2026-10-10 本机）：**未附带** —— 本机 winget 取不到网络（`0x80072efd`），
> 该 exe 未取得，安装包编译也未在本机验证（无 ISCC）。见 `docs/CHANGELOG.md` `[Unreleased]`。
