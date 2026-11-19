# I/O thread split comparison runner. Plan: docs/stage4.md "send thread split (candidate A) plan".
# Conditions (same game code, only the I/O structure differs):
#   PRE = server before the split (C:\dev\yong_pre), IO0 = new code --io-threads 0,
#   IO1/IO2/IO4 = new code --io-threads 1/2/4.
# The SAME load client is used for all. Runs alternate through the conditions inside each round.
# Before each run the whole-PC CPU is sampled for 5 s; if it is above 10 percent the runner waits (max 2 min)
# and otherwise marks the run CONTAMINATED (it still runs, but the aggregation must leave it out).
# Usage (repository root, close games and other heavy programs first):
#   powershell -ExecutionPolicy Bypass -File results\stage4\run-load-io.ps1 -RoomsList 500 -Tag io-r500
# ASCII only on purpose (Windows PowerShell 5.1 reads BOM-less files as ANSI).
param(
  [string]$RoomsList = '500',
  [int]$Reps = 3,
  [int]$Port = 17950,
  [string]$Tag = 'io',
  [string]$CondList = 'PRE,IO0,IO1,IO2,IO4',
  [double]$IdleLimit = 10.0
)
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path
$raw = Join-Path $root 'results\stage4\raw'
$newExe = Join-Path $root 'build\windows-local\Release\game_server.exe'
$preExe = 'C:\dev\yong_pre\build\windows-local\Release\game_server.exe'
$client = Join-Path $root 'build\windows-local\Release\game_load_client.exe'
foreach ($f in @($newExe, $preExe, $client)) { if (-not (Test-Path $f)) { throw "missing: $f" } }

# condition -> exe and extra args
function Get-Cond([string]$c) {
  switch ($c) {
    'PRE' { return @{ Exe = $preExe; Extra = @() } }
    'IO0' { return @{ Exe = $newExe; Extra = @('--io-threads', '0') } }
    'IO1' { return @{ Exe = $newExe; Extra = @('--io-threads', '1') } }
    'IO2' { return @{ Exe = $newExe; Extra = @('--io-threads', '2') } }
    'IO4' { return @{ Exe = $newExe; Extra = @('--io-threads', '4') } }
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

# Average whole-PC CPU over 5 one-second samples.
function Measure-Idle() {
  Start-Sleep -Milliseconds 1000
  $sum = 0.0
  for ($i = 0; $i -lt 5; $i++) { Start-Sleep -Milliseconds 1000; $sum += $cpuCounter.NextValue() }
  return [math]::Round($sum / 5, 1)
}

function Run-One([string]$cond, [int]$rooms, [int]$rep) {
  $label = "$Tag-$cond-r$rooms-rep$rep"
  # 1) quiet-PC check (before our own processes start)
  $idle = Measure-Idle
  $tries = 0
  while ($idle -gt $IdleLimit -and $tries -lt 12) { Start-Sleep -Seconds 10; $idle = Measure-Idle; $tries++ }
  $contaminated = ($idle -gt $IdleLimit)
  $top = (Get-Process | Sort-Object CPU -Descending | Select-Object -First 5 | ForEach-Object { $_.ProcessName }) -join '/'

  # 2) start server and client
  $spec = Get-Cond $cond
  $log = Join-Path $raw "game-server-$label.log"
  $srvArgs = @('--port', "$Port", '--log-level', 'info', '--stats-interval', '1') + $spec.Extra
  $srv = Start-Process -FilePath $spec.Exe -ArgumentList $srvArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
  Start-Sleep -Seconds 2
  $cliArgs = @('--port', "$Port", '--rooms', "$rooms", '--threads', '4', '--warmup', '5', '--duration', '30',
               '--server-pid', "$($srv.Id)", '--label', $label, '--csv', (Join-Path $raw "game-load-$Tag.csv"))
  $cli = Start-Process -FilePath $client -ArgumentList $cliArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $raw "game-$label.txt") -RedirectStandardError (Join-Path $raw "game-$label.err")

  # 3) whole-PC CPU while the run is going (reference only)
  $samples = New-Object System.Collections.Generic.List[double]
  while (-not $cli.HasExited) { Start-Sleep -Milliseconds 1000; $samples.Add([double]$cpuCounter.NextValue()) }
  $cli.WaitForExit()   # without this ExitCode can be empty for a process started with -PassThru and no -Wait
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

$lines = @("Tag: $Tag, rooms: $RoomsList, reps: $Reps, conditions: $CondList, idle limit: $IdleLimit",
  "PRE server: $preExe (session/server from commit 03722ec, same game code)", "NEW server: $newExe  commit: $(& git rev-parse --short HEAD) + uncommitted changes",
  "Logical processors: $env:NUMBER_OF_PROCESSORS",
  "Top CPU-time background processes at start: $((Get-Process | Sort-Object CPU -Descending | Select-Object -First 8 | ForEach-Object { $_.ProcessName }) -join ', ')")
Set-Content -Path (Join-Path $raw "game-load-$Tag-environment.txt") -Value $lines -Encoding UTF8

$conds = $CondList -split ','
foreach ($rooms in ($RoomsList -split ',' | ForEach-Object { [int]$_ })) {
  for ($i = 1; $i -le $Reps; $i++) { foreach ($c in $conds) { Run-One $c $rooms $i } }
}
Write-Output 'DONE'
