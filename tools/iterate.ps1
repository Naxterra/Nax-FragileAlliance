# Builds the native program, runs it for a limited time and prints the end of the runtime log. Used while
# replacing functions: a stop shows up as the last log line.
param([int]$Seconds = 15, [string]$GameArguments = "--window", [switch]$NoBuild)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) {
    & (Join-Path $root "build.ps1") 2>&1 | Select-String -Pattern "error|warning C|Built|failed"
}
$log = Join-Path $env:LOCALAPPDATA "Nax Fragile Allegiance\runtime.log"
$process = Start-Process -FilePath (Join-Path $root "build\fragile\FragileAllegiance.exe") -ArgumentList $GameArguments -PassThru
if (-not $process.WaitForExit($Seconds * 1000)) {
    "still running after $Seconds s"
    Stop-Process -Id $process.Id -Force
} else {
    "exit $($process.ExitCode)"
}
if (Test-Path $log) { Get-Content $log -Tail 8 }
