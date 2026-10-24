# End-to-end smoke test on Windows: broker + subscriber + publisher round trip.
# Called by make.bat after a build, and directly by CI.
#
# Usage: ci\smoke-test.ps1 [-BinDir <dir holding scadup.exe>]
# Exits 0 when one publish() reaches one live subscriber: the broker counts a
# subscriber as reached only after the write to its socket succeeded, so the
# publisher's own report is a reliable end-to-end signal.
param(
    [string]$BinDir = ""
)

$ErrorActionPreference = "Stop"

# Locate the tool: the layout depends on the generator -- Visual Studio nests
# it under Release\, Ninja and Make do not. Try the known layouts, then search.
$candidates = @()
if ($BinDir) { $candidates += (Join-Path $BinDir "scadup.exe") }
$candidates += @(
    "build\test\Release\scadup.exe",
    "build\test\scadup.exe"
)
$exe = $null
foreach ($c in $candidates) {
    if (Test-Path $c) { $exe = (Get-Item $c).FullName; break }
}
if (-not $exe) {
    $found = Get-ChildItem -Path build -Recurse -Filter scadup.exe -File -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -First 1
    if ($found) { $exe = $found.FullName }
}
if (-not $exe) {
    Write-Host "smoke: FAILED, scadup.exe not found under build\ (tried: $($candidates -join ', '))"
    Get-ChildItem -Path build -Recurse -Filter *.exe -File -ErrorAction SilentlyContinue |
        Select-Object -First 5 -ExpandProperty FullName | Write-Host
    exit 1
}

# Private dir with a loopback cfg: the repo's own cfg may point at a LAN
# broker, and a random port keeps a leftover broker from breaking the run.
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("scadup-smoke-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$port = Get-Random -Minimum 20000 -Maximum 40000
Set-Content -Path (Join-Path $work "scadup.cfg") -Value "IP=127.0.0.1`nPORT=$port`n" -NoNewline

$topic = "1234"   # parsed as hex by the tool: 0x1234
$payload = "smoke-test"

function Stop-Leftovers {
    Stop-Process -Id $broker.Id -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $subscriber.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

$broker = Start-Process -FilePath $exe -ArgumentList "1" `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput "$work\broker.log" -RedirectStandardError "$work\broker.err"

# Wait for the listener, then watch it briefly: a broker that could not bind
# dies within moments, and a stranger answering on the port must not pass for
# it. (The log line cannot be waited for: stdout is redirected to a file, so
# the broker's printf output stays buffered until it exits.)
$ready = $false
for ($i = 0; $i -lt 50; $i++) {
    if ($broker.HasExited) { break }
    $probe = New-Object System.Net.Sockets.TcpClient
    try {
        $probe.Connect("127.0.0.1", $port)
        $ready = $true
    } catch {
        # not listening yet
    } finally {
        $probe.Close()
    }
    if ($ready) { break }
    Start-Sleep -Milliseconds 100
}
if ($ready) {
    for ($i = 0; $i -lt 10; $i++) {
        if ($broker.HasExited) { $ready = $false; break }
        Start-Sleep -Milliseconds 100
    }
}
if (-not $ready) {
    Write-Host "smoke: FAILED, broker did not start on port ${port}:"
    Get-Content "$work\broker.log" -Raw -ErrorAction SilentlyContinue | Write-Host
    Stop-Process -Id $broker.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

$subscriber = Start-Process -FilePath $exe -ArgumentList "2", $topic `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput "$work\subscriber.log" -RedirectStandardError "$work\subscriber.err"
Start-Sleep -Seconds 1

# Runs to completion and flushes its own report on exit.
$publisher = Start-Process -FilePath $exe -ArgumentList "3", $topic, $payload `
    -WorkingDirectory $work -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput "$work\publisher.log" -RedirectStandardError "$work\publisher.err"
if (-not $publisher.WaitForExit(30000)) {
    Write-Host "smoke: FAILED, publisher did not exit"
    Stop-Leftovers
    exit 1
}

# Stop the broker only: the subscriber then sees the connection end and exits
# on its own, flushing its log; TerminateProcess would cut it short.
Stop-Process -Id $broker.Id -Force -ErrorAction SilentlyContinue
$subscriber.WaitForExit(10000) | Out-Null
if (-not $subscriber.HasExited) {
    Stop-Process -Id $subscriber.Id -Force -ErrorAction SilentlyContinue
    Write-Host "smoke: note -- subscriber force-killed, its log may be truncated"
}

# Read the logs only now: the subscriber flushes its stdio when it exits.
$out = Get-Content "$work\publisher.log" -Raw -ErrorAction SilentlyContinue
$subOut = Get-Content "$work\subscriber.log" -Raw -ErrorAction SilentlyContinue
Write-Host $out
Write-Host "--- subscriber log ---"
Write-Host $subOut

$status = 0
if ($out -notmatch "delivered to 1 subscriber") {
    Write-Host "smoke: FAILED, no delivery report from the publisher"
    $status = 1
}
if ($subOut -notmatch $payload) {
    Write-Host "smoke: FAILED, subscriber never logged the payload"
    $status = 1
}
if ($status -ne 0) {
    Write-Host "--- broker log ---"
    Get-Content "$work\broker.log" -Raw -ErrorAction SilentlyContinue | Write-Host
}

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($status -eq 0) { Write-Host "smoke: OK (topic 0x$topic, delivered to 1 subscriber)" }
exit $status
