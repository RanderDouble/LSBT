param([switch]$Quick)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$workspace = Split-Path -Parent (Split-Path -Parent $root)
$exe = Join-Path $root 's3_audit_runner.exe'
$ratios = @(0.005,0.01,0.02,0.05,0.10,0.20)
$ghosts = @(0.9,8.0)
$batches = @(1)
if ($env:GRANULARITY_SWEEP -eq '1') { $batches = @(1,2,4,8,16,32) }
$sets = @(@('raw',(Join-Path $workspace 'raw_ethereum_2024')), @('block',(Join-Path $workspace 'lsbt_ethereum_2024')))
$rows = [Collections.Generic.List[object]]::new()
foreach($set in $sets){$traces=@(Get-ChildItem $set[1] -Filter '*.csv' | Sort-Object Name);if($Quick){$traces=@($traces|Select-Object -First 2)};foreach($t in $traces){Write-Host "$($set[0]): $($t.Name)";foreach($b in $batches){foreach($r in $ratios){foreach($g in $ghosts){$out=& $exe --format $set[0] --trace $t.FullName --batch-size $b --capacity-ratio $r --ghost-ratio $g;if($LASTEXITCODE -ne 0){throw "audit failed: $($t.Name)"};$row=$out|ConvertFrom-Csv|Select-Object -First 1;$row|Add-Member NoteProperty trace $t.Name;$rows.Add($row)}}}}}
$tag=if($Quick){'quick'}else{'full'};$rows|Export-Csv (Join-Path $root "results\s3_turnover_audit_$tag.csv") -NoTypeInformation -Encoding utf8
