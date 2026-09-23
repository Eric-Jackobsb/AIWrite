# =============================================================================
#  build.ps1 —— 配置 + 编译（使用系统安装的 CMake + VS 2026 生成器）
#
#  用法：
#      .\build.ps1                      # Debug（默认）
#      .\build.ps1 -Config release      # RelWithDebInfo
#      .\build.ps1 -Reconfigure         # 删除 build 目录后重新配置
#      .\build.ps1 -Target aiwrite      # 只编译某个目标
#
#  产物：<AIWRITE_ROOT>/build/bin/{aiwrite,api_probe,webview2_login}.exe
# =============================================================================
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Config = 'debug',

    [switch]$Reconfigure,

    [string]$Target = ''
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'dev.ps1')

$sourceDir = $PSScriptRoot
$rootDir   = Split-Path -Parent $sourceDir
$buildDir  = Join-Path $rootDir 'build'
$binDir    = Join-Path $buildDir 'bin'

Push-Location $sourceDir
try {
    if ($Reconfigure -and (Test-Path $buildDir)) {
        Write-Host "[build] 删除旧构建目录: $buildDir" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $buildDir
    }

    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        Write-Host '[build] 配置 CMake (preset=default / VS 2026 x64) ...' -ForegroundColor Cyan
        cmake --preset default
        if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败 (exit=$LASTEXITCODE)" }
    }
    else {
        Write-Host "[build] 复用已有配置: $buildDir" -ForegroundColor DarkGray
    }

    Write-Host "[build] 编译 (preset=$Config) ..." -ForegroundColor Cyan
    if ($Target) {
        cmake --build --preset $Config --target $Target
    }
    else {
        cmake --build --preset $Config
    }
    if ($LASTEXITCODE -ne 0) { throw "编译失败 (exit=$LASTEXITCODE)" }

    Write-Host '[build] 编译成功，产物：' -ForegroundColor Green
    if (Test-Path $binDir) {
        Get-ChildItem $binDir -Filter '*.exe' | ForEach-Object {
            Write-Host ('   ' + $_.Name + '  ' + [math]::Round($_.Length / 1KB, 1) + ' KB')
        }
    }
}
finally {
    Pop-Location
}
