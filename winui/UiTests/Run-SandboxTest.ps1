# UI test in Windows Sandbox with real (synthetic) pen and touch input.
# Copies the installed app, a test document (+ its PDF) and SandboxTest.ps1 into a shared folder,
# starts a sandbox, runs the test in its user session and collects screenshots, the app log and
# the saved .xopp files in <Work>\out. Then exports page 1-2 of the result with the GTK version
# to check that it still reads the file.
param(
    [Parameter(Mandatory)] [string]$Document,      # .xopp; its PDF must lie next to it
    [string]$Work = (Join-Path $env:TEMP 'xs-sandbox-test'),
    [string]$App = (Join-Path $env:LOCALAPPDATA 'Programs\Xournal Studio WinUI'),
    [string]$GtkExe = (Join-Path $env:LOCALAPPDATA 'Programs\Xournal Studio\bin\xournalpp.exe')
)
$ErrorActionPreference = 'Stop'

if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force "$Work\data", "$Work\out" | Out-Null
robocopy $App "$Work\app" /MIR /NFL /NDL /NJH /NJS /MT:16 | Out-Null
Copy-Item $Document "$Work\data\test.xopp"
Get-ChildItem (Split-Path $Document) -Filter *.pdf | Copy-Item -Destination "$Work\data"
Copy-Item "$PSScriptRoot\Inj.cs", "$PSScriptRoot\SandboxTest.ps1" $Work
Rename-Item "$Work\SandboxTest.ps1" 'test.ps1'

$id = (wsb start --raw | ConvertFrom-Json).Id
try {
    wsb share --id $id -f $Work -s C:\sbx -w | Out-Null
    Start-Process wsb -ArgumentList "connect --id $id"   # the test needs an interactive session
    Start-Sleep 20
    wsb exec --id $id -r ExistingLogin -c 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\sbx\test.ps1' | Out-Null
    for ($i = 0; $i -lt 120 -and -not (Test-Path "$Work\out\done.txt"); $i++) { Start-Sleep 2 }
}
finally {
    wsb stop --id $id | Out-Null
}
Get-Content "$Work\out\transcript.txt" | Select-String -Pattern '^(init|shot|exited|ERROR)'

if (Test-Path $GtkExe) {
    New-Item -ItemType Directory -Force "$Work\gtk" | Out-Null
    Copy-Item "$Work\out\saved2.xopp" "$Work\gtk\test.xopp"
    Copy-Item "$Work\data\*.pdf" "$Work\gtk"
    $env:XDG_CONFIG_HOME = "$Work\gtk\cfg"; $env:XDG_DATA_HOME = "$Work\gtk\data"
    & $GtkExe "--create-img=$Work\gtk\page.png" --export-range=1-2 "$Work\gtk\test.xopp"
}
"Results: $Work\out"
