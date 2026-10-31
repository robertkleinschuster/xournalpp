param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'Programs\Xournal Studio')
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

# The fork was installed as "Xournal++ Fork" before it was renamed to Xournal Studio
$oldInstall = [System.IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs\Xournal++ Fork'))
$oldStartMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Xournal++ Fork'

$runningFork = Get-Process -Name xournalpp, xournalpp-wrapper -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Path -and ($_.Path.StartsWith($install, [System.StringComparison]::OrdinalIgnoreCase) -or
            $_.Path.StartsWith($oldInstall + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase))
    }
if ($runningFork) {
    throw 'Close the running Xournal Studio and retry the installation so locked files are not left partially updated.'
}

New-Item -ItemType Directory -Path $install -Force | Out-Null
Get-ChildItem -LiteralPath $portable -Force | Copy-Item -Destination $install -Recurse -Force

$startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Xournal Studio'
New-Item -ItemType Directory -Path $startMenu -Force | Out-Null
$shortcutPath = Join-Path $startMenu 'Xournal Studio.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = Join-Path $install 'bin\xournalpp-wrapper.exe'
$shortcut.WorkingDirectory = Join-Path $install 'bin'
$shortcut.IconLocation = "$($shortcut.TargetPath),0"
$shortcut.Description = 'Xournal Studio'
$shortcut.Save()

# Remove the installation under the old name; the settings stay (they live in %LOCALAPPDATA%\xournalpp-fork)
if ($install -ne $oldInstall -and (Test-Path -LiteralPath (Join-Path $oldInstall 'bin\xournalpp-wrapper.exe'))) {
    Remove-Item -LiteralPath $oldInstall -Recurse -Force
    Write-Output "Removed the old installation at $oldInstall"
}
if (Test-Path -LiteralPath $oldStartMenu) {
    Remove-Item -LiteralPath $oldStartMenu -Recurse -Force
}

Write-Output "Installed Xournal Studio to $install"
Write-Output "Start menu shortcut: $shortcutPath"
