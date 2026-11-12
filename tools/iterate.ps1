# Builds the native program and runs it once, printing the first stop. Used while replacing functions.
param([string[]]$GameArguments = @())
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $root "build.ps1") 2>&1 | Select-String -Pattern "error|warning C|Built|failed"
& (Join-Path $root "build\fragile\FragileAllegiance.exe") @GameArguments
"exit $LASTEXITCODE"
