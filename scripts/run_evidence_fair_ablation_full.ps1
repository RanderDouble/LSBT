param(
    [switch]$Quick,
    [switch]$AdaptiveOnly,
    [switch]$S3AdaptiveOnly,
    [switch]$ColdAblationOnly
)

$ErrorActionPreference = 'Stop'
$artifactRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent (Split-Path -Parent $artifactRoot)
$ratios = @(0.005, 0.01, 0.02, 0.05, 0.10, 0.20)

function Invoke-Csv([string]$exe, [string[]]$argv) {
    $output = & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$exe failed: $($argv -join ' ')" }
    return $output | ConvertFrom-Csv | Select-Object -First 1
}

function Add-Row($rows, [string]$workload, $trace, [string]$algorithm,
                 [double]$ratio, $result) {
    $rows.Add([pscustomobject]@{
        workload = $workload
        trace = $trace.Name
        algorithm = $algorithm
        capacity_ratio = $ratio
        capacity = [uint64]$result.capacity
        requests = [uint64]$result.requests
        hits = [uint64]$result.hits
        misses = [uint64]$result.misses
        hit_ratio = [double]$result.hit_ratio
        elapsed_ms = [double]$result.elapsed_ms
    })
}

function Run-Raw([bool]$quick) {
    $exe = Join-Path $artifactRoot 'ablation_path_runner.exe'
    $traces = @(Get-ChildItem (Join-Path $workspaceRoot 'raw_ethereum_2024') -Filter '*.csv' |
        Sort-Object Name)
    if ($quick) { $traces = @($traces | Select-Object -First 3) }
    $cases = @(
        @('S3FIFO-g09', @('--algorithm', 's3fifo')),
        @('S3FIFO-g8', @('--algorithm', 's3fifo_ghost8')),
        @('LSBT-discrete', @('--algorithm', 'lsbt_discrete')),
        @('LSBT-consecutive', @('--algorithm', 'lsbt_consecutive')),
        @('PRD', @('--algorithm', 'prd'))
    )
    if ($AdaptiveOnly) {
        $cases = @(
            @('PRD-adaptive-ghost', @('--algorithm', 'lsbt_ghost_hot_turnover8_adaptive_ghost')),
            @('PRD-adaptive-lease', @('--algorithm', 'lsbt_ghost_hot_turnover8_adaptive_lease')),
            @('PRD-adaptive-ghost-lease', @('--algorithm', 'lsbt_ghost_hot_turnover8_adaptive_ghost_lease'))
        )
    }
    if ($S3AdaptiveOnly) {
        $cases = @(
            @('S3-adaptive-ghost', @('--algorithm', 's3fifo_adaptive_ghost')),
            @('S3-adaptive-lease', @('--algorithm', 's3fifo_adaptive_lease')),
            @('S3-adaptive-ghost-lease', @('--algorithm', 's3fifo_adaptive_ghost_lease'))
        )
    }
    if ($ColdAblationOnly) {
        $cases = @(
            @('PRD-adaptive-null-depth', @('--algorithm', 'lsbt_ghost_hot_turnover8_adaptive_ghost_lease_null_depth')),
            @('PRD-adaptive-pure-depth', @('--algorithm', 'lsbt_ghost_hot_turnover8_adaptive_ghost_lease_pure_depth')),
            @('PRD-adaptive-cold-fifo', @('--algorithm', 'lsbt_ghost_hot_turnover8_adaptive_ghost_lease_cold_fifo'))
        )
    }
    $rows = [Collections.Generic.List[object]]::new()
    foreach ($trace in $traces) {
        Write-Host "raw: $($trace.Name)"
        foreach ($ratio in $ratios) {
            foreach ($case in $cases) {
                $argv = @('--trace', $trace.FullName,
                    '--capacity-ratio', [string]$ratio) + $case[1]
                $result = Invoke-Csv $exe $argv
                Add-Row $rows 'raw' $trace $case[0] $ratio $result
            }
        }
    }
    return $rows
}

function Run-Block([bool]$quick) {
    $exe = Join-Path $artifactRoot 'ablation_runner.exe'
    $traces = @(Get-ChildItem (Join-Path $artifactRoot 'workload\lsbt_ethereum_2024') -Filter '*.csv' |
        Sort-Object Name)
    if ($quick) { $traces = @($traces | Select-Object -First 3) }
    $cases = @(
        @('S3FIFO-g09', @('--algorithm', 'S3FIFO')),
        @('S3FIFO-g8', @('--algorithm', 'S3FIFOGhost8')),
        @('LSBT-discrete', @('--algorithm', 'LSBTdiscrete')),
        @('LSBT-consecutive', @('--algorithm', 'LSBTconsecutive')),
        @('PRD', @('--algorithm', 'PRD'))
    )
    if ($AdaptiveOnly) {
        $cases = @(
            @('PRD-adaptive-ghost', @('--algorithm', 'PRDAdaptiveGhost', '--layer-protection', 'false')),
            @('PRD-adaptive-lease', @('--algorithm', 'PRDAdaptiveLease', '--layer-protection', 'false')),
            @('PRD-adaptive-ghost-lease', @('--algorithm', 'PRD', '--layer-protection', 'false'))
        )
    }
    if ($S3AdaptiveOnly) {
        $cases = @(
            @('S3-adaptive-ghost', @('--algorithm', 'S3FIFOAdaptiveGhost')),
            @('S3-adaptive-lease', @('--algorithm', 'S3FIFOAdaptiveLease')),
            @('S3-adaptive-ghost-lease', @('--algorithm', 'S3FIFOAdaptiveGhostLease'))
        )
    }
    if ($ColdAblationOnly) {
        $cases = @(
            @('PRD-adaptive-null-depth', @('--algorithm', 'PRDNullDepth', '--layer-protection', 'false')),
            @('PRD-adaptive-pure-depth', @('--algorithm', 'PRDPureDepth', '--layer-protection', 'false')),
            @('PRD-adaptive-cold-fifo', @('--algorithm', 'PRDColdFifo', '--layer-protection', 'false'))
        )
    }
    $rows = [Collections.Generic.List[object]]::new()
    foreach ($trace in $traces) {
        Write-Host "block: $($trace.Name)"
        foreach ($ratio in $ratios) {
            foreach ($case in $cases) {
                $argv = @('--trace', $trace.FullName,
                    '--capacity-ratio', [string]$ratio) + $case[1]
                $result = Invoke-Csv $exe $argv
                Add-Row $rows 'block' $trace $case[0] $ratio $result
            }
        }
    }
    return $rows
}

$allRows = [Collections.Generic.List[object]]::new()
foreach ($row in (Run-Raw $Quick)) { $allRows.Add($row) }
foreach ($row in (Run-Block $Quick)) { $allRows.Add($row) }
$suffix = if ($Quick) { 'quick' } else { 'full' }
$tag = if ($ColdAblationOnly) { "adaptive_cold_ablation_$suffix" } elseif ($S3AdaptiveOnly) { "s3_adaptive_controls_$suffix" } elseif ($AdaptiveOnly) { "adaptive_ghost_start8_$suffix" } else { "evidence_fair_ablation_$suffix" }
$output = Join-Path $artifactRoot "results\$tag.csv"
$allRows | Export-Csv -LiteralPath $output -NoTypeInformation -Encoding utf8
Write-Host "Wrote $output"
