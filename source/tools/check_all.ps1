# ============================================================================
#  check_all.ps1 —— 一键跑完「全部离线自检」并汇总（M6 `PM-03`）
#
#  用途：给「不熟悉命令行的人 / 每轮收尾」一条命令看全部结论。
#  纪律（AI_README §3）：
#   * 所有输出**重定向到文件**再读回（本机 shell 集成会吞管道输出）
#   * GUI 程序（aiwrite.exe）stdout 必须用 Start-Process -RedirectStandardOutput 才抓得到
#   * 日志统一落 build\logs\check_all_<时间戳>.log（UTF-8 无 BOM）
#
#  用法：
#    powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\check_all.ps1
#       [-SkipBuild]     不重新构建（只跑自检）
#       [-SkipGui]       跳过需要 GUI 的项（--upload-selftest）
#       [-SkipLoop]      跳过 loop_verify.ps1 -StatusOnly（台账重建）
#       [-OnlyBuild]     只构建
# ============================================================================
param(
    [switch]$SkipBuild,
    [switch]$SkipGui,
    [switch]$SkipLoop,
    [switch]$OnlyBuild
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # source\tools\..\.. = 仓库根
$sourceDir = Join-Path $repoRoot 'source'
$binDir    = Join-Path $repoRoot 'build\bin'
$logDir    = Join-Path $repoRoot 'build\logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

$stamp   = Get-Date -Format 'yyyyMMdd_HHmmss'
$logPath = Join-Path $logDir "check_all_$stamp.log"
$writer  = New-Object System.IO.StreamWriter($logPath, $false, (New-Object System.Text.UTF8Encoding $false))
$writer.AutoFlush = $true

function Write-Both([string]$text) {
    Write-Host $text
    $writer.WriteLine($text)
}

$results = New-Object System.Collections.ArrayList

function Add-Result([string]$name, [bool]$ok, [string]$detail) {
    [void]$results.Add([pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail })
    $flag = if ($ok) { 'PASS' } else { 'FAIL' }
    Write-Both ("  {0,-6} {1,-34} {2}" -f $flag, $name, $detail)
}

# 跑一个程序，输出落文件后读回；返回 @{ Code; Text; File }
function Invoke-Capture([string]$exe, [string]$arguments, [string]$tag) {
    $outFile = Join-Path $logDir "check_all_$stamp`_$tag.txt"
    if ($arguments -eq '') {
        $p = Start-Process -FilePath $exe -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outFile
    } else {
        $p = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outFile
    }
    $text = ''
    if (Test-Path $outFile) { $text = Get-Content $outFile -Raw -ErrorAction SilentlyContinue }
    return [pscustomobject]@{ Code = $p.ExitCode; Text = $text; File = $outFile }
}

Write-Both "# check_all.ps1 · $stamp"
Write-Both "# 仓库根: $repoRoot"
Write-Both "# 日志:   $logPath"

# ---- ① 构建 ----
if (-not $SkipBuild) {
    Write-Both "`n== ① 构建（Debug） =="
    $buildOut = Join-Path $logDir "check_all_$stamp`_build.txt"
    Push-Location $sourceDir
    # 构建失败（含 VS 工具链偶发 `MSB4018 GetOutOfDateItems`）时**不得中止整套自检** ——
    # 本脚本的用途就是「一条命令看全部结论」：失败要打印 `FAIL 构建` 行，并继续跑后面各项。
    # （`$ErrorActionPreference='Stop'` 会把原生命令的 stderr 升级成终止性错误 ⇒ 这里临时降级）
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $buildCode = 1
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $sourceDir 'build.ps1') *> $buildOut
        $buildCode = $LASTEXITCODE
    } catch {
        $buildCode = 1
    } finally {
        $ErrorActionPreference = $prevEap
        Pop-Location
    }
    $buildText = if (Test-Path $buildOut) { Get-Content $buildOut -Raw -ErrorAction SilentlyContinue } else { '' }
    $errCount  = ([regex]::Matches($buildText, 'error C\d+|error LNK\d+|error MSB\d+')).Count
    $warnCount = ([regex]::Matches($buildText, 'warning C\d+')).Count
    $buildDetail = "exit=$buildCode · error=$errCount · warning=$warnCount"
    if ($buildText -match 'MSB4018' -and $buildText -match 'GetOutOfDateItems') {
        # 已知工具链偶发（VS Insiders FileTracker + `build\` 生成态陈旧）⇒ 给可操作提示，
        # 别让人以为是代码坏了；**重跑无效**，要清空重建（陷阱 9 / loop_dev.md §10.1）
        $buildDetail += ' · ⚠ MSB4018 FileTracker（`build\` 生成态陈旧 · 重跑无效）⇒ 清空重建：cd source; .\build.ps1 -Reconfigure'
    }
    Add-Result '构建（0 error / 0 warning）' `
        ($buildCode -eq 0 -and $errCount -eq 0 -and $warnCount -eq 0) `
        $buildDetail
} else {
    Write-Both "`n== ① 构建：已跳过（-SkipBuild） =="
}

