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
    # Optional staging folder. Without it, the active pack is updated.
    [string]$Destination = "",
    # Enlarges every dumped picture again, for example after choosing another model.
    [switch]$Rebuild
)
$ErrorActionPreference = "Stop"

$tool = Join-Path $Upscayl "bin\upscayl-bin.exe"
$models = Join-Path $Upscayl "models"
if (-not (Test-Path $tool)) { throw "Upscayl was not found at $Upscayl. Install it with: winget install Upscayl.Upscayl" }
if (-not (Test-Path (Join-Path $models "$Model.param"))) { throw "The model $Model is not among the Upscayl models in $models." }
$dump = Join-Path $Folder "dump"
$pack = if ($Destination) { [IO.Path]::GetFullPath($Destination) } else { Join-Path $Folder "pack" }
if (-not (Test-Path $dump)) { throw "There are no dumped pictures in $dump. Start the game with --dump-textures first." }
New-Item -ItemType Directory -Force $pack | Out-Null

$todo = @(Get-ChildItem -LiteralPath $dump -Filter *.png | Where-Object {
    $Rebuild -or -not (Test-Path -LiteralPath (Join-Path $pack $_.Name)) -or
    (Test-Path -LiteralPath (Join-Path $pack ('redrawn\'+$_.BaseName)))
})
if ($todo.Count -eq 0) { Write-Host "The pack already has every dumped picture."; return }

# Each picture is enlarged by its own run of the upscaler. When the GPU fails on one picture, the rest of a
# batch run comes out as noise although the upscaler reports success, so every run is checked for errors and
# a failed picture is left out of the pack instead.
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$work = [IO.Path]::GetFullPath((Join-Path $temporaryRoot ("nax-texture-pack-" + [guid]::NewGuid().ToString("N"))))
if (-not $work.StartsWith($temporaryRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'The temporary folder left its intended namespace.' }
New-Item -ItemType Directory -Force $work | Out-Null
Add-Type -AssemblyName System.Drawing
try {
    $done = 0
    $failed = @()
    for ($i = 0; $i -lt $todo.Count; $i++) {
        $picture = $todo[$i]
        Write-Progress -Activity "Enlarging pictures with $Model" -Status $picture.Name -PercentComplete (100 * $i / $todo.Count)
        $made = Join-Path $work $picture.Name
        $messages = & $tool -i $picture.FullName -o $made -m $models -n $Model -s 4 -t 256 -f png 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0 -or $messages -match "failed" -or -not (Test-Path $made)) {
            $failed += $picture.Name
            Remove-Item $made -ErrorAction SilentlyContinue
            continue
        }
        $valid = $false
        $originalImage = $null
        $enlargedImage = $null
        try {
            $originalImage = [Drawing.Image]::FromFile($picture.FullName)
            $enlargedImage = [Drawing.Image]::FromFile($made)
            $valid = $enlargedImage.Width -eq 4*$originalImage.Width -and $enlargedImage.Height -eq 4*$originalImage.Height
        } finally {
            if ($originalImage) { $originalImage.Dispose() }
            if ($enlargedImage) { $enlargedImage.Dispose() }
        }
        if (-not $valid) { $failed += $picture.Name; continue }
        $palette = [System.IO.Path]::ChangeExtension($picture.FullName, ".pal")
        if (Test-Path $palette) { Copy-Item $palette $pack -Force }
        Move-Item $made (Join-Path $pack $picture.Name) -Force
        $marker=Join-Path $pack ('redrawn\'+$picture.BaseName)
        if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker }
        $done++
    }
    Write-Progress -Activity "Enlarging pictures with $Model" -Completed
    Write-Host "Added $done pictures to $pack."
    if ($failed.Count -gt 0) { throw "The upscaler failed on $($failed.Count) pictures: $($failed -join ', ')" }
} finally {
    if (Test-Path -LiteralPath $work) {
        $resolvedWork=(Resolve-Path -LiteralPath $work).Path
        if (-not $resolvedWork.StartsWith($temporaryRoot,[StringComparison]::OrdinalIgnoreCase) -or
            -not ([IO.Path]::GetFileName($resolvedWork)).StartsWith('nax-texture-pack-')) { throw 'Refusing to remove an unexpected temporary directory.' }
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    }
}
