# Builds Xournal Studio (WinUI) and installs it side by side with the GTK version:
#   %LOCALAPPDATA%\Programs\Xournal Studio WinUI  +  Start menu entry "Xournal Studio (WinUI)".
# No file associations are registered; .xopp stays with the GTK version.
param([switch]$SkipInstall, [switch]$SkipTests)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$publish = Join-Path $root 'XournalStudio\bin\publish'
$target = Join-Path $env:LOCALAPPDATA 'Programs\Xournal Studio WinUI'

if (-not $SkipTests) {
    dotnet test (Join-Path $root 'XournalStudio.Tests') -nologo -v q
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}

if (Test-Path $publish) { Remove-Item -Recurse -Force $publish }
dotnet publish (Join-Path $root 'XournalStudio\XournalStudio.csproj') -c Release -o $publish -nologo -v q
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

if ($SkipInstall) { "Published to $publish"; return }

$running = Get-Process XournalStudio -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$target\*" }
if ($running) { throw 'Xournal Studio (WinUI) is running; close it first or use -SkipInstall.' }

if (Test-Path $target) { Remove-Item -Recurse -Force $target }
Copy-Item -Recurse $publish $target

$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Xournal Studio (WinUI).lnk'
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($shortcut)
$link.TargetPath = Join-Path $target 'XournalStudio.exe'
$link.WorkingDirectory = $target
$link.IconLocation = (Join-Path $target 'XournalStudio.exe') + ',0'
$link.Save()
"Installed to $target"