if ($OnlyBuild) {
    Write-Both "`n（-OnlyBuild：到此结束）"
    $writer.Close()
    exit 0
}


$apiProbe = Join-Path $binDir 'api_probe.exe'
$aiwrite  = Join-Path $binDir 'aiwrite.exe'

# ---- ② api_probe 三组自检 + 三个 M6 断言 ----
Write-Both "`n== ② api_probe 自检 =="

$r = Invoke-Capture $apiProbe '--selftest' 'selftest'
$summary = (($r.Text -split "`n") | Where-Object { $_ -match '=== 结果' } | Select-Object -First 1)
Add-Result 'api_probe --selftest' ($r.Code -eq 0 -and ($summary -notmatch 'FAIL')) ($summary -replace '^.*=== 结果', '结果')

$r = Invoke-Capture $apiProbe '--exec-selftest' 'exec'
# 注：`--exec-selftest` 会打印**两行**「执行器自检结果」（executor 与 execution 各一行）
# ⇒ 取**最后**一行 = 汇总（实测：73 / 464）
$execLine = (($r.Text -split "`n") | Where-Object { $_ -match '执行器自检结果' } | Select-Object -Last 1)
$execPass = 0; $execFail = 0
if ($execLine -match '(\d+) 通过 / (\d+) 失败') { $execPass = [int]$Matches[1]; $execFail = [int]$Matches[2] }
Add-Result 'api_probe --exec-selftest' ($r.Code -eq 0 -and $execFail -eq 0) "通过 $execPass / 失败 $execFail"

$r = Invoke-Capture $apiProbe '--graph-selftest' 'graph'
$graphLine = (($r.Text -split "`n") | Where-Object { $_ -match '图模型自检结果' } | Select-Object -Last 1)
$graphPass = 0; $graphFail = 0
if ($graphLine -match '(\d+) 通过 / (\d+) 失败') { $graphPass = [int]$Matches[1]; $graphFail = [int]$Matches[2] }
Add-Result 'api_probe --graph-selftest' ($r.Code -eq 0 -and $graphFail -eq 0) "通过 $graphPass / 失败 $graphFail"

$r = Invoke-Capture $apiProbe '--diag-assert' 'diag'
Add-Result 'api_probe --diag-assert' ($r.Code -eq 0) `
    (((($r.Text -split "`n") | Where-Object { $_ -match '诊断报告：' } | Select-Object -First 1) -replace '^\s+', ''))

$r = Invoke-Capture $apiProbe '--dep-check' 'dep'
Add-Result 'api_probe --dep-check' ($r.Code -eq 0) `
    (((($r.Text -split "`n") | Where-Object { $_ -match 'WebView2 运行时' } | Select-Object -First 1) -replace '^\s+', ''))

$r = Invoke-Capture $apiProbe '--settings-assert' 'settings'
Add-Result 'api_probe --settings-assert' ($r.Code -eq 0) `
    (((($r.Text -split "`n") | Where-Object { $_ -match '默认配置通过' } | Select-Object -First 1) -replace '^\s+', ''))

# PB2-24：自建网页版站点闭环（纯函数 + 真实落盘 → 重载 → 下拉/校验即时可用；**自清理**）
$r = Invoke-Capture $apiProbe '--site-assert' 'site'
$siteLine = (($r.Text -split "`n") | Where-Object { $_ -match '纯函数 .* 进表=' } | Select-Object -First 1)
Add-Result 'api_probe --site-assert' ($r.Code -eq 0) ($siteLine -replace '^\s+', '')

# PC-01/02：自动保存 / 崩溃恢复（场景在系统临时目录内构造；**不碰用户数据与快照**；自清理）
$r = Invoke-Capture $apiProbe '--autosave-assert' 'autosave'
$autosaveLine = (($r.Text -split "`n") | Where-Object { $_ -match '场景 .* 默认落点=' } | Select-Object -First 1)
Add-Result 'api_probe --autosave-assert' ($r.Code -eq 0) ($autosaveLine -replace '^\s+', '')

# ---- ③ node 夹具（离线；直接读源码脚本）----
Write-Both "`n== ③ 夹具（node） =="
foreach ($fixture in @('poll_answer_gate_test.js', 'send_chain_test.js', 'dom_gate_test.js')) {
    $outFile = Join-Path $logDir "check_all_$stamp`_fix_$fixture.txt"
    Push-Location $repoRoot
    & node (Join-Path $sourceDir "tools\$fixture") *> $outFile
    $code = $LASTEXITCODE
    Pop-Location
    $text = Get-Content $outFile -Raw
    $tail = (($text -split "`n") | Where-Object { $_ -match '\d+ / \d+' } | Select-Object -Last 1)
    Add-Result "夹具 $fixture" ($code -eq 0) ($tail.Trim())
}

