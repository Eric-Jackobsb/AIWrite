# =============================================================================
#  dev.ps1 —— AIwrite 开发环境初始化（仅作用于当前 PowerShell 会话）
#
#  用法： .\dev.ps1     （build.ps1 会自动调用）
#
#  作用：
#      1) 设置 vcpkg 环境变量（缓存与依赖安装树全部在 F: 盘）
#      2) 确认 CMake 可用：优先使用系统安装版，缺失时回退到工程自带的 3.31.6
#
#  说明：构建使用 “Visual Studio 18 2026” 生成器，MSVC 环境由 CMake 自动处理，
#        因此**不需要** Developer Command Prompt / vcvars64。
# =============================================================================

$ErrorActionPreference = 'Stop'

$script:SourceDir   = $PSScriptRoot
$script:AiwriteRoot = Split-Path -Parent $script:SourceDir

$VcpkgRoot   = 'C:\dev\vcpkg'
$VcpkgCache  = Join-Path $script:AiwriteRoot 'vcpkg-cache'
$PinnedCmake = Join-Path $script:AiwriteRoot 'third_party\cmake-3.31.6\bin'

# --- vcpkg 环境（工具在 C:，缓存/安装树在 F:）-------------------------------
$env:VCPKG_ROOT                 = $VcpkgRoot
$env:VCPKG_DOWNLOADS            = Join-Path $VcpkgCache 'downloads'
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $VcpkgCache 'binary'
$env:VCPKG_INSTALLED_DIR        = Join-Path $script:AiwriteRoot 'vcpkg-installed'
$env:VCPKG_DEFAULT_TRIPLET      = 'x64-windows'
$env:VCPKG_DEFAULT_HOST_TRIPLET = 'x64-windows'

foreach ($d in @($env:VCPKG_DOWNLOADS, $env:VCPKG_DEFAULT_BINARY_CACHE, $env:VCPKG_INSTALLED_DIR)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
}

# --- CMake 解析 -------------------------------------------------------------
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake -and (Test-Path $PinnedCmake)) {
    $env:PATH = "$PinnedCmake;$env:PATH"
    Write-Host '[dev] 未检测到系统 CMake，回退到工程自带 third_party/cmake-3.31.6' -ForegroundColor Yellow
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
}

Write-Host ('[dev] AIWRITE_ROOT    = ' + $script:AiwriteRoot)
Write-Host ('[dev] cmake           = ' + ($cmake | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue))
Write-Host ('[dev] VCPKG_ROOT      = ' + $env:VCPKG_ROOT)
Write-Host ('[dev] VCPKG_INSTALLED = ' + $env:VCPKG_INSTALLED_DIR)
Write-Host ('[dev] 构建目录        = ' + (Join-Path $script:AiwriteRoot 'build') + '（生成器 Visual Studio 18 2026 / x64）')
