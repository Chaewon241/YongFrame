# Game load re-measurement runner (collision / zone / respawn / speed 360). Plan: docs/stage4.md "load re-measurement plan".
# Usage (from the repository root, Release build, nothing else heavy running):
#   powershell -ExecutionPolicy Bypass -File results\stage4\run-load-recheck.ps1
#   powershell -ExecutionPolicy Bypass -File results\stage4\run-load-recheck.ps1 -RoomsA 300,450 -RoomsB 0
# Condition A: 0 structures per room. Condition B: 20 prebuilt houses per room (--bench-prebuilt-structures 20).
# Each run starts a fresh server, runs game_load_client (4 threads, warmup 5 s, measure 30 s, drain 3 s), stops the server
# and gzips its log. ASCII only on purpose (Windows PowerShell 5.1 reads BOM-less files as ANSI).
param(
  [int[]]$RoomsA = @(100, 400, 500, 600),
  [int[]]$RoomsB = @(500),
  [int]$Reps = 3,
  [int]$Port = 17900
)
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$raw = Join-Path $root 'results\stage4\raw'
$server = Join-Path $root 'build\windows-local\Release\game_server.exe'
$client = Join-Path $root 'build\windows-local\Release\game_load_client.exe'
if (-not (Test-Path $server) -or -not (Test-Path $client)) { throw 'Release build not found. Build local-release first.' }

function Gzip-File([string]$path) {
  $in = [IO.File]::OpenRead($path)
  $out = [IO.File]::Create($path + '.gz')
  $gz = New-Object IO.Compression.GzipStream($out, [IO.Compression.CompressionMode]::Compress)
  $in.CopyTo($gz); $gz.Close(); $out.Close(); $in.Close()
  Remove-Item $path
}

function Run-One([string]$cond, [int]$rooms, [int]$rep, [int]$prebuilt, [string]$csv) {
  $label = "recheck-$cond-r$rooms-rep$rep"
  $log = Join-Path $raw "game-server-$label.log"
  $srvArgs = @('--port', "$Port", '--log-level', 'info', '--stats-interval', '1')
  if ($prebuilt -gt 0) { $srvArgs += @('--bench-prebuilt-structures', "$prebuilt") }
  $srv = Start-Process -FilePath $server -ArgumentList $srvArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
  Start-Sleep -Seconds 2
  $cliArgs = @('--port', "$Port", '--rooms', "$rooms", '--threads', '4', '--warmup', '5', '--duration', '30',
               '--server-pid', "$($srv.Id)", '--label', $label, '--csv', $csv)
  $cli = Start-Process -FilePath $client -ArgumentList $cliArgs -PassThru -Wait -WindowStyle Hidden -RedirectStandardOutput (Join-Path $raw "game-$label.txt") -RedirectStandardError (Join-Path $raw "game-$label.err")
  $code = $cli.ExitCode
  if (-not $srv.HasExited) { Stop-Process -Id $srv.Id -Force }
  Start-Sleep -Seconds 2
  Gzip-File $log
  Add-Content -Path (Join-Path $raw 'game-load-recheck-progress.txt') -Value "$label exit=$code" -Encoding UTF8
  Write-Output "$label exit=$code"
  Start-Sleep -Seconds 4
}

# environment record
$envFile = Join-Path $raw 'game-load-recheck-environment.txt'
$os = (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + (Get-CimInstance Win32_OperatingSystem).Version
$cpu = (Get-CimInstance Win32_Processor).Name
$ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
$commit = (& git rev-parse --short HEAD)
$dirty = (& git status --short | Measure-Object).Count
$bg = (Get-Process | Sort-Object CPU -Descending | Select-Object -First 8 | ForEach-Object { $_.ProcessName }) -join ', '
$lines = @("OS: $os", "CPU: $cpu", "RAM: $ram GB", 'Build: Release (MSVC 14.44, VS 2022)', "Commit: $commit (uncommitted changed files: $dirty)",
  'Server: --log-level info --stats-interval 1, 1 io_context thread, TCP_NODELAY on, memory storage, PlayConfig defaults (speed 360, build zone 1200)',
  'Generator: game_load_client --threads 4, same PC loopback, 4 players per room, MoveInput 10 Hz per player, warmup 5 s, measure 30 s, drain 3 s',
  "Top CPU-time background processes at start: $bg")
Set-Content -Path $envFile -Value $lines -Encoding UTF8

foreach ($r in $RoomsA) { if ($r -le 0) { continue }; for ($i = 1; $i -le $Reps; $i++) { Run-One 'A' $r $i 0 (Join-Path $raw 'game-load-recheck.csv') } }
foreach ($r in $RoomsB) { if ($r -le 0) { continue }; for ($i = 1; $i -le $Reps; $i++) { Run-One 'B' $r $i 20 (Join-Path $raw 'game-load-recheck-structures.csv') } }
Write-Output 'DONE'
