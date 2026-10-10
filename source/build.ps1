# =============================================================================
#  build.ps1 —— 配置 + 编译（使用系统安装的 CMake + VS 2026 生成器）
#
#  用法：
#      .\build.ps1                      # Debug（默认）
#      .\build.ps1 -Config release      # RelWithDebInfo
#      .\build.ps1 -Reconfigure         # 清空 build 目录后重新配置（**保留 build\logs\**）
#      .\build.ps1 -Target aiwrite      # 只编译某个目标
#      .\build.ps1 -LogName my.log      # 自定义日志文件名（仍落 build/logs/）
#      .\build.ps1 -NoLog               # 本次不写日志
#
#  产物：<AIWRITE_ROOT>/build/bin/{aiwrite,api_probe,webview2_login}.exe
#
#  日志：<AIWRITE_ROOT>/build/logs/build_<config>_<yyyyMMdd_HHmmss>.log
#        —— 控制台输出（dev.ps1 信息 + cmake / MSBuild 原生输出）逐行同时落盘，
#           UTF-8 无 BOM；`build\` 整体被 .gitignore 忽略 ⇒ 构建日志不入库。
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Config = 'debug',

    [switch]$Reconfigure,

    [string]$Target = '',

    [string]$LogName = '',

    [switch]$NoLog
)

$ErrorActionPreference = 'Stop'

$sourceDir = $PSScriptRoot
$rootDir   = Split-Path -Parent $sourceDir
$buildDir  = Join-Path $rootDir 'build'
$binDir    = Join-Path $buildDir 'bin'
$logDir    = Join-Path $buildDir 'logs'

# ----------------------------------------------------------------- 日志 --------
#  约定（2026-10-09）：构建日志**统一落 build\logs\**，不再散落在仓库根；
#    文件名 build_<config>_<yyyyMMdd_HHmmss>.log（-LogName 可自定义，-NoLog 关闭）。
#  为什么不用 Tee-Object：PS 5.1 的 Tee-Object **没有 -Encoding**，默认写 UTF-16LE，
#    会把日志写成 UTF-16（且与 UTF-8 行混排成乱码）⇒ 这里用 StreamWriter 显式 UTF-8 无 BOM。
$logPath = ''
$script:LogWriter  = $null
$script:StepExitCode = 0

function Write-BuildLog {
    param([string]$Text, [string]$Color = '')
    if ($Color) { Write-Host $Text -ForegroundColor $Color } else { Write-Host $Text }
    if ($script:LogWriter) { $script:LogWriter.WriteLine($Text) }
}

function Invoke-BuildStep {
    # 运行原生命令（cmake …）：逐行写日志，并把行**原样送到脚本 stdout**（与不做日志时
    # 的行为一致 ⇒ `> 文件` 重定向 与 VS Code 的 $msCompile 问题匹配器照常工作）；
    # 退出码经 $script:StepExitCode 回传（不能用 return，否则 stdout 行会被当成返回值）。
    param([Parameter(Mandatory)][scriptblock]$Step)
    $prevEap = $ErrorActionPreference
    # 临时降级：PS 5.1 下 `2>&1 |` 管道会把原生 stderr 当终止性错误抛出（实测）
    $ErrorActionPreference = 'Continue'
    try {
        & $Step 2>&1 | ForEach-Object {
            $line = [string]$_
            Write-Output $line
            if ($script:LogWriter) { $script:LogWriter.WriteLine($line) }
        }
    }
    finally { $ErrorActionPreference = $prevEap }
    $script:StepExitCode = $LASTEXITCODE
}

if (-not $NoLog) {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    if (-not $LogName) { $LogName = 'build_{0}_{1}.log' -f $Config, (Get-Date -Format 'yyyyMMdd_HHmmss') }
    elseif ($LogName -notlike '*.log') { $LogName += '.log' }
    $logPath = Join-Path $logDir $LogName
    $script:LogWriter = New-Object System.IO.StreamWriter($logPath, $false, (New-Object System.Text.UTF8Encoding($false)))
    $script:LogWriter.AutoFlush = $true
    $targetText = if ($Target) { $Target } else { '(all)' }
    Write-BuildLog ('# AIwrite build.ps1 · config={0} · target={1} · reconfigure={2}' -f $Config, $targetText, $Reconfigure.IsPresent) 'DarkGray'
    Write-BuildLog ('# start={0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) 'DarkGray'
    Write-BuildLog ('# log={0}' -f $logPath) 'DarkGray'
}

# dev.ps1 的信息行（Write-Host ⇒ 信息流 6）一并收进日志，保持「日志 = 整段控制台输出」
$devOut = & { . (Join-Path $sourceDir 'dev.ps1') } 6>&1
foreach ($line in $devOut) { Write-BuildLog ([string]$line) }

Push-Location $sourceDir
try {
    if ($Reconfigure -and (Test-Path $buildDir)) {
        Write-BuildLog "[build] 清空构建目录（保留 build\logs\）: $buildDir" 'Yellow'
        Get-ChildItem -LiteralPath $buildDir -Force | Where-Object { $_.Name -ne 'logs' } |
            Remove-Item -Recurse -Force
    }

    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        Write-BuildLog '[build] 配置 CMake (preset=default / VS 2026 x64) ...' 'Cyan'
        Invoke-BuildStep { cmake --preset default }
        if ($script:StepExitCode -ne 0) { throw "CMake 配置失败 (exit=$script:StepExitCode)" }
    }
    else {
        Write-BuildLog "[build] 复用已有配置: $buildDir" 'DarkGray'
    }

    Write-BuildLog "[build] 编译 (preset=$Config) ..." 'Cyan'
    if ($Target) {
        Invoke-BuildStep { cmake --build --preset $Config --target $Target }
    }
    else {
        Invoke-BuildStep { cmake --build --preset $Config }
    }
    if ($script:StepExitCode -ne 0) { throw "编译失败 (exit=$script:StepExitCode)" }

    Write-BuildLog '[build] 编译成功，产物：' 'Green'
    if (Test-Path $binDir) {
        Get-ChildItem $binDir -Filter '*.exe' | ForEach-Object {
            Write-BuildLog ('   ' + $_.Name + '  ' + [math]::Round($_.Length / 1KB, 1) + ' KB')
        }
    }
}
catch {
    Write-BuildLog ('[build] 失败: ' + $_.Exception.Message) 'Red'
    throw
}
finally {
    Pop-Location
    if ($logPath) { Write-BuildLog ("# end={0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) 'DarkGray' }
    if ($script:LogWriter) { $script:LogWriter.Flush(); $script:LogWriter.Dispose(); $script:LogWriter = $null }
    if ($logPath) { Write-Host "[build] 日志: $logPath" -ForegroundColor DarkGray }
}
