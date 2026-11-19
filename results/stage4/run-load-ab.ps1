# Same-session old-vs-new server comparison runner. Plan: docs/stage4.md "same-session old/new server comparison".
# OLD = server built from the measured commit cb9d3c8 in C:\dev\yong_old (speed 300, no collision). NEW = current Release server.
# The SAME (current) load client is used for both. Runs alternate OLD, NEW, OLD, NEW ... within each room count.
# Usage (repository root, nothing else heavy running):
#   powershell -ExecutionPolicy Bypass -File results\stage4\run-load-ab.ps1 -RoomsList 500,400
# ASCII only on purpose (Windows PowerShell 5.1 reads BOM-less files as ANSI).
param(
  [string]$RoomsList = '500,400',
  [int]$Reps = 3,
  [int]$Port = 17900,
  [string]$Tag = 'ab'
)
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$raw = Join-Path $root 'results\stage4\raw'
$servers = @{ 'OLD' = 'C:\dev\yong_old\build\windows-local\Release\game_server.exe'; 'NEW' = (Join-Path $root 'build\windows-local\Release\game_server.exe') }
$client = Join-Path $root 'build\windows-local\Release\game_load_client.exe'
foreach ($k in $servers.Keys) { if (-not (Test-Path $servers[$k])) { throw "missing server: $k" } }

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
  $srvArgs = @('--port', "$Port", '--log-level', 'info', '--stats-interval', '1')
  $srv = Start-Process -FilePath $servers[$cond] -ArgumentList $srvArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
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
$lines = @("Tag: $Tag, rooms: $RoomsList, reps: $Reps",
  "OLD server: $($servers['OLD']) (built from commit cb9d3c8)", "NEW server: $($servers['NEW'])  commit: $(& git rev-parse --short HEAD) + uncommitted instrumentation/bench options",
  "Top CPU-time background processes at start: $bg")
Set-Content -Path (Join-Path $raw "game-load-$Tag-environment.txt") -Value $lines -Encoding UTF8

foreach ($rooms in ($RoomsList -split ',' | ForEach-Object { [int]$_ })) {
  for ($i = 1; $i -le $Reps; $i++) { foreach ($c in @('OLD', 'NEW')) { Run-One $c $rooms $i } }
}
Write-Output 'DONE'
