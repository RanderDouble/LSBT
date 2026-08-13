$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -pedantic (Join-Path $root 'code\s3_audit_runner.cpp') -o (Join-Path $root 's3_audit_runner.exe')
if ($LASTEXITCODE -ne 0) { throw 'S3 audit runner build failed' }
