<# Restores faithful enlargements while retaining the whole previous pack. #>
[CmdletBinding()]
param(
    [string]$Folder=(Join-Path $env:LOCALAPPDATA 'Nax Fragile Allegiance\textures'),
    [string]$Upscayl=(Join-Path $env:ProgramFiles 'Upscayl\resources'),
    [string]$Model='high-fidelity-4x'
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$folderPath=(Resolve-Path -LiteralPath $Folder).Path
$dump=Join-Path $folderPath 'dump'
$pack=Join-Path $folderPath 'pack'
if (-not (Test-Path -LiteralPath $dump)) { throw 'The original texture dump is missing.' }
$token=(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$backup=Join-Path $folderPath ('backups\pack-'+$token)
$stage=Join-Path $root ('build\texture-recovery\'+$token)
function Snapshot([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    @(Get-ChildItem -LiteralPath $Path -File -Recurse | Sort-Object FullName | ForEach-Object {
        [pscustomobject]@{Name=$_.FullName.Substring($Path.TrimEnd('\').Length+1);Length=$_.Length;Hash=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    })
}
$before=@(Snapshot $pack)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backup),$stage | Out-Null
if (Test-Path -LiteralPath $pack) {
    Copy-Item -LiteralPath $pack -Destination $backup -Recurse
    $copied=@(Snapshot $backup)
    if ($before.Count -ne $copied.Count -or ($before.Count -and @(Compare-Object $before $copied -Property Name,Length,Hash).Count)) { throw 'The texture backup did not match the active pack.' }
}
$before | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $stage 'previous-pack.json')
Write-Output "Preserved previous pack: $backup"
# Generate into a separate folder. A failed picture prevents promotion of the batch.
& (Join-Path $PSScriptRoot 'make-texture-pack.ps1') -Folder $folderPath -Upscayl $Upscayl -Model $Model -Destination (Join-Path $stage 'pack') -Rebuild
$made=Join-Path $stage 'pack'
$originals=@(Get-ChildItem -LiteralPath $dump -Filter '*.png' -File)
foreach ($original in $originals) {
    if (-not (Test-Path -LiteralPath (Join-Path $made $original.Name))) { throw 'The staged pack is incomplete.' }
}
$current=@(Snapshot $pack)
if ($before.Count -ne $current.Count -or ($before.Count -and @(Compare-Object $before $current -Property Name,Length,Hash).Count)) { throw 'The active pack changed during recovery; staged files are retained for inspection.' }
New-Item -ItemType Directory -Force -Path $pack | Out-Null
foreach ($original in $originals) {
    foreach ($extension in @('.pal','.png')) {
        $name=$original.BaseName+$extension
        $source=Join-Path $made $name
        if (-not (Test-Path -LiteralPath $source)) { continue }
        $target=Join-Path $pack $name
        Copy-Item -LiteralPath $source -Destination ($target+'.new') -Force
        Move-Item -LiteralPath ($target+'.new') -Destination $target -Force
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) { throw 'A promoted texture differs from its validated staged file.' }
    }
    $marker=Join-Path $pack ('redrawn\'+$original.BaseName)
    if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker }
}
[pscustomobject]@{Pictures=$originals.Count;Model=$Model;Backup=$backup;Stage=$stage;Complete=$true} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'recovery.json')
Write-Output "Restored $($originals.Count) faithful enlarged pictures. Recovery evidence: $stage"
