param(
    [string]$Trace = "..\..\..\libCacheSim\data\twitter_cluster52.csv",
    [string]$TraceType = "csv",
    [string]$TraceParams = "time-col=1,obj-id-col=2,obj-size-col=3,delimiter=,,has-header=true,obj-id-is-num=1",
    [string[]]$Capacities = @("1MB", "5MB", "10MB")
)

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$sim = Join-Path $root "libCacheSim\_build_prd\bin\cachesim"
if (-not (Test-Path $sim)) {
    throw "Missing $sim. Build libCacheSim first; see results/libcachesim_twitter_prd.md."
}

foreach ($capacity in $Capacities) {
    & $sim $Trace $TraceType prd $capacity -t $TraceParams -e "cold-size-ratio=0.2,ghost-size-ratio=8,lease-turnovers=8"
    & $sim $Trace $TraceType prd $capacity -t $TraceParams -e "cold-size-ratio=0.2,ghost-size-ratio=8,lease-turnovers=1,adaptive-lease=true"
    & $sim $Trace $TraceType prd $capacity -t $TraceParams -e "cold-size-ratio=0.2,ghost-size-ratio=8,lease-turnovers=8,adaptive-lease=true"
    & $sim $Trace $TraceType s3fifo $capacity -t $TraceParams
    & $sim $Trace $TraceType lru $capacity -t $TraceParams
}
