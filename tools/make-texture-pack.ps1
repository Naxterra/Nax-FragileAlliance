<#
.SYNOPSIS
Makes the enhanced texture pack from the game pictures that the program dumped.

.DESCRIPTION
Start the game once with --dump-textures and visit the screens whose pictures should be enhanced; each new
picture is saved under textures\dump in the user folder. This script then enlarges every dumped picture that
the pack does not have yet four times with an AI upscaler (Real-ESRGAN models, as Upscayl installs them) and
puts the result into textures\pack. A running game notices a new pack folder within a few seconds and looks
for new or changed pack pictures every few seconds, so it does not have to be restarted. Pictures can also be edited
or painted by hand: any PNG in the pack with the same name replaces the picture, at any size, as long as it
keeps the transparent margin of the dumped picture in proportion. The pictures stay on this
computer, because they are made from the original game.

Upscayl is installed with: winget install Upscayl.Upscayl

.EXAMPLE
.\tools\make-texture-pack.ps1
.\tools\make-texture-pack.ps1 -Model digital-art-4x -Rebuild
#>
param(
    # The model: high-fidelity-4x keeps the grain of the original art, digital-art-4x paints smoother surfaces,
    # ultrasharp-4x and upscayl-standard-4x lie in between.
    [string]$Model = "high-fidelity-4x",
    [string]$Folder = (Join-Path $env:LOCALAPPDATA "Nax Fragile Allegiance\textures"),
    [string]$Upscayl = (Join-Path $env:ProgramFiles "Upscayl\resources"),
    # Enlarges every dumped picture again, for example after choosing another model.
    [switch]$Rebuild
)
$ErrorActionPreference = "Stop"

$tool = Join-Path $Upscayl "bin\upscayl-bin.exe"
$models = Join-Path $Upscayl "models"
if (-not (Test-Path $tool)) { throw "Upscayl was not found at $Upscayl. Install it with: winget install Upscayl.Upscayl" }
if (-not (Test-Path (Join-Path $models "$Model.param"))) { throw "The model $Model is not among the Upscayl models in $models." }
$dump = Join-Path $Folder "dump"
$pack = Join-Path $Folder "pack"
if (-not (Test-Path $dump)) { throw "There are no dumped pictures in $dump. Start the game with --dump-textures first." }
New-Item -ItemType Directory -Force $pack | Out-Null

$todo = @(Get-ChildItem $dump -Filter *.png | Where-Object { $Rebuild -or -not (Test-Path (Join-Path $pack $_.Name)) })
if ($todo.Count -eq 0) { Write-Host "The pack already has every dumped picture."; return }

# Each picture is enlarged by its own run of the upscaler. When the GPU fails on one picture, the rest of a
# batch run comes out as noise although the upscaler reports success, so every run is checked for errors and
# a failed picture is left out of the pack instead.
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("nax-texture-pack-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    $done = 0
    $failed = @()
    for ($i = 0; $i -lt $todo.Count; $i++) {
        $picture = $todo[$i]
        Write-Progress -Activity "Enlarging pictures with $Model" -Status $picture.Name -PercentComplete (100 * $i / $todo.Count)
        $made = Join-Path $work $picture.Name
        $messages = & $tool -i $picture.FullName -o $made -m $models -n $Model -s 4 -f png 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0 -or $messages -match "failed" -or -not (Test-Path $made)) {
            $failed += $picture.Name
            Remove-Item $made -ErrorAction SilentlyContinue
            continue
        }
        $palette = [System.IO.Path]::ChangeExtension($picture.FullName, ".pal")
        if (Test-Path $palette) { Copy-Item $palette $pack -Force }
        Move-Item $made (Join-Path $pack $picture.Name) -Force
        $done++
    }
    Write-Progress -Activity "Enlarging pictures with $Model" -Completed
    Write-Host "Added $done pictures to $pack."
    if ($failed.Count -gt 0) { Write-Warning "The upscaler failed on $($failed.Count) pictures, which keep their original look: $($failed -join ', ')" }
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