# ---- ④ GUI 自检（上传链路离线三组）----
if (-not $SkipGui) {
    Write-Both "`n== ④ GUI 自检（aiwrite.exe） =="
    $r = Invoke-Capture $aiwrite '--upload-selftest' 'upload'
    $uploadLine = (($r.Text -split "`n") | Where-Object { $_ -match '站点上传模板自检结果' } | Select-Object -First 1)
    Add-Result 'aiwrite --upload-selftest' ($r.Code -eq 0) ($uploadLine.Trim())
} else {
    Write-Both "`n== ④ GUI 自检：已跳过（-SkipGui） =="
}

# ---- ⑤ 站点台账重建（零副作用；-SkipLoop 可跳过）----
if (-not $SkipLoop) {
    Write-Both "`n== ⑤ 站点台账（loop_verify.ps1 -StatusOnly） =="
    $outFile = Join-Path $logDir "check_all_$stamp`_status.txt"
    Push-Location $repoRoot
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $sourceDir 'tools\loop_verify.ps1') -StatusOnly *> $outFile
    $code = $LASTEXITCODE
    Pop-Location
    Add-Result 'loop_verify -StatusOnly' ($code -eq 0) "exit=$code（台账重建见 $outFile）"
}

# ---- ⑥ 文档断链自检（DBG-L01 收口：判据 = **全库 0**） ----
#  * 脚本 = `source/tools/check_links.py`（仓库根由脚本位置推导；剔除代码块 / 行内代码）
#  * 只读、零副作用；退出码 1 = 有断链（逐条 `文件:行 -> 目标` 打印在自检输出里）
Write-Both "`n== ⑥ 文档断链（check_links.py · 全库 0 判据） =="
$linksScript = Join-Path $sourceDir 'tools\check_links.py'
if (Test-Path $linksScript) {
    Push-Location $repoRoot
    $linksOut = & python $linksScript 2>&1
    $linksCode = $LASTEXITCODE
    Pop-Location
    $linksLine = ($linksOut | Where-Object { $_ -match 'checked links' } | Select-Object -Last 1)
    Add-Result 'check_links（文档断链）' ($linksCode -eq 0) ($linksLine -replace '^\s+', '')
}
else {
    Add-Result 'check_links（文档断链）' $false "脚本缺失：$linksScript"
}

# ---- ⑦ 打包 payload 自检（FEA-M6-06 · 打包流水线入闸；DBG-L01 同款「离线可验证」口径） ----
#  * 跑 package.ps1 -SkipBuild -SkipInstaller：重建 payload + manifest + `.iss ↔ payload` 一致性锁
#  * 判据：退出码 ∈ {0, 2}（0 = payload+安装包都成功 / 2 = payload 成功、安装包未编译）**且**
#    `build\dist\payload` 与 manifest.txt 都存在；ISCC 缺失**不是**失败（本机无 Inno Setup 亦可入闸）
Write-Both "`n== ⑦ 打包 payload（package.ps1 -SkipBuild -SkipInstaller） =="
$packageScript = Join-Path $sourceDir 'tools\package.ps1'
if (Test-Path $packageScript) {
    $outFile = Join-Path $logDir "check_all_$stamp`_package.txt"
    Push-Location $repoRoot
    & powershell -NoProfile -ExecutionPolicy Bypass -File $packageScript -SkipBuild -SkipInstaller *> $outFile
    $pkgCode = $LASTEXITCODE
    Pop-Location
    $pkgText = if (Test-Path $outFile) { Get-Content $outFile -Raw -ErrorAction SilentlyContinue } else { '' }
    $pkgLine = (($pkgText -split "`n") | Where-Object { $_ -match 'payload：' } | Select-Object -Last 1)
    $pkgDetail = if ($pkgLine) { $pkgLine.Trim() } else { '' }
    $manifestPath = Join-Path $repoRoot 'build\dist\manifest.txt'
    $pkgOk = ($pkgCode -eq 0 -or $pkgCode -eq 2) -and (Test-Path $manifestPath)
    Add-Result 'package（payload + 一致性锁）' $pkgOk "exit=$pkgCode · $pkgDetail"
} else {
    Add-Result 'package（payload + 一致性锁）' $false "脚本缺失：$packageScript"
}
# ---- 汇总 ----
$failed = @($results | Where-Object { -not $_.Ok })
Write-Both "`n=== check_all 汇总：共 $($results.Count) 项 · 失败 $($failed.Count) 项 ==="
if ($failed.Count -gt 0) {
    foreach ($item in $failed) { Write-Both ("    FAIL  {0}  {1}" -f $item.Name, $item.Detail) }
}
Write-Both "日志：$logPath"
$writer.Close()

if ($failed.Count -gt 0) { exit 1 }
exit 0
