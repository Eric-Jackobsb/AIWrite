# ============================================================================
#  check_globals.ps1 —— 跨帧可变状态断言（ARCH-A5 · ARCHITECTURE.md §1.1 规则 6 / §4 判据 2）
#
#  判据（本脚本 = 唯一执行者）：
#    * **R1 跨 TU extern**：任何层 `*.h` 在命名空间作用域出现 `extern <类型> <名>;`
#      （函数 / `extern "C"` 除外）= 多个 TU 共享的可变状态 ⇒ 违规。
#    * **R2 文件作用域可变（内部链接）**：`*.cpp` / `*.h` 在命名空间作用域（含匿名
#      `namespace { … }` 内）定义的**可变**变量（无名空间成员 = 内部链接，等价文件级 `static`）⇒ 违规。
#    * **R3 文件作用域可变（外部链接）**：命名空间作用域、非 `static`、非匿名空间、非 const
#      的可变定义 = 真正的外部链接全局 ⇒ 违规（跨 TU 可见，最危险）。
#
#  不计入（规则 6 的合法形态）：
#    * 类 / 结构体 / 联合体 / 枚举**成员** —— `TextureCache` 这类对象成员天然合规（无例外条款）；
#    * `const` / `constexpr` / `consteval` / `constinit` 常量表（只读，不可变）；
#    * 函数体内的 `static` 局部变量（函数作用域：不判**文件级**违规，但计入
#      **函数内 static 棘轮** —— 见 `-MaxFunctionStatics`，只许减）；
#    * 函数 / 类型 / 别名 / 模板 / `using` / `typedef` / 预处理行 / 前向声明。
#
#  纪律（AI_README §3）：
#    * 只读、零副作用；退出码 1 = 有违规（逐条 `文件:行  声明` 打印）
#    * 结果行固定含 `checked files:` / `violations:`，供 check_all 抓取摘要
#
#  用法：
#    powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_globals.ps1
#       [-SourceDir <仓库的 source 目录>]   默认由脚本位置推导（source\tools\.. = source）
#       [-Layers ui,app]                    只扫指定层（默认全部 7 层）
#       [-SummaryOnly]                      只打印汇总表，不逐条列出违规
#       [-MaxFunctionStatics <n>]           函数内 static 上限（默认见 `$script:FnStaticCap`）
#       [-Baseline <基线文件>]              遗留能力：棘轮模式（ARCH-A5 引入）—— 存量归零后
#                                           已不再使用（基线文件已删除）；保留给将来的大批量迁移
#       [-Baseline <基线文件> -UpdateBaseline]
#                                           重写基线（**只许减**）
#     不带 -Baseline ⇒ **严格零容忍**（现用形态：文件级可变状态必须恒为 0）
# ============================================================================
param(
    [string]$SourceDir = '',
    [string[]]$Layers = @(),
    [string]$Baseline = '',
    [switch]$UpdateBaseline,
    [switch]$SummaryOnly,
    [int]$MaxFunctionStatics = -1
)

# 函数内 static 棘轮上限（ARCH-A7 收口时实测值 · **只许减**）：
#  * 这些是「函数作用域单实例」的合法形态（访问器 / 惰性初始化 / 纯缓存），但**不允许再长**——
#    否则「文件级归零、函数内继续长」就绕开了规则 6 的机器判据（A7 审计 §2 记录）。
#  * 减少后请同步下调本数字（或显式传 `-MaxFunctionStatics <n>`）。
$script:FnStaticCap = 51

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($SourceDir)) {
    $SourceDir = Split-Path -Parent $PSScriptRoot      # source\tools\.. = source
}
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path

$allLayers  = @('app', 'ui', 'engine', 'nodes', 'ai', 'web', 'utils')
$scanLayers = if ($Layers.Count -gt 0) { @($Layers) } else { $allLayers }
foreach ($l in $scanLayers) {
    if ($allLayers -notcontains $l) { throw "未知层：$l（合法值：$($allLayers -join ' / ')）" }
}

function Write-Line([string]$text) { Write-Host $text }

Write-Line '# check_globals.ps1 · 跨帧可变状态断言（规则 6 / 判据 2）'
Write-Line "# source 目录: $SourceDir"
Write-Line '# 违规：R1 头文件 extern 变量 · R2 文件作用域 static（含匿名 namespace）· R3 外部链接全局'
Write-Line ''

