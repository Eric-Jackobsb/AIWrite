<#
  loop_verify.ps1 —— 网页站循环「一轮」总驱动（TST-L01 · docs/loop_dev.md §4 / §6.1 / §6.3）

  做三件事（§6.1 的目录契约由本脚本自动创建，不再手工建目录）：
    ① 门禁      → <轮次目录>\gate\*.txt      （§4 开轮前/收轮后同一张表）
    ② S2 探测   → <轮次目录>\probe.txt / probe.json（10 站批量只读）
    ③ S3 逐站   → <轮次目录>\raw\<id>.run<N>.txt（rc + stdout）· raw\<id>.run<N>.workflow.json（保存的工作流 JSON 物证）
                  · raw\<id>.run<N>.archive\（板上 PC-05 归档：n3-LLMGenerate.txt 的「状态:」= n3 判据权威来源）
    ④ 报告      → <轮次目录>\report.md（四块：① 数字表（含 ①′ 跨轮 diff 段 · TST-L03）② 站点矩阵 ③ needs_login[] + 待人工清单 ④ 站点台账）
                  + report.json（机器读版：含 diff{} 块，供程序与上轮对比；口径对齐 --json-diff）
    ⑤ 台账      → build\loop\SITE_STATUS.json（**跨轮累积**：每站 history[] + latest + 能力两列「文字理解 / 图片理解」）
                   · build\loop\SITE_STATUS.md / <轮次目录>\site_status.md（人读快照）
                   · 四个「人读位」哨兵块（**同一次调用写入 ⇒ 同源不漂移**）：
                   · docs\loop_dev.md §2.2 **合并一览**（20 条）· docs\AI_Status.md §二 **总览**（汇总 + 分片链接）
                   · docs\AI_Status_web.md §二 **网页版分片**（web 11 站）· docs\AI_Status_api.md §二 **官方 API 分片**（official 9 条）
                   · 开关：`-StatusDoc` / `-StatusDocWeb` / `-StatusDocApi`（置空 = 关闭该落点）
                   · -StatusOnly = 零副作用只重建台账（不建轮次目录 / 不跑门禁 / 不探测 / 不起浏览器）

  S3 绿判据 = **三口径**（缺一不可）：
    ① rc=0；② n3（LLMGenerate）状态=done；③ 归档正文不是站侧自己的错误/忙碌串。
    仅 rc=0 不够 —— 需登录 / 发送未生效的站在 --run-selftest 里同样 PASS + rc=0（§10.1 陷阱 3）；
    n3=done 也不够 —— 站点繁忙时会把「System is currently busy…」当答案返回、节点照样 done
    （§10.1 陷阱 6 · SMOKE7 实证），故对「正文 ≤120 码点 且命中已知错误短语」判为「未真生成 ⇒ 失败」。
    n3 状态优先取磁盘归档 n3-LLMGenerate.txt（stdout 丢失时仍可得，§10.1 陷阱 2）。
  退出码（§6.3）：有真失败或有「未测」⇒ 1；否则有需登录 ⇒ 3；否则 0；用法/配置错 ⇒ 2。
  用法示例：
    powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -Round R2 -Providers kimi-web,doubao-web
    powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -SkipGates -SkipProbe -Providers kimi-web   # 只跑 S3（烟测）
    powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -StatusOnly                        # 只重建站点台账（不跑门禁/探测/浏览器）
  注意：S3 会开 WebView2 窗口真机生成 ⇒ **一轮一次**，不要并发（§3.3 同站最多 2 次尝试）。
