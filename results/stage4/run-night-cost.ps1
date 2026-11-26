# Night (day rollover) cost runner. Plan and pass criteria: docs/content-day.md completion criterion 12
# (written before measuring; do not change them after seeing results).
# Conditions (same server binary, only the day length differs), all with --io-threads 0, 400 rooms:
#   A = default day (12,000 ticks = 10 min): no night happens inside the 35 s run.
#   B = --bench-day-ticks 200: every room is forced to a night every 10 s (about 3 per room while measuring).
#       The generator creates the rooms almost at once, so B puts many rooms' nights on nearby ticks (a bad case).
# Runs alternate A, B inside each round, a fresh server every run.
# Before each run the whole-PC CPU is sampled for 5 s; if it is above the limit the runner waits (max 2 min)
# and otherwise marks the run CONTAMINATED (it still runs, but the write-up must leave it out or say so).
# Record per run (from the day group at the end of the server log stats line, and the night line of the client output):
#   night_us total, max_night_micros, average per-room night time, DayStarted received.
# Usage (repository root, Release build, close games and other heavy programs first):
#   powershell -ExecutionPolicy Bypass -File results\stage4\run-night-cost.ps1
# ASCII only on purpose (Windows PowerShell 5.1 reads BOM-less files as ANSI).
param(
  [int]$Rooms = 400,
  [int]$Reps = 3,
  [int]$Port = 17960,
  [string]$Tag = 'night',
  [string]$CondList = 'A,B',
  [double]$IdleLimit = 10.0
)
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$raw = Join-Path $root 'results\stage4\raw'
$server = Join-Path $root 'build\windows-local\Release\game_server.exe'
$client = Join-Path $root 'build\windows-local\Release\game_load_client.exe'
foreach ($f in @($server, $client)) { if (-not (Test-Path $f)) { throw "missing: $f" } }
if (-not (Test-Path $raw)) { New-Item -ItemType Directory -Path $raw | Out-Null }

function Get-Extra([string]$c) {
  switch ($c) {
    'A' { return @('--io-threads', '0') }
    'B' { return @('--io-threads', '0', '--bench-day-ticks', '200') }
    default { throw "unknown condition: $c" }
  }
}

function Gzip-File([string]$path) {
  $in = [IO.File]::OpenRead($path)
  $out = [IO.File]::Create($path + '.gz')
  $gz = New-Object IO.Compression.GzipStream($out, [IO.Compression.CompressionMode]::Compress)
  $in.CopyTo($gz); $gz.Close(); $out.Close(); $in.Close()
  Remove-Item $path
}

$cpuCounter = New-Object System.Diagnostics.PerformanceCounter('Processor', '% Processor Time', '_Total')
[void]$cpuCounter.NextValue()

function Measure-Idle() {
  Start-Sleep -Milliseconds 1000
  $sum = 0.0
  for ($i = 0; $i -lt 5; $i++) { Start-Sleep -Milliseconds 1000; $sum += $cpuCounter.NextValue() }
  return [math]::Round($sum / 5, 1)
}

function Run-One([string]$cond, [int]$rep) {
  $label = "$Tag-$cond-r$Rooms-rep$rep"
  $idle = Measure-Idle
  $tries = 0
  while ($idle -gt $IdleLimit -and $tries -lt 12) { Start-Sleep -Seconds 10; $idle = Measure-Idle; $tries++ }
  $contaminated = ($idle -gt $IdleLimit)
  $top = (Get-Process | Sort-Object CPU -Descending | Select-Object -First 5 | ForEach-Object { $_.ProcessName }) -join '/'

  $log = Join-Path $raw "game-server-$label.log"
  $srvArgs = @('--port', "$Port", '--log-level', 'info', '--stats-interval', '1') + (Get-Extra $cond)
  $srv = Start-Process -FilePath $server -ArgumentList $srvArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
  Start-Sleep -Seconds 2
  $cliArgs = @('--port', "$Port", '--rooms', "$Rooms", '--threads', '4', '--warmup', '5', '--duration', '30',
               '--server-pid', "$($srv.Id)", '--label', $label, '--csv', (Join-Path $raw "game-load-$Tag.csv"))
  $cli = Start-Process -FilePath $client -ArgumentList $cliArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $raw "game-$label.txt") -RedirectStandardError (Join-Path $raw "game-$label.err")

  $samples = New-Object System.Collections.Generic.List[double]
  while (-not $cli.HasExited) { Start-Sleep -Milliseconds 1000; $samples.Add([double]$cpuCounter.NextValue()) }
  $cli.WaitForExit()
  $code = $cli.ExitCode
  if (-not $srv.HasExited) { Stop-Process -Id $srv.Id -Force }
  Start-Sleep -Seconds 2
  Gzip-File $log
  foreach ($f in @(($log + '.err'), (Join-Path $raw "game-$label.err"))) { if ((Test-Path $f) -and ((Get-Item $f).Length -eq 0)) { Remove-Item $f } }
  $avg = if ($samples.Count -gt 0) { [math]::Round(($samples | Measure-Object -Average).Average, 1) } else { 0 }
  $max = if ($samples.Count -gt 0) { [math]::Round(($samples | Measure-Object -Maximum).Maximum, 1) } else { 0 }
  $flag = if ($contaminated) { 'CONTAMINATED' } else { 'clean' }
  $line = "$label exit=$code pre_idle_cpu=$idle $flag pc_cpu_avg=$avg pc_cpu_max=$max top=$top"
  Add-Content -Path (Join-Path $raw "game-load-$Tag-progress.txt") -Value $line -Encoding UTF8
  Write-Output $line
  Start-Sleep -Seconds 4
}

$lines = @("Tag: $Tag, rooms: $Rooms, reps: $Reps, conditions: $CondList (A default day, B --bench-day-ticks 200), io-threads 0, idle limit: $IdleLimit",
  "Server: $server  commit: $(& git rev-parse --short HEAD)",
  "Logical processors: $env:NUMBER_OF_PROCESSORS",
  "Top CPU-time background processes at start: $((Get-Process | Sort-Object CPU -Descending | Select-Object -First 8 | ForEach-Object { $_.ProcessName }) -join ', ')")
Set-Content -Path (Join-Path $raw "game-load-$Tag-environment.txt") -Value $lines -Encoding UTF8

$conds = $CondList -split ','
for ($i = 1; $i -le $Reps; $i++) { foreach ($c in $conds) { Run-One $c $i } }
Write-Output 'DONE'
