# Per-room CPU decomposition runner. Plan: docs/stage4.md "CPU decomposition experiment plan".
# Usage (repository root, Release build, nothing else heavy running):
#   powershell -ExecutionPolicy Bypass -File results\stage4\run-load-decompose.ps1
# Conditions at a fixed room count: X0 baseline, X1 speed 300, X2 no collision, X3 speed 300 + no collision.
# ASCII only on purpose (Windows PowerShell 5.1 reads BOM-less files as ANSI).
param(
  [int]$Rooms = 400,
  [int]$Reps = 3,
  [int]$Port = 17900,
  [string[]]$Conditions = @('X0', 'X1', 'X2', 'X3'),
  [string]$Tag = 'decomp'
)
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$raw = Join-Path $root 'results\stage4\raw'
$server = Join-Path $root 'build\windows-local\Release\game_server.exe'
$client = Join-Path $root 'build\windows-local\Release\game_load_client.exe'
if (-not (Test-Path $server) -or -not (Test-Path $client)) { throw 'Release build not found.' }

$extra = @{
  'X0' = @();
  'X1' = @('--bench-move-speed', '300');
  'X2' = @('--bench-no-collision', 'on');
  'X3' = @('--bench-move-speed', '300', '--bench-no-collision', 'on');
}

function Gzip-File([string]$path) {
  $in = [IO.File]::OpenRead($path)
  $out = [IO.File]::Create($path + '.gz')
  $gz = New-Object IO.Compression.GzipStream($out, [IO.Compression.CompressionMode]::Compress)
  $in.CopyTo($gz); $gz.Close(); $out.Close(); $in.Close()
  Remove-Item $path
}

function Run-One([string]$cond, [int]$rooms, [int]$rep) {
  $label = "$Tag-$cond-r$rooms-rep$rep"
  $log = Join-Path $raw "game-server-$label.log"
  $srvArgs = @('--port', "$Port", '--log-level', 'info', '--stats-interval', '1') + $extra[$cond]
  $srv = Start-Process -FilePath $server -ArgumentList $srvArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
  Start-Sleep -Seconds 2
  $cliArgs = @('--port', "$Port", '--rooms', "$rooms", '--threads', '4', '--warmup', '5', '--duration', '30',
               '--server-pid', "$($srv.Id)", '--label', $label, '--csv', (Join-Path $raw "game-load-$Tag.csv"))
  $cli = Start-Process -FilePath $client -ArgumentList $cliArgs -PassThru -Wait -WindowStyle Hidden -RedirectStandardOutput (Join-Path $raw "game-$label.txt") -RedirectStandardError (Join-Path $raw "game-$label.err")
  $code = $cli.ExitCode
  if (-not $srv.HasExited) { Stop-Process -Id $srv.Id -Force }
  Start-Sleep -Seconds 2
  Gzip-File $log
  foreach ($f in @(($log + '.err'), (Join-Path $raw "game-$label.err"))) { if ((Test-Path $f) -and ((Get-Item $f).Length -eq 0)) { Remove-Item $f } }
  Add-Content -Path (Join-Path $raw "game-load-$Tag-progress.txt") -Value "$label exit=$code" -Encoding UTF8
  Write-Output "$label exit=$code"
  Start-Sleep -Seconds 4
}

$bg = (Get-Process | Sort-Object CPU -Descending | Select-Object -First 8 | ForEach-Object { $_.ProcessName }) -join ', '
$lines = @("Tag: $Tag, rooms: $Rooms, reps: $Reps, conditions: $($Conditions -join ',')",
  "Commit: $(& git rev-parse --short HEAD) (working tree has uncommitted changes: instrumentation + bench options)",
  "Top CPU-time background processes at start: $bg")
Set-Content -Path (Join-Path $raw "game-load-$Tag-environment.txt") -Value $lines -Encoding UTF8

# interleave conditions across repetitions so slow drift hits every condition equally
for ($i = 1; $i -le $Reps; $i++) { foreach ($c in $Conditions) { Run-One $c $Rooms $i } }
Write-Output 'DONE'
