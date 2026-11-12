<#
.SYNOPSIS
Translates Fragile Allegiance from the user's own copy and builds the native Windows program.

.DESCRIPTION
The script runs the translator on FRAGILE.EXE from the game folder, writes the C++ code to generated\fragile
and compiles it together with the runtime and the native replacements into build\fragile. Both folders stay
local, because their content is derived from the original game. Without -GameFolder the folder recorded by
the GOG installer is used. Only sources that changed since the last build, or whose headers changed, are
compiled again; -Clean compiles everything.

.EXAMPLE
.\build.ps1
.\build.ps1 -GameFolder "D:\Games\Fragile Allegiance" -Configuration Debug
#>
param(
    [string]$GameFolder = "",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
. (Join-Path $root "tools\vsenv.ps1") *> $null

if (-not $GameFolder) {
    $key = "HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\1207659059"
    if (Test-Path $key) { $GameFolder = (Get-ItemProperty $key).path }
}
if (-not $GameFolder -or -not (Test-Path (Join-Path $GameFolder "FRAGILE.EXE"))) {
    throw "The Fragile Allegiance folder was not found. Pass -GameFolder with the folder that contains FRAGILE.EXE."
}

$generated = Join-Path $root "generated\fragile"
$python = (Get-Command python -ErrorAction Stop).Source
& $python (Join-Path $root "tools\recomp\emit32.py") (Join-Path $GameFolder "FRAGILE.EXE") $generated `
    --overrides (Join-Path $root "games\fragile\overrides.txt") --shift 0x400000
if ($LASTEXITCODE -ne 0) { throw "Translating FRAGILE.EXE failed." }

$build = Join-Path $root "build\fragile"
$objects = Join-Path $build "obj-$Configuration"
if ($Clean -and (Test-Path $objects)) { Remove-Item -Recurse -Force $objects }
New-Item -ItemType Directory -Force $objects | Out-Null

$handwritten = @(Get-ChildItem (Join-Path $root "runtime") -Filter *.cpp) + @(Get-ChildItem (Join-Path $root "games\fragile") -Filter *.cpp)
$translated = @(Get-ChildItem $generated -Filter *.cpp)
# Object files are named after their source, so two sources with the same name would overwrite each other.
$clash = @(($handwritten + $translated) | Group-Object BaseName | Where-Object Count -gt 1)
if ($clash.Count -gt 0) { throw "Two source files share the name $($clash[0].Name); rename one of them." }
$headers = @(Get-ChildItem (Join-Path $root "runtime"), (Join-Path $root "games\fragile") -Filter *.h)
$newestHeader = ($headers | Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
$chunksHeader = (Get-Item (Join-Path $generated "chunks.h")).LastWriteTimeUtc
$stale = @()
foreach ($source in $handwritten + $translated) {
    $object = Join-Path $objects ($source.BaseName + ".obj")
    $limit = $newestHeader
    if ($translated -contains $source -and $chunksHeader -gt $limit) { $limit = $chunksHeader }
    if (-not (Test-Path $object)) { $stale += $source.FullName; continue }
    $built = (Get-Item $object).LastWriteTimeUtc
    if ($built -lt $source.LastWriteTimeUtc -or $built -lt $limit) { $stale += $source.FullName }
}

$optimize = if ($Configuration -eq "Release") { @("/O2") } else { @("/Od", "/Zi") }
$flags = @("/nologo", "/c", "/std:c++20", "/EHsc", "/MT", "/W3", "/wd4102", "/MP", "/bigobj", "/utf-8", "/DNOMINMAX",
    "/D_CRT_SECURE_NO_WARNINGS", "/DWIN32_LEAN_AND_MEAN", "/DUNICODE", "/D_UNICODE", "/I$root", "/I$generated",
    "/Fo$($objects -replace '\\', '/')/", "/Fd$($objects -replace '\\', '/')/") + $optimize
if ($stale.Count -gt 0) {
    & cl.exe @flags @stale
    if ($LASTEXITCODE -ne 0) { throw "Compiling failed." }
}

$exe = Join-Path $build "FragileAllegiance.exe"
$linkObjects = ($handwritten + $translated) | ForEach-Object { Join-Path $objects ($_.BaseName + ".obj") }
$debug = if ($Configuration -eq "Debug") { @("/DEBUG") } else { @() }
& link.exe /nologo /OUT:$exe /SUBSYSTEM:WINDOWS /STACK:8388608 @debug @linkObjects advapi32.lib user32.lib shell32.lib ole32.lib d3d12.lib dxgi.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw "Linking failed." }
Copy-Item (Join-Path $generated "image.bin") (Join-Path $build "image.bin") -Force
Write-Host "Built $exe ($($stale.Count) sources compiled)"
