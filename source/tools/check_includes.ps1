# ============================================================================
#  check_includes.ps1 —— 依赖单向断言（ARCH-A4 · ARCHITECTURE.md §1.1 规则 1 / §4 判据 5）
#
#  判据（本脚本 = 唯一执行者）：
#    * **R1 能力层不得反向依赖界面层**：`engine/` `nodes/` `ai/` `web/` `utils/`
#      的源文件（*.h / *.cpp）**禁止** include 归属 `ui/` 或 `app/` 的头文件。
#    * **R2 编排层不得反向依赖入口层**：`ui/` 的源文件**禁止** include 归属 `app/` 的头文件
#      （唯一合法方向：app → ui → engine → ai/web/nodes/utils）。
#    * `app/` 可依赖所有层；`third_party/` 只作为「头文件归属表」来源，不参与断言。
#
#  头文件归属如何判定（本项目 include 一律写**裸文件名**，如 `#include "graph.h"`）：
#    * 先给所有层内 `.h` 建「文件名 → 所属层」表（跨层同名 = 归属不明 ⇒ 判失败，防止静默放行）；
#    * include 目标带路径时（`ui/x.h` / `..\ui\x.h`），首段是层名则直接按层归属；
#    * 否则按裸文件名查表；查不到（系统头 / 未扫描的第三方头）计入「未归属」但不判失败。
#
#  纪律（AI_README §3）：
#    * 只读、零副作用；退出码 1 = 有违规（逐条 `文件:行` 打印）
#    * 结果行固定含 `checked files:` / `violations:`，供 check_all 抓取摘要
#
#  用法：
#    powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_includes.ps1
#       [-SourceDir <仓库的 source 目录>]     默认由脚本位置推导（source\tools\.. = source）
# ============================================================================
param(
    [string]$SourceDir = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($SourceDir)) {
    $SourceDir = Split-Path -Parent $PSScriptRoot      # source\tools\.. = source
}
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path

# ---- 层定义：Forbidden = 本层禁止反向依赖的层 ------------------------------
#  顺序即输出顺序；新增层时**只需加一行**（判据与文档同步在 ARCHITECTURE.md §1.1）。
$layers = [ordered]@{
    'app'    = @{ Forbidden = @() }                  # 入口层：窗口 / 主循环 / 启动组装
    'ui'     = @{ Forbidden = @('app') }             # 编排层：禁止 ui → app
    'engine' = @{ Forbidden = @('ui', 'app') }       # 领域层
    'nodes'  = @{ Forbidden = @('ui', 'app') }       # 能力层
    'ai'     = @{ Forbidden = @('ui', 'app') }       # 能力层
    'web'    = @{ Forbidden = @('ui', 'app') }       # 能力层
    'utils'  = @{ Forbidden = @('ui', 'app') }       # 能力层
}

function Write-Line([string]$text) { Write-Host $text }

Write-Line '# check_includes.ps1 · 依赖单向断言（规则 1 / 判据 5）'
Write-Line "# source 目录: $SourceDir"
Write-Line '# 禁止：engine/nodes/ai/web/utils → ui|app · ui → app'
Write-Line ''

# ---- ① 头文件归属表（文件名 → 层）；跨层同名视为归属不明 ----------------
$headerOwner      = @{}     # 裸文件名（小写）→ 层
$ambiguousHeaders = @()     # 跨层同名（判失败）

$ownerSources = @($layers.Keys) + @('third_party')
foreach ($layer in $ownerSources) {
    $layerDir = Join-Path $SourceDir $layer
    if (-not (Test-Path -LiteralPath $layerDir)) { continue }
    $headers = @(Get-ChildItem -LiteralPath $layerDir -Recurse -File -Filter '*.h' -ErrorAction SilentlyContinue)
    foreach ($h in $headers) {
        $key = $h.Name.ToLowerInvariant()
        if ($headerOwner.ContainsKey($key)) {
            $prev = $headerOwner[$key]
            if ($prev -ne $layer) { $ambiguousHeaders += "$($h.Name) → $prev / $layer" }
        }
        else {
            $headerOwner[$key] = $layer
        }
    }
}

