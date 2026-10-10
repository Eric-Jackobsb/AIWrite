# ============================================================================
#  package.ps1 —— 一条命令产出「可安装包」（M6 · 方向 C）
#
#  做什么：①（可选）构建 → ② 收集 payload（build\dist\payload）→ ③ 调 Inno Setup 编译安装包
#
#  关键口径（刻意如此，别"顺手优化"）：
#   * **payload 不含 Python**：排除 build\bin\python\**（M8 §0「零 Python 运行时」方向；
#     pydoll 通道已退场）。因此安装版**没有** `--pydoll-*` 能力 —— 如需开发态自检请直接用 build\bin。
#   * **安装包不下载、不安装 WebView2**：只在程序首启引导里给官方下载页（不静默装依赖）。
#   * **版本号单一来源** = `source/utils/include/app_version.h` 的 `kAppVersion`：
#     本脚本的 -Version 默认值与它保持字面一致；**改版本号属 AI_README §6 动作，需开发者明确指令**。
#   * payload 阶段**完全离线可验证**（不需要 Inno Setup）：产物 = 目录 + manifest.txt。
#
#  用法：
#    powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\package.ps1
#        [-Version 0.2.0] [-SkipBuild] [-SkipInstaller] [-Configuration debug]
#
#  退出码：0 = payload + 安装包都成功 ｜ 2 = 只完成 payload（未找到 ISCC，已打印获取方式）｜ 1 = 失败
# ============================================================================
param(
    [string]$Version = '0.2.0',
    [string]$Configuration = 'debug',
    [switch]$SkipBuild,
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'

$repoRoot  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceDir = Join-Path $repoRoot 'source'
$binDir    = Join-Path $repoRoot 'build\bin'
$distDir   = Join-Path $repoRoot 'build\dist'
$payload   = Join-Path $distDir 'payload'
$issFile   = Join-Path $repoRoot 'packaging\aiwrite.iss'
$logDir    = Join-Path $repoRoot 'build\logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

$stamp   = Get-Date -Format 'yyyyMMdd_HHmmss'
$logPath = Join-Path $logDir "package_$stamp.log"
$writer  = New-Object System.IO.StreamWriter($logPath, $false, (New-Object System.Text.UTF8Encoding $false))
$writer.AutoFlush = $true
function Write-Both([string]$text) { Write-Host $text; $writer.WriteLine($text) }

Write-Both "# package.ps1 · $stamp"
Write-Both "# 版本: $Version （口径：source/utils/include/app_version.h::kAppVersion）"
Write-Both "# payload: $payload"
Write-Both "# 日志: $logPath"

# ---- ① 构建（默认做；-SkipBuild 复用现有 build\bin）----
if (-not $SkipBuild) {
    Write-Both "`n== ① 构建（$Configuration） =="
    Push-Location $sourceDir
    $buildOut = Join-Path $logDir "package_$stamp`_build.txt"
    $buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $sourceDir 'build.ps1'))
    if ($Configuration -ne 'debug') { $buildArgs += @('-Config', $Configuration) }
    & powershell @buildArgs *> $buildOut
    $buildCode = $LASTEXITCODE
    Pop-Location
    if ($buildCode -ne 0) {
        Write-Both "构建失败（exit=$buildCode）；详见 $buildOut"
        $writer.Close(); exit 1
    }
    $buildText = Get-Content $buildOut -Raw
    $errCount  = ([regex]::Matches($buildText, 'error C\d+|error LNK\d+|error MSB\d+')).Count
    $warnCount = ([regex]::Matches($buildText, 'warning C\d+')).Count
    Write-Both "   PASS  构建完成：error=$errCount · warning=$warnCount（日志 $buildOut）"
    if ($errCount -ne 0) { Write-Both '   构建有 error ⇒ 停止打包'; $writer.Close(); exit 1 }
} else {
    Write-Both "`n== ① 构建：已跳过（-SkipBuild） =="
}

# ---- ② 必需产物自检（缺件就明确报错，不产出半成品安装包）----
Write-Both "`n== ② 检查 build\bin 产物 =="
$required = @('aiwrite.exe', 'api_probe.exe', 'WebView2Loader.dll', 'glfw3.dll', 'assets\providers.json')
$missing  = @()
foreach ($item in $required) {
    if (-not (Test-Path (Join-Path $binDir $item))) { $missing += $item }
}
if ($missing.Count -gt 0) {
    Write-Both ("   缺少产物：" + ($missing -join '、') + "`n   先跑 source\build.ps1 再打包。")
    $writer.Close(); exit 1
}
$dllCount = @(Get-ChildItem (Join-Path $binDir '*.dll')).Count
Write-Both "   PASS  aiwrite.exe / api_probe.exe / assets\providers.json 就绪；运行时 DLL $dllCount 个"
if ($Configuration -eq 'debug') {
    Write-Both "   WARN  当前是 **Debug** 产物：payload 会带上 *d.dll（调试版运行时）⇒ 仅供本机验证，"
    Write-Both "         对外分发请改用：package.ps1 -Configuration release（会重新构建 release 产物）"
}


