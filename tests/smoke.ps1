# MCP stdio 冒烟测试：不打开任何 GUI，直接把报文喂给进程、看响应。
#
# 用法：
#   .\tests\smoke.ps1
#   .\tests\smoke.ps1 -Exe D:/Code/MCP/build/Release/weather-mcp.exe
#
# 两个 Windows 上的坑，这里都绕开了：
#   1. 不用 PowerShell 管道。PS 5.1 往原生命令走管道时在 Windows 上会有编码问题，
#      这里直接往子进程 stdin 的 BaseStream 写 UTF-8 无 BOM 字节，绕过编码层。
#   2. stdout / stderr 异步并发读再 WaitForExit，否则缓冲区写满会死锁。

param(
    [string]$Exe = (Join-Path $PSScriptRoot "..\build\Release\weather-mcp.exe"),
    [string]$Requests = (Join-Path $PSScriptRoot "requests.txt"),
    [switch]$SkipAssert
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
    Write-Host "FAIL 找不到可执行文件: $Exe（先执行 cmake --build build --config Release）" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $Requests)) {
    Write-Host "FAIL 找不到请求文件: $Requests" -ForegroundColor Red
    exit 1
}

$Exe = (Resolve-Path $Exe).Path
$Requests = (Resolve-Path $Requests).Path

# 注意必须指定 UTF8：PS 5.1 默认按系统 ANSI(GBK) 读取文本文件，
# 中文城市名会被读错，再以 UTF-8 写进 stdin 就成了双重乱码。
$lines = Get-Content -Path $Requests -Encoding UTF8 | Where-Object { $_.Trim().Length -gt 0 }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
try {
    $psi.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $psi.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)
} catch {
    # 老版本 .NET 没有这两个属性，忽略即可
}

Write-Host "=== 启动 $Exe ===" -ForegroundColor Cyan
$proc = [System.Diagnostics.Process]::Start($psi)

$stdoutTask = $proc.StandardOutput.ReadToEndAsync()
$stderrTask = $proc.StandardError.ReadToEndAsync()

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
foreach ($line in $lines) {
    $bytes = $utf8NoBom.GetBytes($line + "`n")
    $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
}
$proc.StandardInput.BaseStream.Flush()
$proc.StandardInput.Close()

# 给子进程留出处理时间，然后确认它随 stdin 关闭而退出
if (-not $proc.WaitForExit(15000)) {
    Write-Host "FAIL 进程未退出（可能没有把 stdin EOF 当作结束信号）" -ForegroundColor Red
    $proc.Kill()
    exit 1
}

$stdout = $stdoutTask.Result
$stderr = $stderrTask.Result

Write-Host "--- stderr (日志) ---" -ForegroundColor DarkGray
Write-Host $stderr -ForegroundColor DarkGray

Write-Host "--- stdout (协议响应) ---" -ForegroundColor Cyan
Write-Host $stdout

# ---------------------------------------------------------------- 断言

$failures = @()

$responses = @()
foreach ($rawLine in ($stdout -split "`r?`n")) {
    if ($rawLine.Trim().Length -eq 0) { continue }
    try {
        $responses += ($rawLine | ConvertFrom-Json)
    } catch {
        $failures += "stdout 里有无法解析为 JSON 的行: $rawLine"
    }
}

if ($SkipAssert) {
    Write-Host "`n=== 跳过断言（仅打印响应，用于临时观察工具输出）===" -ForegroundColor Yellow
    exit 0
}

Write-Host "--- 校验 ---" -ForegroundColor Cyan

# 9 行请求里：1 条通知(不回)、1 行坏 JSON(跳过) → 应有 7 条响应
if ($responses.Count -ne 7) {
    $failures += "响应条数应为 7，实际为 $($responses.Count)"
}

function Get-Response($id) {
    return ($responses | Where-Object { $_.id -eq $id } | Select-Object -First 1)
}

