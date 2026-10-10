; ============================================================================
;  AIwrite 安装包脚本（Inno Setup 6）—— M6 收口 `RM-D5`（方向 C：自建站点闭环 + 安装包）
;
;  编译方式（**不要手改这里传参**）：
;      cd F:\GameDao\Tools\AIwrite
;      powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\package.ps1
;  脚本会：① 构建 ② 收集 **payload**（build\dist\payload）③ 调 ISCC 编译本脚本。
;
;  设计约束（都是**刻意**的）：
;   * **不含 Python 运行时**：`build\bin\python\**` 被 package.ps1 排除（M8 §0「零 Python 运行时」
;     是长期方向；pydoll 通道已退场 ⇒ 安装包不带解释器）。装包体积与启动时间都靠这条守住。
;   * **不碰用户数据**：`%USERPROFILE%\.brain-ai\**`（配置 / 凭据 / 登录态 / 图片）由程序自己管，
;     安装与卸载**都不删除**（卸载只移除 {app} 目录）—— 见 AI_README §7 第 5 条红线。
;   * **版本号单一来源** = `source/utils/include/app_version.h` 的 `kAppVersion`
;     ⇒ 这里只接受 `/DMyAppVersion=...`（package.ps1 自动传），默认值与头文件保持字面一致。
;   * 依赖（WebView2 运行时）**不打包、不静默安装**：由程序首启引导提示用户去官方下载页。
; ============================================================================

#ifndef MyAppVersion
  #define MyAppVersion "0.2.0"
#endif

; payload 目录（相对本 .iss 所在目录；package.ps1 会显式传 /DPayloadDir）
#ifndef PayloadDir
  #define PayloadDir "..\build\dist\payload"
#endif
#ifndef OutDir
  #define OutDir "..\build\dist"
#endif

#define MyAppName "AIwrite"
#define MyAppExeName "aiwrite.exe"

[Setup]
AppId={{8F2C41D6-7A93-4B5E-9C21-6D3E5A7B9F04}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppName}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
OutputDir={#OutDir}
OutputBaseFilename=AIwrite-Setup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 允许**非管理员**安装（装到用户目录）；管理员也可用同一安装包
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayName={#MyAppName} {#MyAppVersion}

[Languages]
; 刻意只用 Inno **自带**的语言文件：`ChineseSimplified.isl` 属"Inno Setup Unofficial languages"，
; 默认安装**不含** ⇒ 写上会让 ISCC 直接报 Unknown language file（无法在本机验证的东西不写进脚本）。
; 若已装非官方语言包，按下面一行加中文向导界面即可：
;   Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "default"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: unchecked

[Files]
; 主程序 + 诊断工具（api_probe.exe：支持人员可让用户跑 `--diag-dump` 取环境报告）
Source: "{#PayloadDir}\*.exe"; DestDir: "{app}"; Flags: ignoreversion
; 运行时依赖 DLL（glfw3 / spdlog / openssl / WebView2Loader 等）
Source: "{#PayloadDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
; 随程序发布的站点资产（providers.json 是站点数据的唯一权威位）
Source: "{#PayloadDir}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
; `PQ-02`（已拍板 2026-10-09）：安装包**附带** Edge WebView2 Bootstrapper —— 但它是**可选件**：
;   * 把 `MicrosoftEdgeWebview2Setup.exe` 放到 `packaging\webview2\` ⇒ package.ps1 带进 payload ⇒ 随包安装
;   * 没有该文件 ⇒ `skipifsourcedoesntexist` 让本行**静默跳过**（安装包照常生成，界面回落到官方下载页）
;   * 本行**不**代表已附带：本机无网络（winget `0x80072efd`）⇒ 该文件未在本机取得，如实登记
Source: "{#PayloadDir}\webview2\MicrosoftEdgeWebview2Setup.exe"; DestDir: "{app}\webview2"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent
; 可选：一键安装网页版站点所需的依赖（**默认不勾选** ⇒ 绝不静默安装第三方组件）
;  * `Check: BootstrapperExists` 保证「随包没有该文件时」这一项不出现（见下方 [Code]）
Filename: "{app}\webview2\MicrosoftEdgeWebview2Setup.exe"; \
    Description: "安装网页版站点所需的 Edge WebView2 运行时（需联网）"; \
    Flags: postinstall nowait skipifsilent unchecked; Check: BootstrapperExists

; 说明：卸载由 Inno 自动移除本安装包写入的文件（{app} 下），
;      **不删**用户数据 `%USERPROFILE%\.brain-ai\**`（配置 / 凭据 / 登录态 / 图片）
;      ⇒ 刻意**不用** [UninstallDelete] 通配删除，避免误伤

[Code]
// 安装完成后的一句话提示：不在这里判断 / 安装 WebView2 —— 由程序首启引导给出官方下载页
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    // 纯提示（不弹窗、不阻塞）：写进安装日志，便于支持人员核对
    Log('AIwrite {#MyAppVersion} 安装完成；依赖（Edge WebView2 运行时）由程序首启引导检查。');
  end;
end;

// 随包是否带了 Bootstrapper（决定 [Run] 里那一条是否出现）
// 说明：本函数是 Inno 文档里的标准写法；⚠ **本机未编译验证**（无 ISCC/无网络），见 docs/CHANGELOG
function BootstrapperExists(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\webview2\MicrosoftEdgeWebview2Setup.exe'));
end;