#>
[CmdletBinding()]
param(
    [string]   $Round      = 'R1',
    [string[]] $Providers  = @(),          # 空 = 由 S2 探测结果自动取「可用（匿名）」站
    [int]      $TimeoutSec = 420,          # 单站 S3 上限（§3.2：取答案超时）
    [int]      $ProbeTimeoutSec = 30,
    [switch]   $SkipGates,
    [switch]   $SkipProbe,
    [string]   $Root = '',
    [switch]   $StatusOnly,                # 只重建站点台账（⑤）⇒ 不建轮次目录、不跑门禁/探测/S3（零副作用，可随时跑）
    [switch]   $UpdateDocs = $true,        # 回写四个 docs 哨兵块（§2.2 + 状态页总览 + web 分片 + official 分片；-UpdateDocs:$false 关闭）
    [string]   $SiteStatusJson = '',       # 台账 JSON 落点（默认 build\loop\SITE_STATUS.json）
    [string]   $StatusDoc      = 'docs\AI_Status.md',      # 状态页**总览**落点（汇总 + 两份分片链接；'' = 关闭）
    [string]   $StatusDocWeb   = 'docs\AI_Status_web.md',  # **网页版分片**落点（web 11 站专项两表；'' = 关闭）
    [string]   $StatusDocApi   = 'docs\AI_Status_api.md'   # **官方 API 分片**落点（official 9 条专项两表；'' = 关闭）
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Root)) { $Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$exe     = Join-Path $Root 'build\bin\aiwrite.exe'
$apiProbe= Join-Path $Root 'build\bin\api_probe.exe'
$stamp   = Get-Date -Format 'yyyyMMdd-HHmm'
$dir     = Join-Path $Root "build\loop\$Round-$stamp"
$rawDir  = Join-Path $dir 'raw'
$gateDir = Join-Path $dir 'gate'
$sandbox = Join-Path $dir 'sandbox'
$statusOnly = $StatusOnly.IsPresent
if (-not $statusOnly) {
    foreach ($d in @($dir, $rawDir, $gateDir, $sandbox)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
}
if (-not $statusOnly) { Set-Content -Path (Join-Path $Root 'build\loop\LATEST.txt') -Value $dir -Encoding UTF8 }
if (-not $statusOnly) { Write-Host "[loop] 轮次目录：$dir" }
else { Write-Host '[loop] -StatusOnly：只重建站点台账（不建轮次目录 / 不跑门禁 / 不探测 / 不起浏览器）' }

$report = [ordered]@{}
$report.round = $Round; $report.started = (Get-Date).ToString('s'); $report.dir = $dir
$report.gates = @(); $report.sites = @(); $report.needs_login = @(); $report.notes = @()

function Read-TextNoLock {           # 读「子进程重定向」产物：WebView2 孙进程（浏览器进程）可能
    param([string]$Path)             # 继承 stdout 写句柄且比子进程活得久 ⇒ 默认 FileShare.Read 会被
    if (-not (Test-Path $Path)) { return '' }   # 「写句柄仍开」拒掉（ReadAllText 崩溃根因，R2 实测）⇒
    try {                            # 用 FileShare.ReadWrite 共享读 + 失败容错（返回空串）
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
        try { return [System.IO.StreamReader]::new($fs, $true).ReadToEnd() } finally { $fs.Dispose() }
    } catch { return '' }
}
function Invoke-Capture {           # 统一「重定向到文件」口径（AI_README §3：管道会吞输出）
    param([string]$File, [string]$Exe, [string[]]$Arg, [string]$Work = $Root, [int]$TimeoutSec = 0)
    $out = Join-Path $File ''
    $rc = 125
    $p = Start-Process -FilePath $Exe -WorkingDirectory $Work -ArgumentList $Arg -PassThru `
         -RedirectStandardOutput ($File + '.out.txt') -RedirectStandardError ($File + '.err.txt')
    if ($TimeoutSec -gt 0) {
        try { $p | Wait-Process -Timeout $TimeoutSec -ErrorAction Stop }
        catch { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; $rc = 124 }
    } else { $p | Wait-Process }
    try { $p.Refresh(); $rc = $p.ExitCode } catch { if ($rc -ne 124) { $rc = 125 } }
    $text = Read-TextNoLock ($File + '.out.txt')
    Set-Content -Path $File -Value $text -Encoding UTF8
    Remove-Item ($File + '.out.txt'), ($File + '.err.txt') -Force -ErrorAction SilentlyContinue
    return [pscustomobject]@{ Rc = $rc; Text = $text }
}
function Write-JsonNoBom {          # 「机器读」JSON 一律**不带 BOM**（python / jq 直接可解析；
    param([string]$Path, $Obj, [int]$Depth = 10)   # PS 5.1 的 Set-Content -Encoding UTF8 会写 BOM）
    $json = $Obj | ConvertTo-Json -Depth $Depth
    [System.IO.File]::WriteAllText($Path, $json, (New-Object System.Text.UTF8Encoding($false)))
}
function Add-Gate {                 # 记一条门禁结论
    param([string]$Name, [string]$Expect, [int]$Rc, [string]$Text, [string]$Pattern)
    $m = $null
    if ($Pattern) { $m = [regex]::Matches($Text, $Pattern) | Select-Object -Last 1 }
    $verdict = 'PASS'
    if ($Rc -ne 0) { $verdict = 'FAIL' }
    if ($Pattern -and (-not $m)) { $verdict = 'FAIL' }
    $hit = if ($m) { $m.Value.Trim() } else { "rc=$Rc" }
    $report.gates += [ordered]@{ name = $Name; expect = $Expect; rc = $Rc; verdict = $verdict; got = $hit }
    Write-Host "[gate] $Name ⇒ $verdict（$hit）"
}

# ---------------------------------------------------------------- ① 门禁 ----
if (-not $SkipGates -and -not $statusOnly) {
    Write-Host '[loop] ① 门禁（§4 同一张表）…'
    $r = Invoke-Capture (Join-Path $gateDir 'build.txt') 'powershell' @('-NoProfile','-ExecutionPolicy','Bypass','-File', (Join-Path $Root 'source\build.ps1'), '-NoLog')
    $errs = [regex]::Matches($r.Text, '(?m)^.*\b(error|warning)\b.*$').Count
    Add-Gate 'build' '0 error / 0 warning' $r.Rc $r.Text $null
    if ($r.Rc -eq 0 -and $errs -eq 0) { $report.gates[-1].got = '0 error / 0 warning' } else { $report.gates[-1].verdict = 'FAIL'; $report.gates[-1].got = "$errs 行含 error/warning" }

    $r = Invoke-Capture (Join-Path $gateDir 'selftest.txt')  $apiProbe @('--selftest')       $Root
    Add-Gate 'selftest（工具/契约总闸）' '七组 PASS · exit 0' $r.Rc $r.Text '=== 结果:.*==='
    $r = Invoke-Capture (Join-Path $gateDir 'graph.txt')     $apiProbe @('--graph-selftest') $Root
    Add-Gate 'graph-selftest' '110 / 0 · exit 0' $r.Rc $r.Text '图模型自检结果:.*'
    $r = Invoke-Capture (Join-Path $gateDir 'exec.txt')      $apiProbe @('--exec-selftest')  $Root
    Add-Gate 'exec-selftest' '452 / 0 · exit 0' $r.Rc $r.Text '执行器自检结果:.*'
    $r = Invoke-Capture (Join-Path $gateDir 'provider.txt')  $exe @('--provider-selftest')   $Root
    # 陷阱 8：真实末行是「50 项通过 / 0 项失败」（数字与斜杠之间有汉字）⇒ 不能再用裸 `\d+\s*/\s*\d+`
    Add-Gate 'provider-selftest' '50 / 0 · exit 0' $r.Rc $r.Text '(?m)^\[配置表自检\]\s*\d+\s*项通过\s*/\s*\d+\s*项失败\s*$'
    $r = Invoke-Capture (Join-Path $gateDir 'upload.txt')    $exe @('--upload-selftest')    $Root
    Add-Gate 'upload-selftest（站点上传模板 · 离线）' '72 / 0 · exit 0' $r.Rc $r.Text '=== 站点上传模板自检结果：.*==='
    $r = Invoke-Capture (Join-Path $gateDir 'dump.txt')      $exe @('--provider-dump')      $Root
    Add-Gate 'provider-dump（条数快照）' '20 条（official 9 / web 11）' $r.Rc $r.Text '条目 \d+ 条（official \d+ / web \d+）'

    foreach ($fx in @('poll_answer_gate_test.js','send_chain_test.js','dom_gate_test.js')) {
        $r = Invoke-Capture (Join-Path $gateDir ("fixture-$fx.txt")) 'node' @((Join-Path $Root "source\tools\$fx")) $Root
        Add-Gate "fixture:$fx" '各自 0 失败 · exit 0' $r.Rc $r.Text '(?m)^.*\d+\s*/\s*\d+.*$'
    }
    $linker = Join-Path $Root 'vcpkg-cache\check_links.py'
    if (Test-Path $linker) {
        $r = Invoke-Capture (Join-Path $gateDir 'links.txt') 'python' @($linker) $Root
        Add-Gate 'links（断链：本轮不得新增）' 'broken 数 ≤ 8（既有 DBG-L01）' $r.Rc $r.Text 'broken.*'
    } else { $report.notes += 'links 门禁跳过：未找到 vcpkg-cache\check_links.py' }
}

# ------------------------------------------------------------- ② S2 探测 ----
$probeJson = Join-Path $dir 'probe.json'
$probeOut  = Join-Path $dir 'probe.txt'
$sites = @()
if (-not $SkipProbe -and -not $statusOnly) {
    Write-Host '[loop] ② S2 批量只读探测（10 站）…'
    $r = Invoke-Capture $probeOut $exe @('--web-adapter-selftest','--provider','all','--timeout',"$ProbeTimeoutSec",'--json',$probeJson) $Root ($ProbeTimeoutSec * 20)
    $report.probe_exit = $r.Rc
    if (Test-Path $probeJson) {
        $pj = [System.IO.File]::ReadAllText($probeJson) | ConvertFrom-Json
        $sites = @($pj.sites)
        if ($pj.PSObject.Properties.Name -contains 'needs_login') { $report.needs_login = @($pj.needs_login) }
    } else { $report.notes += 'S2 未产出 probe.json ⇒ 站点矩阵只能记「未测」' }
} else {
    $report.notes += '本轮未跑 S2（-SkipProbe）⇒ 站点矩阵的可用/需登录列记「未测」'
}

# ------------------------------------------------------------- ③ S3 逐站 ----
$targets = @($Providers | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })   # 兼容 -File 调用下的「a,b」单串
if ($statusOnly) { $targets = @() }   # -StatusOnly：不跑 S3
if ($targets.Count -eq 0) {
    $targets = @($sites | Where-Object { $_.state -eq '可用' -and (-not $_.needs_login) } | ForEach-Object { $_.id })
}
if ($targets.Count -eq 0) { $report.notes += '无可跑的 S3 目标站（S2 未给「可用」站 且 未指定 -Providers）' }
if (-not $statusOnly) { Write-Host "[loop] ③ S3 逐站（$($targets.Count) 站）：$($targets -join ', ')" }

$wf = Join-Path $sandbox 'aiwrite_run_selftest_workflow.json'
$lastWfSnapshot = ''
foreach ($id in $targets) {
    $rc = 125; $stdoutLost = $false; $va07 = ''; $jsonCopy = ''; $n3State = ''; $n3File = $null; $offline = ''
    for ($try = 1; $try -le 2; $try++) {           # §3.3：同站最多 2 次
        Remove-Item (Join-Path $sandbox 'aiwrite_run_selftest_outputs') -Recurse -Force -ErrorAction SilentlyContinue
        $beforeCopy = Join-Path $rawDir "$id.run$try.json_before.json"
        Remove-Item $beforeCopy -Force -ErrorAction SilentlyContinue
        if (Test-Path $wf) { Copy-Item $wf $beforeCopy -Force -ErrorAction SilentlyContinue }   # 运行前基线（离线复算 VA-07 用）
        Remove-Item $wf -Force -ErrorAction SilentlyContinue
        $fs = [System.IO.File]::Open($wf, 'OpenOrCreate', 'ReadWrite', 'ReadWrite')   # 运行时锁住 ⇒ 留 JSON 物证
        $out = Join-Path $rawDir "$id.run$try.txt"
        $err = Join-Path $rawDir "$id.run$try.err.txt"
        $env:TEMP = $sandbox; $env:TMP = $sandbox
        $rc = 125
        try {
            $p = Start-Process -FilePath $exe -WorkingDirectory $Root -PassThru `
                 -ArgumentList '--run-selftest','--web','--provider',$id `
                 -RedirectStandardOutput $out -RedirectStandardError $err
            try { $p | Wait-Process -Timeout $TimeoutSec -ErrorAction Stop }
            catch { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; $rc = 124 }
            try { $p.Refresh(); $rc = $p.ExitCode } catch { if ($rc -ne 124) { $rc = 125 } }
        } catch { $rc = 126 }
        if ($null -eq $rc) { $rc = 125 }
        $jsonCopy = Join-Path $rawDir "$id.run$try.workflow.json"
        if (Test-Path $wf) { try { Copy-Item $wf $jsonCopy -Force -ErrorAction Stop } catch { $jsonCopy = '' } }
        if ($jsonCopy -and (Test-Path $jsonCopy)) { $lastWfSnapshot = $jsonCopy }
        $fs.Close(); $fs.Dispose()

        # 板上归档（PC-05）= 磁盘物证：**stdout 丢失也能判 n3**（见 loop_dev §10.1 陷阱 2）
        $arcRoot = Join-Path $sandbox 'aiwrite_run_selftest_outputs'
        $arcDir  = Get-ChildItem $arcRoot -Directory -ErrorAction SilentlyContinue |
                   Sort-Object LastWriteTime | Select-Object -Last 1
        $n3File = $null; $n3Path = $null
        if ($arcDir) {
            $n3Path = Join-Path $arcDir.FullName 'n3-LLMGenerate.txt'
            if (Test-Path $n3Path) {
                $n3Text  = [System.IO.File]::ReadAllText($n3Path)
                $n3File  = ([regex]::Match($n3Text, '(?m)^状态:\s*(?<s>\w+)') | Select-Object -First 1).Groups['s'].Value
            }
            $dest = Join-Path $rawDir "$id.run$try.archive"
            Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
            Copy-Item $arcDir.FullName $dest -Recurse -Force -ErrorAction SilentlyContinue
        }
        $text = Read-TextNoLock $out
        $va07 = ([regex]::Match($text, '(?m)^.*VA-07 诊断：.*$') | Select-Object -First 1).Value.Trim()
        $n3Stdout = ([regex]::Match($text, '(?m)^\[运行自检\] n3\s+LLMGenerate\s+(?<s>\w+)') | Select-Object -First 1).Groups['s'].Value
        $fin  = ([regex]::Match($text, '(?m)^\[运行自检\] 完成 \d+/\d+.*$') | Select-Object -First 1).Value.Trim()
        $stdoutLost = ([string]::IsNullOrWhiteSpace($text))

        # 归档正文（去头部信息块）—— 「离线复算」与「站侧忙碌/拒绝」判据共用（都不依赖 stdout）
        $body = ''; $bodyCps = 0; $busyHit = ''
        if ($n3Path -and (Test-Path $n3Path)) {
            $body = [System.IO.File]::ReadAllText($n3Path)
            $parts = $body -split '(?m)^-{8,}\r?\n', 2
            $body = if ($parts.Count -gt 1) { $parts[1] } else { $body }
            $body = $body.Trim()
            $bodyCps = [System.Globalization.StringInfo]::ParseCombiningCharacters($body).Count
            # §10.1 陷阱 6：站侧忙碌 / 拒绝 —— 「n3=done」但正文就是站点自己的错误串（= 没真生成）
            # 判据刻意保守：**正文 ≤ 120 码点** 且命中已知错误短语（避免长篇合法答案里的偶然出现 ⇒ 假失败）
            if ($bodyCps -gt 0 -and $bodyCps -le 120) {
                $busyHit = ([regex]::Matches($body, '(?i)(system is currently busy|please try again later|rate limit|too many requests|系统繁忙|服务繁忙|访问过于频繁|请求过于频繁|请稍后再试|稍后再试|服务暂不可用|服务异常)') |
                            Select-Object -First 1).Value
            }
            # PW-07n（真机驱动 · T-L17 回归扫描 ernie-web）：**生成中占位**也算「未真生成」——
            #   n3=done 但正文 = 站侧自动标题 + `正在编写，请稍候...` + `0%` + `改用对话直接回答`（19 字节）
            #   ⇒ 三口径会误判「可用」。判据 = 占位 / 进度短语 **且** 裸百分比（两个独立信号同时出现）。
            if (-not $busyHit -and $bodyCps -gt 0 -and $bodyCps -le 120) {
                if (($body -match '\d{1,3}\s*%') -and ($body -match '(正在编写|正在生成|正在思考|正在回答|生成中|思考中|回答中|请稍候|请稍等|改用对话直接回答|Generating|Thinking|Loading)')) {
                    $busyHit = '生成中占位（占位词 + 百分比进度 · PW-07n）'
                }
            }
            # PW-07m（真机驱动 · T-L17 回归扫描 yuanbao-web）：**站侧内部码**也算「未真生成」——
            #   n3=done 但正文 = `CONFLICT_LOG`（12 字节，元宝把内部诊断节点渲染进了 DOM）
            #   ⇒ 三口径会误判「可用」。判据 = 整串（去空白）就是一个 `SNAKE_CASE` 常量名。
            if (-not $busyHit -and $bodyCps -gt 0 -and $bodyCps -le 120) {
                if (($body -replace '\s', '') -match '^[A-Z][A-Z0-9]*(_[A-Z0-9]+)+$') {
                    $busyHit = "站侧内部码（PW-07m：$($body.Trim())）"
                }
            }
        }

        # 离线复算 VA-07（不依赖 stdout）：归档 n3 正文 + 运行前 JSON 基线 ⇒ 独立复核 App 自报口径
        $offline = ''
        $basePath = $beforeCopy; $baseSrc = '本轮运行前文件'
        if (-not (Test-Path $basePath)) {
            if ($lastWfSnapshot -and (Test-Path $lastWfSnapshot)) { $basePath = $lastWfSnapshot; $baseSrc = '上次运行保存的文件（等价性由 p11_3≡p12 逐字节相同实证）' }
            else { $basePath = '' }
        }
        if ($body -and $basePath) {
            $idx  = [System.Globalization.StringInfo]::ParseCombiningCharacters($body)
            $cps  = $bodyCps
            $pre  = if ($cps -gt 24) { $body.Substring(0, $idx[23] + 1) } else { $body }
            $preCps = [System.Globalization.StringInfo]::ParseCombiningCharacters($pre).Count
            $jsonBefore = [System.IO.File]::ReadAllText($basePath)
            $hit = ($preCps -ge 24) -and ($jsonBefore.Contains($pre))
            $verdict = if ($hit) { '是（合法复述 ⇒ 非泄漏）' }
                       elseif ($baseSrc -eq '本轮运行前文件') { '否（未出现在基线 ⇒ 辅助证据：倾向「真泄漏」）' }
                       else { '未定（基线取自上次快照且未命中 ⇒ 以 App 自报为准）' }
            $offline = "离线粗核对（口径：归档正文 / App 口径为「节点输出端口文本」，两者不完全一致）：命中运行前内容=$verdict / 归档正文 $([System.Text.Encoding]::UTF8.GetByteCount($body)) 字节/$cps 码点 / 取样=$preCps 码点（$([System.Text.Encoding]::UTF8.GetByteCount($pre)) 字节） / 基线 $([System.Text.Encoding]::UTF8.GetByteCount($jsonBefore)) 字节 · 来源=$baseSrc"
        }
        if ($busyHit) { $offline = "离线复算：不适用（归档正文＝站侧错误串「$busyHit」· $bodyCps 码点 ⇒ 本轮该站没有真实答案可判读；见 §10.1 陷阱 6）" }

        # 判据（三口径！）：rc 只说明「自检全通过」，**不说明该站生成成功** ——
        # 需登录 / 发送未生效的站也会 PASS + rc=0（R1 实证：spark-web、yuanbao-web）⇒ 必须同时看 n3 状态；
        # n3=done 也不够：站点繁忙时会把「System is currently busy…」当答案返回（SMOKE7 实证）⇒ 再加正文自检（$busyHit）。
        # n3 状态**优先取磁盘归档**（stdout 丢失时仍可得），无归档时才退回 stdout 解析。
        $n3State = if ($n3File) { $n3File } else { $n3Stdout }
        if (($rc -eq 0) -and ($n3State -eq 'done') -and (-not $busyHit)) { break }   # 已得绿结论（stdout 丢不丢都不再重试）
        if ($try -eq 1) {
            Write-Host "[loop]   $id rc=$rc · n3=$(if ($n3State) { "$n3State$(if ($n3File) { '（归档）' } else { '（stdout）' })" } else { '（无 n3 证据）' })$(if ($busyHit) { " · ⚠ 正文＝站侧错误串「$busyHit」" })$(if ($stdoutLost) { ' · stdout 丢失' }) ⇒ 重试 1 次（§3.3）"
        }
    }
    $state = '失败（真失败 ⇒ 退出码 1）'
    if ($rc -eq 0 -and $n3State -eq 'done' -and $busyHit) { $state = "失败（站侧忙碌/拒绝：n3=done 但正文＝站侧错误串「$busyHit」· 未真生成）" }
    elseif ($rc -eq 0 -and $n3State -eq 'done') { $state = '可用（匿名）' }
    elseif ($rc -eq 0 -and $n3State) { $state = "失败（生成未成功：n3=$n3State ⇒ 站点侧 / 需登录 · 退出码未区分）" }
    elseif ($rc -eq 0) { $state = '未测（rc=0 但无 n3 证据 ⇒ 归档 + stdout 均缺，需人工复核）' }
    if ($rc -eq 3)   { $state = '需登录' }
    if ($rc -eq 124) { $state = '失败（取答案超时 ⇒ §5.1②③）' }
    $report.sites += [ordered]@{
        id = $id; rc = $rc; state = $state; n3 = $n3State
        n3_source = $(if ($n3File) { 'archive' } elseif ($n3Stdout) { 'stdout' } else { '' })
        busy = $busyHit; body_cps = $bodyCps
        finished = $fin; stdout_lost = $stdoutLost; va07 = $va07; offline = $offline
        json = $(if ($jsonCopy) { Split-Path $jsonCopy -Leaf } else { '' })
    }
    Write-Host "[loop]   $id ⇒ rc=$rc · $state"
    Start-Sleep -Seconds 3
    if ($rc -eq 2) {   # §6.3：用法/配置错误 ⇒ 停循环，不许带着错误参数继续
        $report.notes += "$id 返回退出码 2（用法/配置错误）⇒ 本轮提前停止（§6.3）"
        Write-Host "[loop]   $id 返回 2（用法/配置错误）⇒ 提前停止本轮"
        break
    }
}

# ------------------------------------------------- ⑤ 站点台账（TST-L05）----
# 两列含义是**链路**、不是模型强弱：
#   文字理解 = 「文本 → 取回文本」（S3 三口径：rc=0 + n3=done + 正文非站侧错误串）
#   图片理解 = 「图片 → 取回文本」（站点上传双证据 + 取回正文）
# 数据源（唯一权威，不要在 docs 里手改数字）：
#   ① source\assets\providers.json —— 声明与接线：capabilities.vision / web.upload_adapter / web.input_selector / verified / protocol / api_base
#   ② source\assets\site_capability_evidence.json —— **真机实测证据台账**（人工唯一维护点：每站 text / image 的 tested + evidence + date）
#   ③ build\loop\*\report.json —— 各轮实测结果（跨轮累积 history[]；网页文字链路的「可用（匿名）」以轮次物证为准）
# 冲突规则（实测优先，冲突必须显形）：tested=true 而 providers.json 同名声明为假 ⇒ 该列追加「⚠ 声明未回填」，摘要里计数
function Get-SiteStaticInfo {
    param([string]$Root)
    $static = @{}; $cap = @{}
    $pPath = Join-Path $Root 'source\assets\providers.json'
    if (Test-Path $pPath) {
        $j = [System.IO.File]::ReadAllText($pPath) | ConvertFrom-Json
        foreach ($p in $j.providers) {
            $names  = @($p.PSObject.Properties.Name)
            $w      = $(if ($names -contains 'web') { $p.web } else { $null })
            $wnames = $(if ($w) { @($w.PSObject.Properties.Name) } else { @() })
            $static[[string]$p.id] = [ordered]@{
                id              = [string]$p.id
                display         = [string]$p.display
                kind            = [string]$p.kind
                protocol        = [string]$p.protocol
                verified        = [bool]$p.verified
                vision          = $(if (($names -contains 'capabilities') -and $p.capabilities) { [bool]$p.capabilities.vision } else { $false })
                upload_adapter  = $(if ($wnames -contains 'upload_adapter') { [string]$w.upload_adapter } else { '' })
                input_selector  = $(if (($wnames -contains 'input_selector') -and $w.input_selector) { 'Y' } else { 'N' })
                answer_selector = $(if (($wnames -contains 'answer_selector') -and $w.answer_selector) { 'Y' } else { 'N' })
                api_base        = $(if ($names -contains 'api_base') { [string]$p.api_base } else { '' })
            }
        }
    } else { Write-Host '[loop] ⚠ 未找到 providers.json ⇒ 台账只含轮次数据' }
    $cPath = Join-Path $Root 'source\assets\site_capability_evidence.json'
    if (Test-Path $cPath) {
        $cj = [System.IO.File]::ReadAllText($cPath) | ConvertFrom-Json
        foreach ($prop in $cj.PSObject.Properties) {
            if ($prop.Name.StartsWith('_')) { continue }
            $cap[$prop.Name] = $prop.Value
        }
    } else { Write-Host '[loop] ⚠ 未找到 site_capability_evidence.json ⇒ 能力列只按 providers.json 声明推导' }
    return @{ static = $static; cap = $cap }
}
function Add-SiteHistory {
    param([hashtable]$Hist, [string]$Id, $Entry)
    if (-not $Hist.ContainsKey($Id)) { $Hist[$Id] = New-Object System.Collections.ArrayList }
    [void]$Hist[$Id].Add($Entry)
}
function New-SiteStatus {
    param([string]$Root, [bool]$IncludeCurrent, $CurMatrix = @(), $CurSites = @(), [string]$RoundLabel = '', [string]$Stamp = '')
    $sc     = Get-SiteStaticInfo -Root $Root
    $static = $sc.static
    $ledger = $sc.cap

    # 1) 逐轮读 report.json（目录名末 13 位 = 时间戳 ⇒ 时间序 = 跨轮累积顺序）
    $hist = @{}
    $roundList = @()
    $loopRoot = Join-Path $Root 'build\loop'
    if (Test-Path $loopRoot) {
        $rounds = @(Get-ChildItem $loopRoot -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '-\d{8}-\d{4}$' } |
                    Sort-Object { $_.Name.Substring($_.Name.Length - 13) })
        foreach ($rd in $rounds) {
            $rp = Join-Path $rd.FullName 'report.json'
            if (-not (Test-Path $rp)) { continue }
            try { $rj = [System.IO.File]::ReadAllText($rp) | ConvertFrom-Json } catch { Write-Host "[loop] ⚠ 台账跳过无法解析的 $rp"; continue }
            $mx = @($rj.matrix); $ss = @($rj.sites)
            $roundList += [ordered]@{ round = [string]$rj.round; dir = $rd.Name; started = [string]$rj.started; exit_code = $rj.exit_code }
            $ids = @()
            foreach ($m in $mx) { $mid = [string]$m.id; if ($mid -match '^[a-z0-9][a-z0-9._-]*$') { $ids += $mid } elseif ($mid) { Write-Host "[loop] ⚠ 台账跳过非法站点 id「$mid」（$($rd.Name)）" } }
            foreach ($s in $ss) { $sid = [string]$s.id; if (($sid -match '^[a-z0-9][a-z0-9._-]*$') -and ($ids -notcontains $sid)) { $ids += $sid } }
            foreach ($id in $ids) {
                $m = @($mx | Where-Object { $_.id -eq $id }) | Select-Object -First 1
                $s = @($ss | Where-Object { $_.id -eq $id }) | Select-Object -First 1
                Add-SiteHistory -Hist $hist -Id $id -Entry ([ordered]@{
                    round = [string]$rj.round; time = [string]$rj.started; dir = $rd.Name
                    s2 = $(if ($m) { [ordered]@{ state = [string]$m.probe; hits = $m.probe_hits; candidates = $m.probe_candidates; login = [string]$m.login_evidence; url = [string]$m.url } } else { $null })
                    s3 = $(if ($s) { [ordered]@{ rc = $s.rc; n3 = [string]$s.n3; src = [string]$s.n3_source; busy = [string]$s.busy; cps = $s.body_cps; stdout_lost = [bool]$s.stdout_lost } } else { $null })
                    state = $(if ($m) { [string]$m.state } elseif ($s) { [string]$s.state } else { '未测' })
                })
            }
        }
    }
    if ($IncludeCurrent) {
        # 本轮 report.json 还没落盘（⑤ 在写盘前算，④ 块要引用）⇒ 显式把「本轮」补进轮次表，
        # 否则 docs 哨兵块会写「覆盖轮次 N-1」，与轮末 -StatusOnly(读盘) 的结果不一致（= 幂等假象）。
        $roundList += [ordered]@{ round = $RoundLabel; dir = '(本轮)'; started = $Stamp; exit_code = $null }
        $ids = @()
        foreach ($m in $CurMatrix) { $ids += [string]$m.id }
        foreach ($s in $CurSites) { if ($ids -notcontains [string]$s.id) { $ids += [string]$s.id } }
        foreach ($id in $ids) {
            $m = @($CurMatrix | Where-Object { $_.id -eq $id }) | Select-Object -First 1
            $s = @($CurSites | Where-Object { $_.id -eq $id }) | Select-Object -First 1
            Add-SiteHistory -Hist $hist -Id $id -Entry ([ordered]@{
                round = $RoundLabel; time = $Stamp; dir = '(本轮)'
                s2 = $(if ($m) { [ordered]@{ state = [string]$m.probe; hits = $m.probe_hits; candidates = $m.probe_candidates; login = [string]$m.login_evidence; url = [string]$m.url } } else { $null })
                s3 = $(if ($s) { [ordered]@{ rc = $s.rc; n3 = [string]$s.n3; src = [string]$s.n3_source; busy = [string]$s.busy; cps = $s.body_cps; stdout_lost = [bool]$s.stdout_lost } } else { $null })
                state = $(if ($m) { [string]$m.state } elseif ($s) { [string]$s.state } else { '未测' })
            })
        }
    }
    # 2) 逐站算「文字理解 / 图片理解」两列 + 最终态 + 下一动作
    $implProto = @('openai','deepseek-web','dom')   # 本版本已实现协议（来源：aiwrite.exe --provider-dump 的「已实现协议」行）
    $order = @()
    foreach ($id in $static.Keys) { if ($static[$id].kind -eq 'web') { $order += $id } }
    foreach ($id in $static.Keys) { if ($static[$id].kind -ne 'web') { $order += $id } }
    foreach ($id in $hist.Keys) { if ($order -notcontains $id) { if ($static.ContainsKey($id)) { $order += $id } else { Write-Host "[loop] ⚠ 台账忽略历史条目 id「$id」（已不在 providers.json ⇒ 仅旧轮次有记录，不计入条数汇总；原始物证仍在 build/loop/<轮次>/report.json）" } } }   # 2026-10-10：已移除条目（如去重移除的 tongyi-web）不再进台账 / 不计入 total·official（否则与 --provider-dump 的条数不一致）

    $rows = @()
    foreach ($id in $order) {
        $st      = $(if ($static.ContainsKey($id)) { $static[$id] } else { $null })
        if ($hist.ContainsKey($id)) { $h = @($hist[$id]) } else { $h = @() }   # 注意：不能写成 $(if … { @(…) } …)——$( ) 会把单元素数组塌缩成标量
        $latest  = $(if ($h.Count -gt 0) { $h[$h.Count - 1] } else { $null })
        $kind    = $(if ($st) { [string]$st.kind } else { 'unknown' })
        $proto   = $(if ($st) { [string]$st.protocol } else { '' })
        $isWeb   = ($kind -eq 'web')
        $unimpl  = (-not ($implProto -contains $proto))
        $declaredVision = $(if ($st) { [bool]$st.vision } else { $false })
        $wired   = $(if ($st) { [string]$st.upload_adapter } else { '' })
        $hasBase = $(if ($st -and $st.api_base) { $true } else { $false })
        $led      = $(if ($ledger.ContainsKey($id)) { $ledger[$id] } else { $null })
        $ledNames = $(if ($led) { @($led.PSObject.Properties.Name) } else { @() })
        $ledText   = $(if ($ledNames -contains 'text')  { [bool]$led.text.tested } else { $false })
        $ledTextEv = $(if (($ledNames -contains 'text')  -and $led.text.evidence)  { [string]$led.text.evidence }  else { '' })
        $ledImg    = $(if ($ledNames -contains 'image') { [bool]$led.image.tested } else { $false })
        $ledImgEv  = $(if (($ledNames -contains 'image') -and $led.image.evidence) { [string]$led.image.evidence } else { '' })
        $conflict   = @()
        $usableHist = @($h | Where-Object { $_.state -eq '可用（匿名）' })

        # ---- 文字理解（「文本 → 取回文本」）----
        if ($isWeb) {
            if ($usableHist.Count -gt 0) {
                $capText = "✅ 实测（$($usableHist[$usableHist.Count - 1].round)）"
                $textEv  = "轮次物证：$($usableHist[$usableHist.Count - 1].round) 三口径通过"
            } elseif ($ledText) { $capText = '✅ 实测'; $textEv = $ledTextEv }
            elseif ($h.Count -eq 0) { $capText = '⬜ 未测'; $textEv = '无轮次记录（S2 未探到该站）' }
            else { $capText = '⬜ 未测（S3 未通过）'; $textEv = "最近一轮：$($latest.state)" }
        } else {
            $textEv = $ledTextEv
            if ($ledText) { $capText = '✅ 实测' }
            elseif ($st -and $st.verified) { $capText = '✅ 实测（verified）'; $textEv = 'providers.json verified=true（声明已实测 · 无逐条物证指针）' }
            elseif ($unimpl) { $capText = '⚠ 协议未实现' }
            elseif (-not $hasBase) { $capText = '⚠ 未填 api_base' }
            else { $capText = '🟡 声明·未实测' }
            if ($ledText -and -not ($st -and $st.verified)) { $conflict += 'verified 未回填'; $capText = "$capText ⚠未回填" }
        }
        # ---- 图片理解（「图片 → 取回文本」）----
        if ($isWeb) {
            if ($ledImg) { $capImage = '✅ 实测'; $capImageEv = $ledImgEv }
            elseif ($wired) { $capImage = '🟡 就绪·未实测'; $capImageEv = "接线 $wired · attach_selector 待侦察/待真机" }
            else { $capImage = '⬜ 未接'; $capImageEv = '无 web.upload_adapter（VLM + 网页版组合会明确报错）' }
        } else {
            if ($ledImg) { $capImage = '✅ 实测'; $capImageEv = $ledImgEv }
            elseif ($unimpl) { $capImage = '⚠ 协议未实现'; $capImageEv = '协议实现前该链路不可用' }
            elseif (-not $hasBase) { $capImage = '🟡 待 base+Key'; $capImageEv = '未填 api_base' }
            elseif ($declaredVision) { $capImage = '🟡 声明·未实测'; $capImageEv = 'capabilities.vision=true（无真机物证）' }
            else { $capImage = '⬜ 未声明'; $capImageEv = 'capabilities.vision=false' }
        }
        if ($ledImg -and -not $declaredVision) { $conflict += 'vision 声明未回填'; $capImage = "$capImage ⚠声明未回填" }
        # ---- 最终态（口径与 report.json 的 matrix.state 相同；official / 协议型适配器按就绪度）----
        if ($isWeb -and $proto -eq 'deepseek-web') { $final = $(if ($st -and $st.verified) { '✅ 已实测（协议型适配器）' } else { '未验证' }) }
        elseif ($h.Count -gt 0) { $final = [string]$latest.state }
        elseif ($isWeb) { $final = '未测' }
        else { $final = $(if ($unimpl) { '⚠ 协议未实现' } elseif ($ledText -or ($st -and $st.verified)) { '✅ 已实测' } elseif (-not $hasBase) { '⚠ 未填 api_base' } else { '🟡 声明·未实测' }) }

        # ---- 下一动作（按 §5.1 三分法给「最小可执行动作」）----
        $next = '—'
        if ($conflict -contains 'vision 声明未回填') { $next = '回填 providers.json capabilities.vision（M7-10）' }
        elseif ($latest -and $latest.s2 -and $latest.s2.state -eq '可用' -and [int]$latest.s2.hits -eq 0) { $next = '⚠ 候选态：回填 input_selector / 收紧 S2 判据' }
        elseif ($final -like '*需登录*') { $next = '人工在窗口内登录一次 → 重跑 S3' }
        elseif ($final -like '失败（站侧*') { $next = '复测 2~3 次定性（偶发 / 限流）' }
        elseif ($final -like '失败（探测级*' -or $final -like '失败（环境*') { $next = '冷启动后重测；仍失败要有 DOM 物证才可分②改版' }
        elseif ($final -like '失败*') { $next = '按 loop_dev §5.1 归类后定动作' }
        elseif ($final -like '⚠ 未填 api_base*') { $next = '填 providers.json api_base + Key 后真机一次' }
        elseif ($capImage -like '🟡 就绪*') { $next = '侦察 attach_selector → 真机一次' }
        elseif ($final -like '⚠ 协议未实现*') { $next = '协议实现后重测' }
        elseif ($capText -like '🟡*' -or $capImage -like '🟡*') { $next = '有 Key / 环境就绪时真机一次' }
        elseif ($isWeb -and $final -like '未测*') { $next = 'S2 探到「可用」后跑 S3' }

        $rows += [ordered]@{
            id = $id
            display = $(if ($st) { [string]$st.display } else { '' })
            kind = $kind
            protocol = $proto
            protocol_implemented = (-not $unimpl)
            api_base = $(if ($st) { [string]$st.api_base } else { '' })
            type = $(if ($isWeb) { $(if ($proto -eq 'dom') { 'web·dom' } else { "web·$proto" }) } else { "official·$proto" })
            verified = $(if ($st) { [bool]$st.verified } else { $false })
            declared_vision = $declaredVision
            upload_adapter = $wired
            input_selector = $(if ($st) { [string]$st.input_selector } else { 'N' })
            answer_selector = $(if ($st) { [string]$st.answer_selector } else { 'N' })
            latest = $latest
            history = $h
            text = $capText
            text_evidence = $textEv
            image = $capImage
            image_evidence = $capImageEv
            final = $final
            next = $next
            conflict = $conflict
        }
    }
    $sum = [ordered]@{
        total = $rows.Count
        web = @($rows | Where-Object { $_.kind -eq 'web' }).Count
        official = @($rows | Where-Object { $_.kind -eq 'official' }).Count
        text_tested = @($rows | Where-Object { $_.text -like '✅*' }).Count
        image_tested = @($rows | Where-Object { $_.image -like '✅*' }).Count
        image_ready = @($rows | Where-Object { $_.image -like '🟡*' }).Count
        image_none = @($rows | Where-Object { $_.image -like '⬜*' }).Count
        conflict = @($rows | Where-Object { @($_.conflict).Count -gt 0 }).Count
        rounds = $roundList.Count
    }
    return [ordered]@{
        schema = 1
        generated = (Get-Date).ToString('s')
        rounds = $roundList
        sites = $rows
        summary = $sum
        sources = [ordered]@{
            providers = 'source/assets/providers.json'
            evidence  = 'source/assets/site_capability_evidence.json'
            rounds    = 'build/loop/*/report.json'
            rebuild   = 'powershell -NoProfile -ExecutionPolicy Bypass -File .\source\tools\loop_verify.ps1 -StatusOnly'
        }
    }
}
function Format-SiteStatusMd {
    param($Status, [bool]$ForDocs = $false, [string]$Scope = 'all')
    # Scope 四态（同一 $Status ⇒ 同源不漂移）：
    #   'all'      = 合并一览（docs\loop_dev.md §2.2 哨兵块 / build\loop\SITE_STATUS.md）
    #   'summary'  = 总览（docs\AI_Status.md §二：只汇总 + 指向两份分片，不重复列全量）
    #   'web'      = 网页版分片（docs\AI_Status_web.md §二：链路证据 + 能力与接线）
    #   'official' = 官方 API 分片（docs\AI_Status_api.md §二：协议与就绪 + 证据与动作）
    $s   = $Status.summary
    $all = @($Status.sites)
    $web = @($all | Where-Object { $_.kind -eq 'web' })
    $off = @($all | Where-Object { $_.kind -eq 'official' })
    $rows = $all
    if ($Scope -eq 'web') { $rows = $web } elseif ($Scope -eq 'official') { $rows = $off }
    # 哨兵块（ForDocs）只写**日期**：同一日内反复跑 ⇒ 内容逐字不变（可做幂等校验），也避免「每跑一次 docs 就多一行 diff」；
    # 精确到秒的时间戳留在 SITE_STATUS.json / SITE_STATUS.md / <轮次目录>\site_status.md（那几个是易失快照）。
    $stamp   = $(if ($ForDocs) { ([datetime]$Status.generated).ToString('yyyy-MM-dd') } else { $Status.generated })
    $genLine = "> 生成：``loop_verify.ps1``（TST-L05）· 时间 $stamp · 覆盖轮次 $($s.rounds)：$((@($Status.rounds | ForEach-Object { $_.round })) -join ' · ')"
    $legend  = '> **图例**：``✅ 实测``（有真机物证）· ``🟡 就绪/声明·未实测`` · ``⬜ 未接/未测/未声明`` · ``⚠ 不可用或不一致``'
    $srcLine = '> 来源 = ``providers.json``（声明/接线）× ``site_capability_evidence.json``（实测证据）× ``build/loop/*/report.json``（轮次物证），**冲突时实测优先并在列内标 ⚠**。'
    $rebuild = "> 重建（零副作用，随时可跑）：``$($Status.sources.rebuild)``"
    $md = @()

    # ---------------- 总览（docs\AI_Status.md §二）：只汇总 + 分片链接 ----------------
    if ($Scope -eq 'summary') {
        $md += $genLine
        $md += "> **分通道明细**（按 ``kind`` 分片 · 同一次调用写入 ⇒ 与本文汇总同源）：**网页版** ``kind=web`` **$($s.web)** 站 → ``docs/AI_Status_web.md`` §二 · **官方 API** ``kind=official`` **$($s.official)** 条 → ``docs/AI_Status_api.md`` §二"
        $md += "> **汇总**：共 $($s.total) 条（web $($s.web) / official $($s.official)）· 文字理解 ✅ 实测 **$($s.text_tested)** · 图片理解 ✅ 实测 **$($s.image_tested)** · 🟡 就绪/声明 $($s.image_ready) · ⬜ 未接/未声明 $($s.image_none) · ⚠ 声明不一致 $($s.conflict)"
        $md += $rebuild
        return ($md -join "`r`n")
    }
    # ---------------- 网页版分片（docs\AI_Status_web.md §二）：链路证据 + 能力与接线 ----------------
    if ($Scope -eq 'web') {
        $wText  = @($web | Where-Object { $_.text  -like '✅*' }).Count
        $wImg   = @($web | Where-Object { $_.image -like '✅*' }).Count
        $wReady = @($web | Where-Object { $_.image -like '🟡*' }).Count
        $wNone  = @($web | Where-Object { $_.image -like '⬜*' }).Count
        $wLogin = @($web | Where-Object { $_.final -like '*需登录*' }).Count
        $wBusy  = @($web | Where-Object { $_.final -like '*站侧忙碌*' }).Count
        $md += $genLine
        $md += "> **范围**：``kind=web`` 的 **$($web.Count) 站**（DOM 自动化 $($web.Count - 1) 站 + 协议型适配器 ``deepseek-web`` 1 站）· **文字理解**判据 = S3 **三口径**（``rc=0`` + ``n3=done`` + 归档正文非站侧错误/忙碌串）；**图片理解**判据 = 站点上传双证据 + 取回正文。"
        $md += $legend
        $md += $srcLine
        $md += ''
        $md += "**汇总（web）**：共 $($web.Count) 站 · 文字理解 ✅ 实测 **$wText** · 图片理解 ✅ 实测 **$wImg** / 🟡 就绪 $wReady / ⬜ 未接 $wNone · 归因口径：需登录 $wLogin · 站侧忙碌 $wBusy"
        $md += ''
        $md += '**表 A · 链路证据（S2 匿名探测 → S3 端到端）**'
        $md += ''
        $md += '| # | id | 接入 | 最近轮次 | S2 探测 | 输入命中/候选 | 登录证据 | S3 rc | n3 | 正文码点 | 最终态 |'
        $md += '|---|---|---|---|---|---|---|---|---|---|---|'
        $i = 0
        foreach ($row in $rows) {
            $i++
            $lat = $row.latest
            $md += "| $i | ``$($row.id)`` | $($row.type) | $(if ($lat) { $lat.round } else { '—' }) | $(if ($lat -and $lat.s2) { $lat.s2.state } else { '—' }) | $(if ($lat -and $lat.s2) { "$($lat.s2.hits)/$($lat.s2.candidates)" } else { '—' }) | $(if ($lat -and $lat.s2 -and $lat.s2.login) { $lat.s2.login } else { '—' }) | $(if ($lat -and $lat.s3) { $lat.s3.rc } else { '—' }) | $(if ($lat -and $lat.s3) { $lat.s3.n3 } else { '—' }) | $(if ($lat -and $lat.s3 -and $lat.s3.cps) { $lat.s3.cps } else { '—' }) | $($row.final) |"
        }
        $md += ''
        $md += '**表 B · 能力与接线（文字理解 / 图片理解 · 下一动作）**'
        $md += ''
        $md += '| # | id | 文字理解 | 图片理解 | 上传适配器 | 选择器 in/ans | 依据（图片理解） | 下一动作 |'
        $md += '|---|---|---|---|---|---|---|---|'
        $i = 0
        foreach ($row in $rows) {
            $i++
            $adp = $(if ($row.upload_adapter) { "``$($row.upload_adapter)``" } else { '—' })
            $md += "| $i | ``$($row.id)`` | $($row.text) | $($row.image) | $adp | $($row.input_selector)/$($row.answer_selector) | $($row.image_evidence) | $($row.next) |"
        }
        $md += ''
        $md += $rebuild
        return ($md -join "`r`n")
    }
    # ---------------- 官方 API 分片（docs\AI_Status_api.md §二）：协议与就绪 + 证据与动作 ----------------
    if ($Scope -eq 'official') {
        $aText  = @($off | Where-Object { $_.text  -like '✅*' }).Count
        $aImg   = @($off | Where-Object { $_.image -like '✅*' }).Count
        $aProto = @($off | Where-Object { -not $_.protocol_implemented }).Count
        $aBase  = @($off | Where-Object { -not $_.api_base }).Count
        $aConf  = @($off | Where-Object { @($_.conflict).Count -gt 0 }).Count
        $md += $genLine
        $md += "> **范围**：``kind=official`` 的 **$($off.Count) 条**（``openai`` / ``anthropic`` / ``gemini`` 协议直连：不起浏览器、不走 DOM、不依赖登录态）· **文字理解**判据 = 真机 HTTP 200 且取回正文（或 ``providers.json`` ``verified=true``）；**图片理解**判据 = 多模态真机一次 —— ``capabilities.vision=true`` 只是**声明**（§10.1 陷阱 7）。"
        $md += $legend
        $md += $srcLine
        $md += ''
        $md += "**汇总（official）**：共 $($off.Count) 条 · 文字理解 ✅ 实测 **$aText** · 图片理解 ✅ 实测 **$aImg** · ⚠ 协议未实现 $aProto · ⚠ 未填 ``api_base`` $aBase · ⚠ 声明未回填 $aConf"
        $md += ''
        $md += '**表 A · 协议与就绪**'
        $md += ''
        $md += '| # | id | 协议 | 本版本已实现 | api_base | capabilities.vision（声明） | verified（声明） | 最终态 |'
        $md += '|---|---|---|---|---|---|---|---|'
        $i = 0
        foreach ($row in $rows) {
            $i++
            $base = $(if ($row.api_base) { "``$($row.api_base)``" } else { '**—（未填）**' })
            $md += "| $i | ``$($row.id)`` | $($row.protocol) | $(if ($row.protocol_implemented) { '✅ 已实现' } else { '⚠ 未实现' }) | $base | $(if ($row.declared_vision) { 'true' } else { 'false' }) | $(if ($row.verified) { 'true' } else { 'false' }) | $($row.final) |"
        }
        $md += ''
        $md += '**表 B · 实测证据与动作**'
        $md += ''
        $md += '| # | id | 文字理解 | 文字理解依据 | 图片理解 | 依据（图片理解） | ⚠ 冲突 | 下一动作 |'
        $md += '|---|---|---|---|---|---|---|---|'
        $i = 0
        foreach ($row in $rows) {
            $i++
            $cf = $(if (@($row.conflict).Count -gt 0) { (@($row.conflict) -join ' · ') } else { '—' })
            $tev = $(if ($row.text_evidence) { $row.text_evidence } else { '—' })
            $md += "| $i | ``$($row.id)`` | $($row.text) | $tev | $($row.image) | $($row.image_evidence) | $cf | $($row.next) |"
        }
        $md += ''
        $md += $rebuild
        return ($md -join "`r`n")
    }

    # ---------------- 合并一览（docs\loop_dev.md §2.2 哨兵块 / build\loop\SITE_STATUS.md）----------------
    if (-not $ForDocs) { $md += '# 站点状态台账（TST-L05 · 跨轮累积 · 机器生成）'; $md += '' }
    $md += $genLine
    $md += $legend
    $md += '> **判据**：**文字理解** = 「文本 → 取回文本」S3 三口径通过（``rc=0`` + ``n3=done`` + 正文非站侧错误串）；**图片理解** = 「图片 → 取回文本」链路（站点上传双证据 + 取回正文）。'
    $md += '> 两列是**链路**、不是模型强弱；' + $srcLine.Substring(2)
    $md += ''
    $md += "**汇总**：共 $($s.total) 站（web $($s.web) / official $($s.official)）· 文字理解 ✅ 实测 **$($s.text_tested)** · 图片理解 ✅ 实测 **$($s.image_tested)** · 🟡 就绪/声明 $($s.image_ready) · ⬜ 未接/未声明 $($s.image_none) · ⚠ 声明不一致 $($s.conflict)"
    $md += ''
    $md += '**主表（状态 · 跨轮）**'
    $md += ''
    $md += '| # | id | 类型 | 最近轮次 | S2 探测 | 输入命中/候选 | 登录证据 | S3 rc | n3 | 正文码点 | 最终态 | 下一动作 |'
    $md += '|---|---|---|---|---|---|---|---|---|---|---|---|'
    $i = 0
    foreach ($row in $Status.sites) {
        $i++
        $lat = $row.latest
        $md += "| $i | ``$($row.id)`` | $($row.type) | $(if ($lat) { $lat.round } else { '—' }) | $(if ($lat -and $lat.s2) { $lat.s2.state } else { '—' }) | $(if ($lat -and $lat.s2) { "$($lat.s2.hits)/$($lat.s2.candidates)" } else { '—' }) | $(if ($lat -and $lat.s2 -and $lat.s2.login) { $lat.s2.login } else { '—' }) | $(if ($lat -and $lat.s3) { $lat.s3.rc } else { '—' }) | $(if ($lat -and $lat.s3) { $lat.s3.n3 } else { '—' }) | $(if ($lat -and $lat.s3 -and $lat.s3.cps) { $lat.s3.cps } else { '—' }) | $($row.final) | $($row.next) |"
    }
    $md += ''
    $md += '**能力表（文字理解 / 图片理解 · 两列）**'
    $md += ''
    $md += '| id | 文字理解 | 图片理解 | 依据（图片理解） |'
    $md += '|---|---|---|---|'
    foreach ($row in $Status.sites) { $md += "| ``$($row.id)`` | $($row.text) | $($row.image) | $($row.image_evidence) |" }
    $md += ''
    $md += "> 重建（零副作用，随时可跑）：``$($Status.sources.rebuild)``"
    return ($md -join "`r`n")
}
function Write-SiteStatus {
    param([string]$Root, $Status, [string]$JsonPath, [string]$RoundDir = '', [bool]$UpdateDocs = $true, [string]$StatusDoc = '', [string]$StatusDocWeb = '', [string]$StatusDocApi = '')
    Write-JsonNoBom -Path $JsonPath -Obj $Status -Depth 10
    $mdText = Format-SiteStatusMd -Status $Status -ForDocs $false
    Set-Content -Path (Join-Path $Root 'build\loop\SITE_STATUS.md') -Value $mdText -Encoding UTF8
    if ($RoundDir -and (Test-Path $RoundDir)) { Set-Content -Path (Join-Path $RoundDir 'site_status.md') -Value $mdText -Encoding UTF8 }
    if ($UpdateDocs) {
        # 四个「人读位」由**同一次调用**（同一 $Status）写入 ⇒ 同源、不会漂移；各自取自己的 Scope 视图：
        #   ① docs\loop_dev.md §2.2   —— 合并一览（scope=all，全量 · 循环内嵌薄表）
        #   ② docs\AI_Status.md §二   —— 总览（scope=summary：汇总 + 指向 ③④，不重复列全量）
        #   ③ docs\AI_Status_web.md §二    —— 网页版分片（scope=web：链路证据 + 能力与接线）
        #   ④ docs\AI_Status_api.md §二    —— 官方 API 分片（scope=official：协议与就绪 + 证据与动作）
        # 幂等判据同 §2.2：块内时间戳只到**日期**粒度 ⇒ 同日重复跑内容逐字不变（`''` = 关闭该落点）。
        $targets = @([pscustomobject]@{ rel = 'docs\loop_dev.md'; label = 'docs\loop_dev.md §2.2（合并一览）'; scope = 'all' })
        if (-not [string]::IsNullOrWhiteSpace($StatusDoc))    { $targets += [pscustomobject]@{ rel = $StatusDoc;    label = "$StatusDoc §二（总览）";        scope = 'summary'  } }
        if (-not [string]::IsNullOrWhiteSpace($StatusDocWeb)) { $targets += [pscustomobject]@{ rel = $StatusDocWeb; label = "$StatusDocWeb §二（web 分片）"; scope = 'web'      } }
        if (-not [string]::IsNullOrWhiteSpace($StatusDocApi)) { $targets += [pscustomobject]@{ rel = $StatusDocApi; label = "$StatusDocApi §二（official 分片）"; scope = 'official' } }
        foreach ($t in $targets) {
            $block = Format-SiteStatusMd -Status $Status -ForDocs $true -Scope $t.scope
            $changed = Update-DocsSiteStatus -Root $Root -BlockText $block -RelPath $t.rel
            if ($changed) { Write-Host "[loop] $($t.label) 哨兵块已回写（内容有变）" }
            else { Write-Host "[loop] $($t.label) 哨兵块无变化（幂等）" }
        }
    }
    return $JsonPath
}
function Update-DocsSiteStatus {
    param([string]$Root, [string]$BlockText, [string]$RelPath = 'docs\loop_dev.md')
    $path = Join-Path $Root $RelPath
    if (-not (Test-Path $path)) { Write-Host "[loop] ⚠ 未找到 $RelPath ⇒ 跳过文档回写"; return $false }
    $text  = [System.IO.File]::ReadAllText($path)
    $begin = '<!-- SITE-STATUS:BEGIN'
    $end   = '<!-- SITE-STATUS:END -->'
    $i = $text.IndexOf($begin)
    $j = $text.IndexOf($end)
    if ($i -lt 0 -or $j -lt 0 -or $j -lt $i) { Write-Host "[loop] ⚠ $RelPath 未找到台账哨兵块（$begin … $end）⇒ 跳过文档回写"; return $false }
    $head = $begin + ' （TST-L05 机器生成 · 人工只审阅 · 改数据请改 providers.json / site_capability_evidence.json） -->'
    $new  = $text.Substring(0, $i) + $head + "`r`n" + $BlockText.TrimEnd() + "`r`n" + $text.Substring($j)
    if ($new -eq $text) { return $false }
    [System.IO.File]::WriteAllText($path, $new, (New-Object System.Text.UTF8Encoding($false)))
    return $true
}

# -StatusOnly：只重建台账（零副作用）——不建轮次目录、不跑门禁/探测/S3、不起浏览器
if ($statusOnly) {
    $outPath = $(if ([string]::IsNullOrWhiteSpace($SiteStatusJson)) { Join-Path $Root 'build\loop\SITE_STATUS.json' } else { $SiteStatusJson })
    New-Item -ItemType Directory -Force -Path (Split-Path $outPath -Parent) | Out-Null
    $st = New-SiteStatus -Root $Root -IncludeCurrent $false
    Write-SiteStatus -Root $Root -Status $st -JsonPath $outPath -UpdateDocs $UpdateDocs.IsPresent -StatusDoc $StatusDoc -StatusDocWeb $StatusDocWeb -StatusDocApi $StatusDocApi | Out-Null
    Write-Host "[loop] 台账已重建：$outPath"
    Write-Host "[loop]   跨轮 $($st.summary.rounds) 轮 · 站 $($st.summary.total)（web $($st.summary.web) / official $($st.summary.official)）· 文字理解 ✅ $($st.summary.text_tested) · 图片理解 ✅ $($st.summary.image_tested) · ⚠ 不一致 $($st.summary.conflict)"
    if ($UpdateDocs.IsPresent) {
        $locs = @('docs\loop_dev.md §2.2（合并一览）')
        $locs += $(if ([string]::IsNullOrWhiteSpace($StatusDoc))    { '(总览页未启用)' }    else { "$StatusDoc §二（汇总 + 分片链接）" })
        $locs += $(if ([string]::IsNullOrWhiteSpace($StatusDocWeb)) { '(web 分片未启用)' }  else { "$StatusDocWeb §二（web 分片）" })
        $locs += $(if ([string]::IsNullOrWhiteSpace($StatusDocApi)) { '(official 分片未启用)' } else { "$StatusDocApi §二（official 分片）" })
        Write-Host "[loop]   四处落点（同一次调用写入 · 同源）：$($locs -join ' · ') · build\loop\SITE_STATUS.md"
    }
    exit 0
}



# -------------------------------------------------------------- ④ 报告 ----
$s3ById = @{}
foreach ($s in $report.sites) { $s3ById[$s.id] = $s }
$matrix = @()
foreach ($site in $sites) {
    $s3 = $s3ById[$site.id]
    if ($s3) { $final = $s3.state }
    elseif ($site.state -eq '可用') { $final = '未测（S3 未跑）' }
    elseif ($site.needs_login) { $final = '需登录' }
    else { $final = '失败（探测级 · §5.1③）' }
    $matrix += [ordered]@{
        id = $site.id; display = $site.display; probe = $site.state; probe_hits = $site.input_hits
        probe_candidates = $site.input_candidates; login_evidence = $site.login_evidence
        attempts = $site.attempts; s3_rc = $(if ($s3) { $s3.rc } else { $null })
        state = $final; url = $site.url
    }
}
# S2 里没有的站（例如 -SkipProbe + -Providers）
foreach ($id in $targets) {
    if (-not ($matrix | Where-Object { $_.id -eq $id })) {
        $s3 = $s3ById[$id]
        $matrix += [ordered]@{ id = $id; display = ''; probe = '未测'; probe_hits = $null; probe_candidates = $null
            login_evidence = ''; attempts = $null; s3_rc = $s3.rc; state = $s3.state; url = '' }
    }
}
$report.matrix = $matrix
$gateFail = @($report.gates | Where-Object { $_.verdict -ne 'PASS' })
$s3Fail   = @($report.sites | Where-Object { $_.state -like '失败*' })
$fakePass = @($report.sites | Where-Object { $_.rc -eq 0 -and $_.n3 -and $_.n3 -ne 'done' })
$s3Untested = @($report.sites | Where-Object { $_.state -like '未测*' })
$busySites = @($report.sites | Where-Object { $_.busy })
$needLogin = @($matrix | Where-Object { $_.state -eq '需登录' })
$usable   = @($matrix | Where-Object { $_.state -eq '可用（匿名）' })
$report.needs_login = @($needLogin | ForEach-Object { $_.id })
$report.summary = [ordered]@{
    total = $matrix.Count; usable_anon = $usable.Count; need_login = $needLogin.Count
    fail = @($matrix | Where-Object { $_.state -like '失败*' }).Count
    untested = @($matrix | Where-Object { $_.state -like '未测*' }).Count
    gates_pass = @($report.gates | Where-Object { $_.verdict -eq 'PASS' }).Count; gates_fail = $gateFail.Count
    s3_fail = $s3Fail.Count; rc0_but_not_done = $fakePass.Count; busy = $busySites.Count
}
$report.probe_exit = $(if ($report.Contains('probe_exit')) { $report.probe_exit } else { $null })

# --------------------------------------------- ①′ 跨轮 diff（TST-L03 · 对齐 --json-diff 口径）----
# 对比 = build/loop/*/report.json 的 matrix[].state（与台账 history[] 同一口径）：
#   状态变化（prev → cur）/ 新增（上一份无此站）/ 消失（本轮未探测）。
# 上一份 = 时间戳最新的那份；首次跑（无上一份）⇒ 本轮即基线，不产生 diff 行。
$report.diff = [ordered]@{ prev_dir = ''; prev_has = $false; changed = @(); added = @(); gone = @() }
$loopRoot = Join-Path $Root 'build\loop'
if (Test-Path $loopRoot) {
    $prev = @(Get-ChildItem $loopRoot -Directory -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -match '-\d{8}-\d{4}$' -and $_.FullName -ne $dir } |
              Sort-Object { $_.Name.Substring($_.Name.Length - 13) } |
              Select-Object -Last 1)
    if ($prev.Count -gt 0) {
        $report.diff.prev_dir = $prev[0].Name
        $prevJsonPath = Join-Path $prev[0].FullName 'report.json'
        if (Test-Path $prevJsonPath) {
            try {
                $prevByState = @{}
                foreach ($pm in @(([System.IO.File]::ReadAllText($prevJsonPath) | ConvertFrom-Json).matrix)) {
                    $prevByState[[string]$pm.id] = [string]$pm.state
                }
                if ($prevByState.Count -gt 0) {
                    $report.diff.prev_has = $true
                    $curIds = @($matrix | ForEach-Object { [string]$_.id })
                    foreach ($m in $matrix) {
                        $id = [string]$m.id
                        if ($prevByState.ContainsKey($id) -and $prevByState[$id] -ne [string]$m.state) {
                            $report.diff.changed += [ordered]@{ id = $id; prev = $prevByState[$id]; state = [string]$m.state; url = [string]$m.url }
                        }
                    }
                    $report.diff.added = @($matrix | Where-Object { -not $prevByState.ContainsKey([string]$_.id) } | ForEach-Object { [string]$_.id })
                    $report.diff.gone  = @($prevByState.Keys | Where-Object { $curIds -notcontains [string]$_ } | ForEach-Object { [string]$_ })
                }
            } catch { Write-Host "[loop] ⚠ 跨轮 diff 跳过：上一份报告无法解析 $prevJsonPath" }
        }
    }
}


# --------------------------------------------- ⑤ 台账数据（先算，④ 块引用）----
$siteStatus = New-SiteStatus -Root $Root -IncludeCurrent $true -CurMatrix $matrix -CurSites $report.sites -RoundLabel $Round -Stamp $report.started

$md = @()
$md += "# 循环报告 · $Round（$stamp）"
$md += ''
$md += "> 物证目录：``$dir``（易失；结论权威位见 docs/loop_dev.md §7）"
$md += '> 生成：``source/tools/loop_verify.ps1``（TST-L01 · 报告契约见 loop_dev §6.1：四块 + ①′ 跨轮 diff）'
$md += ''
$md += '## ① 本轮数字表'
$md += ''
$md += '| 项 | 数字 | 依据 |'
$md += '|---|---|---|'
$md += "| 门禁 PASS / FAIL | $($report.summary.gates_pass) / $($report.summary.gates_fail) | ``gate/``（逐条见 ``report.json`` 的 ``gates[]``） |"
$md += "| S2 探测 | 可用 $(@($sites | Where-Object { $_.state -eq '可用' }).Count) · 需登录 $(@($sites | Where-Object { $_.needs_login }).Count) · 失败 $(@($sites | Where-Object { $_.state -eq '失败' }).Count)（共 $($sites.Count) 站） | ``probe.txt`` / ``probe.json`` |"
$md += "| S3 端到端 | 跑 $($report.sites.Count) 站 · 可用（匿名）$($usable.Count) · 失败（含「rc=0 但生成未成功」）$($s3Fail.Count) | ``raw/<id>.run*.txt`` + ``raw/<id>.run*.workflow.json`` |"
$md += "| ⚠ rc=0 但 n3≠done（退出码未区分） | $($fakePass.Count) 站 | loop_dev §10.1 陷阱 3 |"
$md += "| ⚠ n3=done 但正文＝站侧错误串（未真生成 ⇒ 判失败） | $($busySites.Count) 站 | loop_dev §10.1 陷阱 6 |"
$md += "| stdout 丢失的轮次 | $(@($report.sites | Where-Object { $_.stdout_lost }).Count) 站（**不影响退出码判据**） | 见 §10.1 陷阱 2 |"
$md += "| 站点台账（跨轮） | 覆盖 $($siteStatus.summary.rounds) 轮 · 文字理解 ✅ $($siteStatus.summary.text_tested) · 图片理解 ✅ $($siteStatus.summary.image_tested) · ⚠ 声明不一致 $($siteStatus.summary.conflict) | ``site_status.md`` / ``build/loop/SITE_STATUS.json`` / ``docs/AI_Status.md`` |"

$md += ''
$md += '## ①′ 跨轮 diff（对齐 ``--json-diff`` 口径 · 状态变化 / 新增 / 消失）'
$md += ''
if ($report.diff.prev_has) {
    $md += "> 上一份：``$($report.diff.prev_dir)``（report.json 的 ``matrix[].state``；无变化不列出）"
    if (@($report.diff.changed).Count -eq 0 -and @($report.diff.added).Count -eq 0 -and @($report.diff.gone).Count -eq 0) {
        $md += '- 与本轮**逐站一致**（无状态变化 · 无新增 · 无消失）'
    } else {
        foreach ($d in @($report.diff.changed)) { $md += "- 状态变化 ``$($d.id)``：$($d.prev) → $($d.state)$(if ($d.url) { '（URL 变化：' + $d.url + '）' })" }
        foreach ($a in @($report.diff.added))    { $md += "- 新增 ``$a``（上一份无此站）" }
        foreach ($g in @($report.diff.gone))     { $md += "- 消失 ``$g``（本轮未探测；历史物证仍在 ``build/loop/<轮次>/report.json``）" }
    }
} else {
    $md += '- 无上一份报告（build/loop 下未找到可对比的 report.json）⇒ 本轮即基线，下一轮起输出跨轮 diff'
}

$md += ''
$md += '## ② 站点矩阵（探测态 → S3 端到端 → 最终态）'
$md += ''
$md += '| # | 站 | 探测态 | 输入命中/候选 | 登录证据 | 尝试 | S3 rc | n3（LLMGenerate） | 最终态 |'
$md += '|---|---|---|---|---|---|---|---|---|'
$i = 0
foreach ($m in $matrix) {
    $i++
    $s3 = $s3ById[$m.id]
    $md += "| $i | ``$($m.id)`` $($m.display) | $($m.probe) | $($m.probe_hits)/$($m.probe_candidates) | $(if ($m.login_evidence) { $m.login_evidence } else { '无' }) | $(if ($m.attempts) { $m.attempts } else { '-' }) | $(if ($null -ne $m.s3_rc) { $m.s3_rc } else { '-' }) | $(if ($s3 -and $s3.n3) { $s3.n3 } else { '-' }) | $($m.state) |"
}
$md += ''
$md += '## ③ 需人工清单（needs_login[] + 待人工项）'
$md += ''
if ($needLogin.Count -eq 0) { $md += '- （本轮无「需登录」站）' }
foreach ($n in $needLogin) {
    $md += "- ``$($n.id)``：探测判据 = 输入区候选 $($n.probe_candidates) · 登录控件证据 ``$($n.login_evidence)`` ⇒ **人工在窗口内登录一次后重跑**：``aiwrite.exe --run-selftest --web --provider $($n.id)``"
}
foreach ($f in $report.sites | Where-Object { $_.state -like '失败*' }) {
    $md += "- ``$($f.id)``（S3 rc=$($f.rc) · n3=$($f.n3)$(if ($f.n3_source) { " · 来源=$($f.n3_source)" })$(if ($f.stdout_lost) { ' · stdout 丢失' })）：$($f.state) —— 复现 ``aiwrite.exe --run-selftest --web --provider $($f.id)`` · 只读诊断 ``aiwrite.exe --web-adapter-selftest --provider $($f.id)`` / ``--web-dom-dump --provider $($f.id)`` · 按 loop_dev §5.1 归类后再决定动作"
}
foreach ($f in $s3Untested) {
    $md += "- ``$($f.id)``（S3 rc=$($f.rc)）：$($f.state) —— 复现 ``aiwrite.exe --run-selftest --web --provider $($f.id)``"
}
foreach ($n in $report.notes) { $md += "- 备注：$n" }
$md += ''
$md += '## 附录 · VA-07 诊断抽样（每站一行，便于跨轮比对取样口径）'
$md += ''
foreach ($s in $report.sites) {
    if ($s.va07) { $md += "- ``$($s.id)``：$($s.va07)" }
    else { $md += "- ``$($s.id)``：（stdout 丢失 ⇒ 以**离线复算**替代）$(if ($s.offline) { $s.offline } else { '（复算也缺 ⇒ 仅以退出码 + JSON 物证判定，见 raw/' + $s.id + '.run*.workflow.json）' })" }
    if ($s.va07 -and $s.offline) { $md += "  - 交叉核对：$($s.offline)" }
    if ($s.busy) { $md += "  - ⚠ 归档正文（$($s.body_cps) 码点）＝站侧错误串「$($s.busy)」⇒ **未真生成**（§10.1 陷阱 6）；处置：重跑本站（繁忙多为瞬时），仍复现则按 §5.1② 复核站点选择器 / 限流。" }
}
$md += ''
$md += '## ④ 站点台账（跨轮累积 · 机器维护；**分通道两张表**见 `docs/AI_Status_web.md` §二 / `docs/AI_Status_api.md` §二，合并一览见 `site_status.md`）'
$md += ''
$md += "> 覆盖轮次 $($siteStatus.summary.rounds)：$((@($siteStatus.rounds | ForEach-Object { $_.round })) -join ' · ')"
$md += "> 汇总：共 $($siteStatus.summary.total) 站（web $($siteStatus.summary.web) / official $($siteStatus.summary.official)）· 文字理解 ✅ 实测 $($siteStatus.summary.text_tested) · 图片理解 ✅ 实测 $($siteStatus.summary.image_tested) · 🟡 就绪/声明 $($siteStatus.summary.image_ready) · ⬜ 未接/未声明 $($siteStatus.summary.image_none) · ⚠ 声明不一致 $($siteStatus.summary.conflict)"
$md += ''
$md += '| 站 | 文字理解 | 图片理解 | 下一动作 |'
$md += '|---|---|---|---|'
foreach ($row in $siteStatus.sites) { $md += "| ``$($row.id)`` | $($row.text) | $($row.image) | $($row.next) |" }

Set-Content -Path (Join-Path $dir 'report.md') -Value $md -Encoding UTF8
$report.finished = (Get-Date).ToString('s')
$exit = 0
if ($gateFail.Count -gt 0 -or $s3Fail.Count -gt 0 -or $s3Untested.Count -gt 0) { $exit = 1 }
elseif ($needLogin.Count -gt 0) { $exit = 3 }
$report.exit_code = $exit
Write-JsonNoBom -Path (Join-Path $dir 'report.json') -Obj $report -Depth 8
# -------------------------------------------------------------- ⑤ 台账 ----
$siteStatusPath = $(if ([string]::IsNullOrWhiteSpace($SiteStatusJson)) { Join-Path $Root 'build\loop\SITE_STATUS.json' } else { $SiteStatusJson })
Write-SiteStatus -Root $Root -Status $siteStatus -JsonPath $siteStatusPath -RoundDir $dir -UpdateDocs $UpdateDocs.IsPresent -StatusDoc $StatusDoc -StatusDocWeb $StatusDocWeb -StatusDocApi $StatusDocApi | Out-Null
Write-Host "[loop] 台账：$siteStatusPath（跨轮 $($siteStatus.summary.rounds) 轮 · 文字理解 ✅ $($siteStatus.summary.text_tested) · 图片理解 ✅ $($siteStatus.summary.image_tested) · ⚠ 不一致 $($siteStatus.summary.conflict)）"


Write-Host ''
Write-Host "[loop] 轮次 $Round 结束：跑 $($report.sites.Count) 站 · 可用（匿名）$($usable.Count) · 失败 $($s3Fail.Count) · 未测 $($s3Untested.Count)（其中 rc=0 但 n3≠done $($fakePass.Count) · n3=done 但正文＝站侧错误串 $($busySites.Count)）· 门禁 FAIL $($gateFail.Count) · 需登录 $($needLogin.Count) ⇒ 退出码 $exit"
Write-Host "[loop] 报告：$(Join-Path $dir 'report.md')"
exit $exit