# ---- ③ 收集 payload（幂等：每次都重建 payload 目录）----
Write-Both "`n== ③ 收集 payload =="
if (Test-Path $payload) { Remove-Item -Recurse -Force $payload }
New-Item -ItemType Directory -Path $payload -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $payload 'assets') -Force | Out-Null

# 3.1 可执行 + 运行时 DLL（**刻意不含** webview2_login.exe：M1 技术验证工具，不是产品件）
Copy-Item (Join-Path $binDir 'aiwrite.exe')   $payload -Force
Copy-Item (Join-Path $binDir 'api_probe.exe') $payload -Force
Get-ChildItem (Join-Path $binDir '*.dll') | ForEach-Object { Copy-Item $_.FullName $payload -Force }

# 3.2 站点资产（providers.json 是唯一权威位；assets\ 下的其它 json 一并带上）
Get-ChildItem (Join-Path $binDir 'assets\*') -Recurse | Where-Object { -not $_.PSIsContainer } |
    ForEach-Object {
        $rel  = $_.FullName.Substring((Join-Path $binDir 'assets').Length).TrimStart('\')
        $dest = Join-Path $payload "assets\$rel"
        $destDir = Split-Path -Parent $dest
        if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
        Copy-Item $_.FullName $dest -Force
    }

# 3.2b `PQ-02` 可选件：随包 WebView2 Bootstrapper（有就带上，没有不是错误）
$bootDir = Join-Path $repoRoot 'packaging\webview2'
$bootExe = Join-Path $bootDir 'MicrosoftEdgeWebview2Setup.exe'
if (Test-Path $bootExe) {
    New-Item -ItemType Directory -Path (Join-Path $payload 'webview2') -Force | Out-Null
    Copy-Item $bootExe (Join-Path $payload 'webview2') -Force
    $bootMb = (Get-Item $bootExe).Length / 1MB
    Write-Both ("   PASS  PQ-02 随包附带 WebView2 Bootstrapper（{0:N1} MB）" -f $bootMb)
} else {
    Write-Both "   WARN  PQ-02 未附带 WebView2 Bootstrapper（packaging\webview2\ 里没有该 exe）"
    Write-Both "         ⇒ 安装包照常生成；界面回落到官方下载页。获取方式见 packaging\webview2\README.md"
}

# 3.3 **Python 摘钩**断言：payload 里绝不允许出现 python 目录 / *.py
$pythonLeak = @(Get-ChildItem $payload -Recurse -File -Filter '*.py' -ErrorAction SilentlyContinue)
$pythonDir  = Test-Path (Join-Path $payload 'python')
if ($pythonDir -or $pythonLeak.Count -gt 0) {
    Write-Both "   FAIL  payload 里出现了 Python 产物（dir=$pythonDir / *.py=$($pythonLeak.Count)）—— 安装包口径要求零 Python"
    $writer.Close(); exit 1
}
Write-Both "   PASS  Python 摘钩：payload 内 *.py = 0、python\ 目录 = 不存在"

# 3.4 文件清单 + 体积（取证：manifest 随 payload 一起进安装包/留档）
$manifest = Join-Path $distDir 'manifest.txt'
$files    = Get-ChildItem $payload -Recurse -File | Sort-Object FullName
$lines    = New-Object System.Collections.ArrayList
[void]$lines.Add("# AIwrite payload manifest · 生成 $stamp · 版本 $Version")
[void]$lines.Add("# 口径：不含 Python（M8 §0）；不对 WebView2 做静默安装")
$totalBytes = 0
foreach ($f in $files) {
    $rel = $f.FullName.Substring($payload.Length).TrimStart('\')
    $totalBytes += $f.Length
    [void]$lines.Add(("{0,10}  {1}" -f $f.Length, $rel))
}
[void]$lines.Add(("# 合计 {0} 个文件 / {1:N1} MB" -f $files.Count, ($totalBytes / 1MB)))
[System.IO.File]::WriteAllLines($manifest, $lines, (New-Object System.Text.UTF8Encoding $false))
Write-Both ("   PASS  payload：{0} 个文件 / {1:N1} MB（清单 {2}）" -f $files.Count, ($totalBytes / 1MB), $manifest)
foreach ($f in $files) {
    $rel = $f.FullName.Substring($payload.Length).TrimStart('\')
    Write-Both ("         {0,10}  {1}" -f $f.Length, $rel)
}

# ---- ③.5 `.iss` ↔ payload 一致性（离线锁）----
#  背景（如实说明）：本机**没有 ISCC**（winget 亦无网络：0x80072efd）⇒ 安装包编译无法在本机验证。
#  能离线做的是：把 `.iss` 里每条 `Source:` 通配**逐条**对到 payload 上 —— 只要有人在 payload 里
#  新增子目录却忘了改 `.iss`（或反过来），这里立刻 FAIL，避免"注释说打了、实际没进包"。
Write-Both "`n== ③.5 .iss ↔ payload 一致性（离线锁）=="
$issText  = Get-Content $issFile -Raw
$patterns = [regex]::Matches($issText, 'Source:\s*"\{\#PayloadDir\}\\([^"]+)"') |
    ForEach-Object { $_.Groups[1].Value }
if ($patterns.Count -eq 0) {
    Write-Both "   FAIL  .iss 里没解析到任何 Source: 行（脚本被改坏了？）"
    $writer.Close(); exit 1
}
$issOk = $true
foreach ($pattern in $patterns) {
    $glob = Join-Path $payload $pattern
    $hits = @(Get-ChildItem $glob -Recurse -File -ErrorAction SilentlyContinue)
    if ($hits.Count -eq 0) {
        # `webview2\...` 是 `PQ-02` **可选件**（.iss 里带 skipifsourcedoesntexist）⇒ 缺席只告警
        if ($pattern -like 'webview2*') {
            Write-Both ("   WARN  {0} ⇒ 0 个文件（PQ-02 可选件未附带，.iss 已 skipifsourcedoesntexist）" -f $pattern)
        }
        else {
            Write-Both ("   FAIL  .iss 的 Source 通配 `{#PayloadDir}\{0}` 在 payload 里**一个文件都匹配不到**" -f $pattern)
            $issOk = $false
        }
    }
    else {
        Write-Both ("   PASS  {0} ⇒ {1} 个文件" -f $pattern, $hits.Count)
    }
}
# 反向：payload 顶层是否有 `.iss` 未覆盖的**文件类型**（漏打包检测）
$covered = @{}
foreach ($pattern in $patterns) {
    if ($pattern -match '^\*(\.\w+)$') { $covered[$Matches[1]] = $true }
}
foreach ($top in Get-ChildItem $payload -File) {
    if ($top.Extension -ne '' -and -not $covered.ContainsKey($top.Extension)) {
        Write-Both ("   FAIL  payload 顶层 {0} 的扩展名 {1} 不在 .iss 的 Source 里 ⇒ 装包会漏" -f $top.Name, $top.Extension)
        $issOk = $false
    }
}
if (-not $issOk) { Write-Both '   .iss 与 payload 不一致 ⇒ 停止（先改 packaging\aiwrite.iss）'; $writer.Close(); exit 1 }
Write-Both ("   PASS  .iss 与 payload 一致（{0} 条 Source 通配）" -f $patterns.Count)

# ---- ④ 编译安装包（Inno Setup）----
if ($SkipInstaller) {
    Write-Both "`n== ④ 安装包：已跳过（-SkipInstaller） =="
    $writer.Close(); exit 2
}

Write-Both "`n== ④ 编译安装包（Inno Setup / ISCC） =="
$iscc = $null
foreach ($candidate in @(
        "$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
    if ($candidate -and (Test-Path $candidate)) { $iscc = $candidate; break }
}
if (-not $iscc) {
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { $iscc = $cmd.Source }
}
if (-not $iscc) {
    Write-Both "   未找到 ISCC.exe ⇒ **payload 阶段已完成**，安装包未编译。"
    Write-Both "   获取 Inno Setup 6（任选其一）："
    Write-Both "     winget install --id JRSoftware.InnoSetup -e"
    Write-Both "     或到 https://jrsoftware.org/isdl.php 下载安装（默认装到 Program Files (x86)\Inno Setup 6）"
    Write-Both "   装好后重跑本脚本（可加 -SkipBuild 复用现有产物）。"
    $writer.Close(); exit 2
}

$isccLog = Join-Path $logDir "package_$stamp`_iscc.txt"
& $iscc "/DMyAppVersion=$Version" "/DPayloadDir=$payload" "/DOutDir=$distDir" $issFile *> $isccLog
$isccCode = $LASTEXITCODE
$installer = Join-Path $distDir "AIwrite-Setup-$Version.exe"
if ($isccCode -ne 0 -or -not (Test-Path $installer)) {
    Write-Both "   FAIL  ISCC 编译失败（exit=$isccCode）；详见 $isccLog"
    Get-Content $isccLog -Tail 20 | ForEach-Object { Write-Both ("         " + $_) }
    $writer.Close(); exit 1
}
$size = (Get-Item $installer).Length / 1MB
Write-Both ("   PASS  安装包已生成：{0}（{1:N1} MB）" -f $installer, $size)
Write-Both "`n=== package 汇总：payload OK · 安装包 OK ==="
Write-Both "日志：$logPath"
$writer.Close()
exit 0
