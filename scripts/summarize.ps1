param(
    [string]$Input = '',
    [string]$Output = ''
)

$ErrorActionPreference = 'Stop'
$artifactRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Input)) {
    $Input = Join-Path $artifactRoot 'results\raw.csv'
}
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $artifactRoot 'results\summary.csv'
}

$rows = @(Import-Csv -LiteralPath $Input)
$summary = foreach ($ratioGroup in ($rows | Group-Object capacity_ratio)) {
    $discrete = @($ratioGroup.Group | Where-Object algorithm -eq 'LSBTdiscrete')
    $exact = @($ratioGroup.Group | Where-Object algorithm -eq 'LSBTconsecutive')
    $discreteRequests = ($discrete | Measure-Object requests -Sum).Sum
    $exactRequests = ($exact | Measure-Object requests -Sum).Sum
    $discreteHits = ($discrete | Measure-Object hits -Sum).Sum
    $exactHits = ($exact | Measure-Object hits -Sum).Sum
    $discreteMs = ($discrete | Measure-Object elapsed_ms -Sum).Sum
    $exactMs = ($exact | Measure-Object elapsed_ms -Sum).Sum
    $discreteHitRatio = $discreteHits / $discreteRequests
    $exactHitRatio = $exactHits / $exactRequests
    [pscustomobject]@{
        capacity_ratio = [double]$ratioGroup.Name
        trace_count = $discrete.Count
        requests = [long]$discreteRequests
        discrete_hits = [long]$discreteHits
        exact_hits = [long]$exactHits
        discrete_hit_ratio = $discreteHitRatio
        exact_hit_ratio = $exactHitRatio
        exact_minus_discrete_pp = 100.0 * ($exactHitRatio - $discreteHitRatio)
        discrete_elapsed_ms = [double]$discreteMs
        exact_elapsed_ms = [double]$exactMs
        exact_slowdown = $exactMs / $discreteMs
    }
}

$summary | Sort-Object capacity_ratio | Export-Csv -LiteralPath $Output `
    -NoTypeInformation -Encoding utf8
Write-Host "Summary: $Output"