# id=1 initialize
$init = Get-Response 1
if (-not $init) {
    $failures += "缺少 id=1 的 initialize 响应"
} elseif (-not $init.result.protocolVersion) {
    $failures += "initialize 响应缺少 result.protocolVersion"
} elseif ($init.result.protocolVersion -ne '2025-11-25') {
    # 客户端要的版本我们支持，就必须原样回它。回落成别的版本 =
    # 客户端报 "Server's protocol version is not supported"。
    $failures += "initialize 应原样回客户端请求的版本 2025-11-25，实际: $($init.result.protocolVersion)"
} else {
    Write-Host "OK  initialize -> protocolVersion=$($init.result.protocolVersion), server=$($init.result.serverInfo.name)" -ForegroundColor Green
}

# 通知不应产生任何响应：id=2 是 tools/list，不是通知
$notif = ($responses | Where-Object { $_.id -eq $null })
if ($notif) {
    $failures += "notifications/initialized 不该产生响应，但出现了 id=$($notif.id) 的响应"
}

# id=2 tools/list
$list = Get-Response 2
if (-not $list) {
    $failures += "缺少 id=2 的 tools/list 响应"
} elseif (-not ($list.result.tools | Where-Object { $_.name -eq 'get_weather' })) {
    $failures += "tools/list 里没有找到 get_weather"
} else {
    $toolNames = ($list.result.tools | ForEach-Object { $_.name }) -join ', '
    Write-Host "OK  tools/list -> tools=[$toolNames]" -ForegroundColor Green
}

# id=3 tools/call 成功路径
$call = Get-Response 3
if (-not $call) {
    $failures += "缺少 id=3 的 tools/call 响应"
} elseif ($call.error) {
    $failures += "tools/call(get_weather) 返回了协议错误: $($call.error.code) $($call.error.message)"
} elseif (-not $call.result.content) {
    $failures += "tools/call 的 result 缺少 content 数组"
} else {
    Write-Host "OK  tools/call -> isError=$($call.result.isError), text=$($call.result.content[0].text)" -ForegroundColor Green
}

# id=4 未知工具：协议错误 -32602
$badTool = Get-Response 4
if (-not $badTool.error -or $badTool.error.code -ne -32602) {
    $failures += "未知工具应返回 JSON-RPC error -32602，实际: $($badTool.error.code)"
} else {
    Write-Host "OK  未知工具 -> error $($badTool.error.code): $($badTool.error.message)" -ForegroundColor Green
}

# id=5 未知方法：协议错误 -32601
$badMethod = Get-Response 5
if (-not $badMethod.error -or $badMethod.error.code -ne -32601) {
    $failures += "未知方法应返回 JSON-RPC error -32601，实际: $($badMethod.error.code)"
} else {
    Write-Host "OK  未知方法 -> error $($badMethod.error.code): $($badMethod.error.message)" -ForegroundColor Green
}

# id=6 initialize 协商：客户端要旧版本，必须回旧版本，不能自作主张升级
$initOld = Get-Response 6
if (-not $initOld) {
    $failures += "缺少 id=6 的 initialize 响应"
} elseif ($initOld.result.protocolVersion -ne '2025-03-26') {
    $failures += "客户端请求 2025-03-26 时应原样回 2025-03-26，实际: $($initOld.result.protocolVersion)"
} else {
    Write-Host "OK  版本协商命中 -> 2025-03-26" -ForegroundColor Green
}

# id=7 initialize 协商：客户端要的版本我们不支持，回落到服务端最新版
$initBad = Get-Response 7
if (-not $initBad) {
    $failures += "缺少 id=7 的 initialize 响应"
} elseif (-not $initBad.result.protocolVersion -or $initBad.result.protocolVersion -eq '1999-01-01') {
    $failures += "客户端请求不支持的版本时应回落，实际: $($initBad.result.protocolVersion)"
} else {
    Write-Host "OK  版本协商回落 -> $($initBad.result.protocolVersion)" -ForegroundColor Green
}

if ($failures.Count -gt 0) {
    Write-Host "`n=== FAILED ===" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
    exit 1
}

Write-Host "`n=== ALL PASSED ===" -ForegroundColor Green
exit 0
