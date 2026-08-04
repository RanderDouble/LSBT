param(
    [string]$WorkloadDir = '',
    [double[]]$Ratios = @(0.01, 0.05, 0.10, 0.20),
    [string]$Output = ''
)

$ErrorActionPreference = 'Stop'
$artifactRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($WorkloadDir)) {
    $WorkloadDir = Join-Path $artifactRoot 'workload\lsbt_ethereum_2024'
}
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $artifactRoot 'results\raw.csv'
}

$runner = Join-Path $artifactRoot 'runner.exe'
if (-not (Test-Path -LiteralPath $runner)) {
    & (Join-Path $PSScriptRoot 'build.ps1')
}

$traces = @(Get-ChildItem -LiteralPath $WorkloadDir -Filter '*.csv' -File |
    Where-Object { $_.Name -ne 'manifest.csv' } | Sort-Object Name)
if ($traces.Count -eq 0) {
    throw "no trace CSV files found in $WorkloadDir"
}

$outputDir = Split-Path -Parent $Output
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$wroteHeader = $false
foreach ($trace in $traces) {
    foreach ($ratio in $Ratios) {
        Write-Host ("{0}: capacity ratio {1:P0}" -f $trace.Name, $ratio)
        $lines = @(& $runner --algorithm both --trace $trace.FullName `
            --capacity-ratio $ratio)
        if ($LASTEXITCODE -ne 0) {
            throw "runner failed for $($trace.FullName), ratio $ratio"
        }
        if (-not $wroteHeader) {
            Set-Content -LiteralPath $Output -Value $lines -Encoding utf8
            $wroteHeader = $true
        } else {
            Add-Content -LiteralPath $Output -Value ($lines | Select-Object -Skip 1) `
                -Encoding utf8
        }
    }
}

Write-Host "Raw results: $Output"

