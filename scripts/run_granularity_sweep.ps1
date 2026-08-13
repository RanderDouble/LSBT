$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 's3_audit_runner.exe'
if (!(Test-Path $exe)) { throw 'Build s3_audit_runner.exe first.' }
$trace = if ($args.Count -gt 0) { $args[0] } else { Join-Path (Split-Path $root -Parent | Split-Path -Parent) 'raw_ethereum_2024\trace_2024-01-01_0w_5w.csv' }
$out = Join-Path (Join-Path $root 'results') 'granularity_sweep_pilot.csv'
$rows = @()
foreach ($b in 1,2,4,8,16,32) {
  $text = & $exe --format raw --trace $trace --capacity-ratio 0.01 --batch-size $b
  $rows += ($text | Select-Object -Skip 1)
}
$header = 'algorithm,batch_size,capacity_ratio,unique_keys,capacity,requests,hits,misses,hit_ratio'
Set-Content -LiteralPath $out -Value $header -Encoding UTF8
Add-Content -LiteralPath $out -Value $rows -Encoding UTF8
Write-Host "Wrote $out"