# ---- ② 逐层扫描 include ------------------------------------------------
$includePattern = [regex]'^\s*#\s*include\s+"([^"]+)"'
$violations     = New-Object System.Collections.ArrayList
$unresolved     = New-Object System.Collections.ArrayList
$totalFiles     = 0
$totalIncludes  = 0

Write-Line ('{0,-9} {1,7} {2,9} {3,8}' -f '层', '文件', 'include', '违规')
Write-Line ('{0,-9} {1,7} {2,9} {3,8}' -f '--------', '------', '--------', '------')

foreach ($layer in $layers.Keys) {
    $layerDir = Join-Path $SourceDir $layer
    if (-not (Test-Path -LiteralPath $layerDir)) { continue }
    $forbidden = @($layers[$layer].Forbidden)

    $files        = @(Get-ChildItem -LiteralPath $layerDir -Recurse -File -Include '*.h', '*.cpp' -ErrorAction SilentlyContinue)
    $layerFiles   = 0
    $layerInclude = 0
    $layerBad     = 0

    foreach ($file in $files) {
        $layerFiles++
        $rel    = $file.FullName.Substring($SourceDir.Length + 1)
        $lineNo = 0
        foreach ($line in (Get-Content -LiteralPath $file.FullName -Encoding UTF8)) {
            $lineNo++
            $m = $includePattern.Match($line)
            if (-not $m.Success) { continue }
            $layerInclude++
            $target   = $m.Groups[1].Value.Replace('\', '/')
            $segments = $target.Split('/')

            # 归属判定：带路径且首段是层名 → 按层；否则按裸文件名查表
            $owner = $null
            if ($segments.Count -gt 1 -and $layers.Contains($segments[0])) {
                $owner = $segments[0]
            }
            else {
                $base = $segments[$segments.Count - 1].ToLowerInvariant()
                if ($headerOwner.ContainsKey($base)) { $owner = $headerOwner[$base] }
            }

            if ($null -eq $owner) {
                [void]$unresolved.Add(('{0}:{1}  #include "{2}"' -f $rel, $lineNo, $target))
                continue
            }
            if ($forbidden -contains $owner) {
                $layerBad++
                [void]$violations.Add(('{0}:{1}  #include "{2}"  ⇒ 归属 {3}（{4} 禁止依赖 {3}）' -f `
                            $rel, $lineNo, $target, $owner, $layer))
            }
        }
    }

    $totalFiles    += $layerFiles
    $totalIncludes += $layerInclude
    Write-Line ('{0,-9} {1,7} {2,9} {3,8}' -f $layer, $layerFiles, $layerInclude, $layerBad)
}

# ---- ③ 结论 ------------------------------------------------------------
Write-Line ''

if ($ambiguousHeaders.Count -gt 0) {
    Write-Line '头文件跨层同名（归属不明 ⇒ 失败，请改名或移动）：'
    foreach ($a in ($ambiguousHeaders | Sort-Object -Unique)) { Write-Line "  ! $a" }
    Write-Line ''
}

if ($violations.Count -gt 0) {
    Write-Line "违规明细（$($violations.Count) 条）："
    foreach ($v in $violations) { Write-Line "  x $v" }
    Write-Line ''
}

if ($unresolved.Count -gt 0) {
    Write-Line "未归属 include（系统头 / 未扫描的第三方头，不计失败）：$($unresolved.Count) 条"
    foreach ($u in $unresolved) { Write-Line "  - $u" }
    Write-Line ''
}

$failed = ($violations.Count -gt 0) -or ($ambiguousHeaders.Count -gt 0)
Write-Line ('checked files: {0}   include lines: {1}   violations: {2}' -f $totalFiles, $totalIncludes, $violations.Count)
if ($failed) {
    Write-Line '结果: FAIL（依赖单向被破坏）'
    exit 1
}
Write-Line '结果: PASS（engine/nodes/ai/web/utils 不含 ui|app 头 · ui 不含 app 头）'
exit 0
