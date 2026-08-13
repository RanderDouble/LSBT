param(
    [ValidateSet('raw', 'lsbt')]
    [string]$Format = 'raw',
    [Parameter(Mandatory = $true)]
    [string]$Trace,
    [string[]]$CapacityRatios = @('0.005', '0.01', '0.02')
)

$workspace = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$converter = Join-Path $workspace 'lrd_cache\LSBTreplacement\tools\hierarchy_trace_convert.exe'
$simulator = Join-Path $workspace 'libCacheSim\_build_prd\bin\cachesim'
$converted = Join-Path $workspace ("external_traces\hierarchy\{0}_{1}.csv" -f $Format, [IO.Path]::GetFileNameWithoutExtension($Trace))

if (-not (Test-Path $converter)) { throw "Build $converter first." }
if (-not (Test-Path $simulator)) { throw "Build libCacheSim/_build_prd first." }
New-Item -ItemType Directory -Force (Split-Path $converted) | Out-Null
& $converter $Format $Trace $converted

$unique = (Import-Csv $converted | Select-Object -ExpandProperty obj_id -Unique).Count
foreach ($ratio in $CapacityRatios) {
    $capacity = [Math]::Max(64, [Math]::Floor($unique * [double]$ratio) * 64)
    & $simulator $converted csv prd-epoch $capacity -t 'time-col=1,obj-id-col=2,obj-size-col=3,feature-cols=4,delimiter=,,has-header=true,obj-id-is-num=1'
    & $simulator $converted csv s3fifo $capacity -t 'time-col=1,obj-id-col=2,obj-size-col=3,delimiter=,,has-header=true,obj-id-is-num=1'
    & $simulator $converted csv lru $capacity -t 'time-col=1,obj-id-col=2,obj-size-col=3,delimiter=,,has-header=true,obj-id-is-num=1'
}
