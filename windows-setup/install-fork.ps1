param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'Programs\Xournal++ Fork')
)

$ErrorActionPreference = 'Stop'
$portable = Join-Path ([System.IO.Path]::GetFullPath($BuildDirectory)) 'dist'
$launcher = Join-Path $portable 'bin\xournalpp-wrapper.exe'
if (-not (Test-Path -LiteralPath $launcher)) {
    throw "Portable build not found at '$portable'. Run build-local.ps1 first."
}

$install = [System.IO.Path]::GetFullPath($InstallDirectory)
$programs = [System.IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs')) + [System.IO.Path]::DirectorySeparatorChar
if (-not $install.StartsWith($programs, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Install directory must be below '$programs'."
}

New-Item -ItemType Directory -Path $install -Force | Out-Null
Get-ChildItem -LiteralPath $portable -Force | Copy-Item -Destination $install -Recurse -Force

$startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Xournal++ Fork'
New-Item -ItemType Directory -Path $startMenu -Force | Out-Null
$shortcutPath = Join-Path $startMenu 'Xournal++ Fork.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = Join-Path $install 'bin\xournalpp-wrapper.exe'
$shortcut.WorkingDirectory = Join-Path $install 'bin'
$shortcut.IconLocation = "$($shortcut.TargetPath),0"
$shortcut.Description = 'Xournal++ Fork'
$shortcut.Save()

Write-Output "Installed Xournal++ Fork to $install"
Write-Output "Start menu shortcut: $shortcutPath"