# ---- 声明分类 --------------------------------------------------------------
$reNamespace = [regex]'^\s*(inline\s+)?namespace(\s+[\w:]+)?\s*\{'
$reAnonNs    = [regex]'^\s*(inline\s+)?namespace\s*\{'
$reExternVar = [regex]'^\s*extern\s+(?!")[^\(\)]*;\s*$'
$reStaticVar = [regex]'^\s*(static|thread_local)\s+'
$reConstDecl = [regex]'\b(const|constexpr|consteval|constinit)\b'
$reSkipStart = [regex]'^\s*(#|using\b|typedef\b|template\b|friend\b|static_assert\b|return\b|namespace\b|extern\s+"|struct\b|class\b|union\b|enum\b|public:|private:|protected:|\}|\{)'
$reFuncLike  = [regex]'^[^;=]*\([^;{}]*\)\s*(const\s*)?(noexcept\s*)?(override\s*)?(final\s*)?(->\s*[^;{}]+)?\s*[;{]?\s*$'

# ---- 词法预处理：去注释 / 去字符串与字符字面量（保持行号不变） --------------
function Get-CleanLines([string]$path) {
    $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
    $sb   = New-Object System.Text.StringBuilder($text.Length)
    $i = 0
    $n = $text.Length
    $inBlock = $false
    while ($i -lt $n) {
        $c  = $text[$i]
        $c2 = if ($i + 1 -lt $n) { $text[$i + 1] } else { [char]0 }
        if ($inBlock) {
            if ($c -eq '*' -and $c2 -eq '/') { $inBlock = $false; $i += 2; continue }
            if ($c -eq "`n") { [void]$sb.Append("`n") }
            $i++
            continue
        }
        if ($c -eq '/' -and $c2 -eq '/') {
            while ($i -lt $n -and $text[$i] -ne "`n") { $i++ }
            continue
        }
        if ($c -eq '/' -and $c2 -eq '*') { $inBlock = $true; $i += 2; continue }
        if ($c -eq 'R' -and $c2 -eq '"') {
            # 原始字符串 R"delim( … )delim"：整段替换为占位，但**保留内部换行**以维持行号
            $j     = $i + 2
            $delim = ''
            while ($j -lt $n -and $text[$j] -ne '(' -and $text[$j] -ne "`n") { $delim += $text[$j]; $j++ }
            if ($j -lt $n -and $text[$j] -eq '(') {
                $end      = $text.IndexOf(')' + $delim + '"', $j)
                $stopAt   = if ($end -ge 0) { $end + $delim.Length + 2 } else { $n }
                for ($k = $j; $k -lt $stopAt; $k++) { if ($text[$k] -eq "`n") { [void]$sb.Append("`n") } }
                [void]$sb.Append('""')
                $i = $stopAt
                continue
            }
        }
        if ($c -eq '"' -or $c -eq "'") {
            # C++14 数字分隔符（2'000 / 0x1'000）：不是字符字面量，直接跳过该撇号
            $prev = if ($i -gt 0) { $text[$i - 1] } else { [char]0 }
            if ($c -eq "'" -and $prev -match '[0-9A-Fa-f]' -and $c2 -match '[0-9A-Fa-f]') { $i++; continue }
            $quote = $c
            $i++
            while ($i -lt $n) {
                if ($text[$i] -eq '\') { $i += 2; continue }
                if ($text[$i] -eq $quote) { $i++; break }
                if ($text[$i] -eq "`n") { break }        # 未闭合 / raw string：止于行尾
                $i++
            }
            [void]$sb.Append('""')                        # 占位：保持「有内容」但不含可疑文本
            continue
        }
        [void]$sb.Append($c)
        $i++
    }
    return ($sb.ToString() -split "`n")
}

function Get-ScopeKind([string]$prefix) {
    if ($reNamespace.IsMatch($prefix + '{')) {
        if ($reAnonNs.IsMatch($prefix + '{')) { return 'anon' }
        return 'ns'
    }
    return 'other'
}

# ---- 逐文件扫描 ------------------------------------------------------------
$violations = New-Object System.Collections.ArrayList   # @{Layer,File,Line,Rule,Text}
$fnStatics  = New-Object System.Collections.ArrayList   # 参考：函数内 static
$perLayer   = [ordered]@{}
foreach ($l in $scanLayers) { $perLayer[$l] = [pscustomobject]@{ Files = 0; R1 = 0; R2 = 0; R3 = 0 } }
$totalFiles = 0

foreach ($layer in $scanLayers) {
    $layerDir = Join-Path $SourceDir $layer
    if (-not (Test-Path -LiteralPath $layerDir)) { continue }

    $files = @(Get-ChildItem -LiteralPath $layerDir -Recurse -File -Include '*.h', '*.cpp' -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -notmatch '\\third_party\\' } | Sort-Object FullName)

    foreach ($f in $files) {
        $rel   = $f.FullName.Substring($SourceDir.Length).TrimStart('\')
        $isH   = ($f.Extension -ieq '.h')
        $lines = Get-CleanLines $f.FullName
        $perLayer[$layer].Files++
        $totalFiles++

        # 作用域栈（'ns' 命名空间 · 'anon' 匿名命名空间 · 'other' 函数体 / 类型体 / 初始化列表）
        $scope = New-Object System.Collections.ArrayList
        [void]$scope.Add('ns')

        for ($li = 0; $li -lt $lines.Length; $li++) {
            $text = $lines[$li].Trim()
            if ($text -eq '') { continue }
            # 字面量续行（多行字符串被替换成 "" 后只剩标点）：跳过
            if ($text -match '^[A-Za-z0-9]{0,3}""\s*;?\s*$') { continue }
            $top = $scope[$scope.Count - 1]

            if ($top -ne 'other') {
                # ---- 命名空间作用域：判定声明 ----
                if ($isH -and $reExternVar.IsMatch($text) -and -not $reSkipStart.IsMatch($text)) {
                    [void]$violations.Add([pscustomobject]@{ Layer = $layer; File = $rel; Line = $li + 1; Rule = 'R1'; Text = $text })
                    $perLayer[$layer].R1++
                }
                elseif (-not $reSkipStart.IsMatch($text) -and $text -match ';\s*$' -and
                        $text -notmatch '\(|\)' -and -not $reFuncLike.IsMatch($text) -and
                        -not $reConstDecl.IsMatch($text)) {
                    $inAnon   = $false
                    $isStatic = $reStaticVar.IsMatch($text)
                    foreach ($sc in $scope) {
                        if ($sc -eq 'anon') { $inAnon = $true; break }
                    }
                    $rule = if ($isStatic -or $inAnon) { 'R2' } else { 'R3' }
                    [void]$violations.Add([pscustomobject]@{ Layer = $layer; File = $rel; Line = $li + 1; Rule = $rule; Text = $text })
                    $perLayer[$layer].$rule++
                }
            }
            elseif ($reStaticVar.IsMatch($text) -and $text -match ';\s*$' -and $text -notmatch '\(' -and
                    -not $reConstDecl.IsMatch($text)) {
                # ---- 函数体内 static 局部变量：参考统计（不计失败）----
                [void]$fnStatics.Add([pscustomobject]@{ Layer = $layer; File = $rel; Line = $li + 1; Text = $text })
            }

            # ---- 花括号结算（逐字符，保证同行多块正确配对）----
            $prefix = New-Object System.Text.StringBuilder
            foreach ($ch in $text.ToCharArray()) {
                if ($ch -eq '{') {
                    [void]$scope.Add((Get-ScopeKind $prefix.ToString()))
                    [void]$prefix.Clear()
                }
                elseif ($ch -eq '}') {
                    if ($scope.Count -gt 1) { $scope.RemoveAt($scope.Count - 1) }
                    [void]$prefix.Clear()
                }
                else { [void]$prefix.Append($ch) }
            }
        }
    }
}

# ---- 输出 ------------------------------------------------------------------
Write-Line ('{0,-9} {1,7} {2,6} {3,6} {4,6}' -f '层', '文件', 'R1', 'R2', 'R3')
Write-Line ('{0,-9} {1,7} {2,6} {3,6} {4,6}' -f '--------', '------', '-----', '-----', '-----')
foreach ($l in $perLayer.Keys) {
    $s = $perLayer[$l]
    Write-Line ('{0,-9} {1,7} {2,6} {3,6} {4,6}' -f $l, $s.Files, $s.R1, $s.R2, $s.R3)
}

$r1 = @($violations | Where-Object { $_.Rule -eq 'R1' }).Count
$r2 = @($violations | Where-Object { $_.Rule -eq 'R2' }).Count
$r3 = @($violations | Where-Object { $_.Rule -eq 'R3' }).Count

if (-not $SummaryOnly -and $violations.Count -gt 0) {
    Write-Line ''
    Write-Line '违规清单（层 · 文件:行 · 规则 · 声明）：'
    foreach ($v in ($violations | Sort-Object Layer, File, Line)) {
        Write-Line ("  [{0}] {1}:{2}  {3}  {4}" -f $v.Layer, $v.File, $v.Line, $v.Rule, $v.Text)
    }
}

$fnCap = if ($MaxFunctionStatics -ge 0) { $MaxFunctionStatics } else { $script:FnStaticCap }

if (-not $SummaryOnly -and $fnStatics.Count -gt 0) {
    Write-Line ''
    Write-Line "参考：函数内 static 局部变量 $($fnStatics.Count) 处（上限 $fnCap · 棘轮只许减 · 不计文件级失败）"
    foreach ($s in ($fnStatics | Sort-Object Layer, File, Line)) {
        Write-Line ("  [{0}] {1}:{2}  {3}" -f $s.Layer, $s.File, $s.Line, $s.Text)
    }
}

Write-Line ''
$summary = "checked files: $totalFiles   violations: $($violations.Count)   (R1 $r1 / R2 $r2 / R3 $r3)   ·   fn_static: $($fnStatics.Count) / $fnCap"

# ---- 基线棘轮（-Baseline）：存量允许，**新增即失败** ----------------------
function Get-ViolationKey($v) {
    $t = ($v.Text -replace '\s+', ' ').Trim()
    return "$($v.Rule)|$($v.File)|$t"
}

$curKeys = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($v in $violations) { [void]$curKeys.Add((Get-ViolationKey $v)) }

if ($UpdateBaseline) {
    if ([string]::IsNullOrWhiteSpace($Baseline)) { throw '-UpdateBaseline 需要同时给 -Baseline <路径>' }
    $out = New-Object System.Collections.ArrayList
    [void]$out.Add('# check_globals 基线（ARCH-A5 · 规则 6 存量债务清单）')
    [void]$out.Add('# 格式：规则|文件|声明（声明按空白归一）· 每条 = 一个待对象化的跨帧可变状态')
    [void]$out.Add('# 维护：修好一条就 `-UpdateBaseline` 重写（基线只减不增）；新增违规必须在代码里修，不许加进本文件。')
    foreach ($k in ($curKeys | Sort-Object)) { [void]$out.Add($k) }
    [System.IO.File]::WriteAllLines($Baseline, $out, (New-Object System.Text.UTF8Encoding($false)))
    Write-Line "基线已写入：$Baseline（$($curKeys.Count) 条）"
    Write-Line ''
}

if (-not [string]::IsNullOrWhiteSpace($Baseline)) {
    if (-not (Test-Path -LiteralPath $Baseline)) { throw "基线文件不存在：$Baseline" }

    $baseKeys = @(Get-Content -LiteralPath $Baseline -Encoding UTF8 -ErrorAction SilentlyContinue |
                  Where-Object { $_.Trim() -ne '' -and -not $_.TrimStart().StartsWith('#') } |
                  ForEach-Object { $_.Trim() })
    $baseSet  = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($k in $baseKeys) { [void]$baseSet.Add($k) }

    $newOnes   = @($violations | Where-Object { -not $baseSet.Contains((Get-ViolationKey $_)) } | Sort-Object Layer, File, Line)
    $fixedKeys = @($baseKeys | Where-Object { -not $curKeys.Contains($_) })

    if ($newOnes.Count -gt 0) {
        Write-Line '新增违规（基线外 · 必须修复）：'
        foreach ($v in $newOnes) {
            Write-Line ("  [{0}] {1}:{2}  {3}  {4}" -f $v.Layer, $v.File, $v.Line, $v.Rule, $v.Text)
        }
        Write-Line ''
    }
    if ($fixedKeys.Count -gt 0) {
        Write-Line "提示：基线可收缩 —— 已修复 $($fixedKeys.Count) 条（用 -UpdateBaseline 重写基线）"
    }

    Write-Line "$summary   ·   baseline: $($baseKeys.Count)   new: $($newOnes.Count)   fixable: $($fixedKeys.Count)"
    if ($newOnes.Count -eq 0 -and $fnStatics.Count -le $fnCap) {
        Write-Line '结果: PASS（无新增跨帧可变状态；存量见基线文件，按 ARCH-A6/A7 逐步清偿；函数内 static 未超棘轮）'
        exit 0
    }
    if ($newOnes.Count -eq 0) {
        Write-Line "结果: FAIL（函数内 static 超出棘轮上限 $fnCap：只许减不许增）"
        exit 1
    }
    Write-Line '结果: FAIL（出现基线外的新增跨帧可变状态：见 ARCHITECTURE.md §1.1 规则 6 / §4 判据 2）'
    exit 1
}

if ($violations.Count -eq 0 -and $fnStatics.Count -le $fnCap) {
    Write-Line "$summary"
    Write-Line '结果: PASS（无跨 TU extern 变量 · 无文件作用域可变状态 · 函数内 static 未超棘轮）'
    exit 0
}

if ($violations.Count -gt 0) {
    Write-Line "$summary"
    Write-Line '结果: FAIL（跨帧可变状态必须是对象成员：见 ARCHITECTURE.md §1.1 规则 6 / §4 判据 2）'
    exit 1
}

Write-Line "$summary"
Write-Line "结果: FAIL（函数内 static 超出棘轮上限 $fnCap：只许减不许增；新增的跨帧状态必须做成对象成员，见 ARCHITECTURE.md §1.1 规则 6）"
exit 1


