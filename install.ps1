[CmdletBinding()]
param([string]$GameFolder)
$ErrorActionPreference='Stop'
$build=Join-Path $PSScriptRoot 'build\fragile'
foreach($name in @('FragileAllegiance.exe','image.bin')) {
    if(-not(Test-Path -LiteralPath (Join-Path $build $name))) { throw 'Build the native game before installing it.' }
}
if(-not $GameFolder) {
    $key='HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\1207659059'
    if(Test-Path -LiteralPath $key) { $GameFolder=(Get-ItemProperty -LiteralPath $key).path }
}
if(-not $GameFolder -or -not(Test-Path -LiteralPath (Join-Path $GameFolder 'FRAGILE.EXE'))) { throw 'Pass -GameFolder pointing to the owned original game.' }
$game=(Resolve-Path -LiteralPath $GameFolder).Path
$installRoot=Join-Path $env:LOCALAPPDATA 'Programs\Nax Fragile Allegiance\enhanced'
$destination=$installRoot
$installed=Join-Path $destination 'FragileAllegiance.exe'
$running=@(Get-CimInstance Win32_Process -Filter "Name='FragileAllegiance.exe'")
if($running | Where-Object ExecutablePath -eq $installed) {
    # Keep an open game and its files intact. The shortcut selects the new build
    # on the next launch; save files remain in the same per-user profile.
    $version=(Get-FileHash -LiteralPath (Join-Path $build 'FragileAllegiance.exe') -Algorithm SHA256).Hash.Substring(0,12).ToLowerInvariant()
    $destination=Join-Path $installRoot ('versions\'+$version)
    $installed=Join-Path $destination 'FragileAllegiance.exe'
}
if($running | Where-Object ExecutablePath -eq $installed) { throw 'The same version is already open. Close it before reinstalling that version.' }
New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach($name in @('FragileAllegiance.exe','image.bin')) {
    $target=Join-Path $destination $name
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination ($target+'.new') -Force
    Move-Item -LiteralPath ($target+'.new') -Destination $target -Force
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE'),(Join-Path $PSScriptRoot 'README.md') -Destination $destination -Force
$shortcutPath=Join-Path ([Environment]::GetFolderPath('Programs')) 'Fragile Allegiance Enhanced.lnk'
$shell=New-Object -ComObject WScript.Shell
$shortcut=$shell.CreateShortcut($shortcutPath)
if((Test-Path -LiteralPath $shortcutPath) -and $shortcut.TargetPath -ne $installed) {
    $previous=[IO.Path]::GetFullPath($shortcut.TargetPath)
    $root=[IO.Path]::GetFullPath($installRoot).TrimEnd('\')+'\'
    if(-not $previous.StartsWith($root,[StringComparison]::OrdinalIgnoreCase) -or
       [IO.Path]::GetFileName($previous) -ne 'FragileAllegiance.exe') { throw 'A different application already uses that Start menu shortcut.' }
}
$shortcut.TargetPath=$installed
$shortcut.Arguments='--data "'+$game+'"'
$shortcut.WorkingDirectory=$destination
$shortcut.Description='Fragile Allegiance with native DX12 graphics and faithful enlarged artwork'
$shortcut.Save()
Write-Output "Installed $installed"
Write-Output "Start menu shortcut: $shortcutPath"
