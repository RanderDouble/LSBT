$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'lease_sweep_runner.exe'
if (!(Test-Path $exe)) { throw 'Build lease_sweep_runner.exe first.' }
$trace = if ($args.Count -gt 0) { $args[0] } else { Join-Path (Split-Path $root -Parent | Split-Path -Parent) 'raw_ethereum_2024\trace_2024-01-01_0w_5w.csv' }
$result = Join-Path $root 'results\lease_sweep_pilot.csv'
$control = Join-Path $root 'results\lease_adaptive_pilot.csv'
$header = 'mode,batch_size,lease_factor,capacity_ratio,capacity,requests,hits,misses,hit_ratio,active_hits,late_reuses,proof_expiries'
Set-Content -LiteralPath $result -Value $header -Encoding UTF8
foreach ($lease in 1,2,4,8,16) {
  $line = & $exe --trace $trace --capacity-ratio 0.01 --batch-size 1 --lease-factor $lease | Select-Object -Last 1
  Add-Content -LiteralPath $result -Value $line -Encoding UTF8
}
$line = & $exe --trace $trace --capacity-ratio 0.01 --batch-size 1 --lease-factor 8 --adaptive --control-log $control | Select-Object -Last 1
Add-Content -LiteralPath $result -Value $line -Encoding UTF8
Write-Host "Wrote $result and $control"
