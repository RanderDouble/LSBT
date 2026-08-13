param(
    [string]$Trace = 'workload\lsbt_ethereum_2024\lsbt_2024-01-01_0w_5w.csv'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $root 'ablation_runner.exe'
$tracePath = Join-Path $root $Trace
$ratios = @(0.005, 0.01, 0.02, 0.05, 0.10, 0.20)
$rows = foreach ($ratio in $ratios) {
    $output = & $runner --trace $tracePath --capacity-ratio $ratio --algorithm S3FIFOMainCycleAudit
    if ($LASTEXITCODE -ne 0) { throw "runner failed at capacity ratio $ratio" }
    $output | ConvertFrom-Csv | Select-Object -First 1
}

$destination = Join-Path $root 'results\s3_main_cycle_audit.csv'
$rows | Export-Csv -LiteralPath $destination -NoTypeInformation -Encoding utf8
Write-Host "Wrote $destination"
