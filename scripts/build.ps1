param(
    [ValidateSet('release', 'debug')]
    [string]$Configuration = 'release'
)

$ErrorActionPreference = 'Stop'
$artifactRoot = Split-Path -Parent $PSScriptRoot
$flags = @('-std=c++17', '-Wall', '-Wextra', '-pedantic')
if ($Configuration -eq 'release') {
    $flags += @('-O3', '-DNDEBUG')
} else {
    $flags += @('-O0', '-g')
}

& g++ @flags (Join-Path $artifactRoot 'code\runner.cpp') `
    -o (Join-Path $artifactRoot 'runner.exe')
if ($LASTEXITCODE -ne 0) {
    throw "g++ failed with exit code $LASTEXITCODE"
}

Write-Host "Built PRD runner.exe"
