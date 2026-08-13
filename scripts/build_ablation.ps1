param([ValidateSet('release','debug')][string]$Configuration = 'release')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$flags = @('-std=c++17','-Wall','-Wextra','-pedantic')
if ($Configuration -eq 'release') { $flags += @('-O3','-DNDEBUG') } else { $flags += @('-O0','-g') }
& g++ @flags (Join-Path $root 'code\ablation_runner.cpp') -o (Join-Path $root 'ablation_runner.exe')
if ($LASTEXITCODE -ne 0) { throw "ablation block runner build failed" }
& g++ @flags (Join-Path $root 'code\ablation_path_runner.cpp') -o (Join-Path $root 'ablation_path_runner.exe')
if ($LASTEXITCODE -ne 0) { throw "ablation raw runner build failed" }
& (Join-Path $root 'ablation_runner.exe') --self-test
if ($LASTEXITCODE -ne 0) { throw "block ablation self-test failed" }
& (Join-Path $root 'ablation_path_runner.exe') --self-test
if ($LASTEXITCODE -ne 0) { throw "raw ablation self-test failed" }
