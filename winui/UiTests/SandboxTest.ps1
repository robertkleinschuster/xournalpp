$ErrorActionPreference = 'Stop'
$root = 'C:\sbx'; $out = "$root\out"
New-Item -ItemType Directory -Force $out | Out-Null
Start-Transcript "$out\transcript.txt" | Out-Null
try {
    Add-Type -Path "$root\Inj.cs" -ReferencedAssemblies System.Drawing
    Get-Process XournalStudio -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1
    $work = 'C:\work'; New-Item -ItemType Directory -Force $work | Out-Null
    Copy-Item "$root\data\*" $work -Force
    $env:XOURNAL_STUDIO_CONFIG_DIR = "$work\cfg"; $env:XOURNAL_STUDIO_LOG = "$out\app.log"
    $p = Start-Process "$root\app\XournalStudio.exe" -ArgumentList "`"$work\test.xopp`"" -PassThru
    for ($i = 0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 500; $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break } }
    if ($p.MainWindowHandle -eq 0) { throw "no window (exited: $($p.HasExited))" }
    Start-Sleep 6
    $h = $p.MainWindowHandle
    [Inj]::SetForegroundWindow($h) | Out-Null
    "init: " + [Inj]::Init($h)
    $W = [Inj]::W
    function Shot($name) { Start-Sleep -Milliseconds 800; [Inj]::Shot("$out\$name.png"); if ($p.HasExited) { throw "app exited after $name" }; "shot $name" }
    function Line($x0, $y0, $x1, $y1) { [double[]]@($x0, $y0, $x1, $y1) }
    $tb = 63  # toolbar row (client DIPs)

    Shot '01-start'
    $x = { param($f) [double]($f * $W) }
    [Inj]::PenStroke([double[]]@((& $x 0.3),180, (& $x 0.35),230, (& $x 0.4),180, (& $x 0.45),230, (& $x 0.5),180, (& $x 0.55),230), $false, 600)
    Shot '02-pen'
    [Inj]::TouchDrag((& $x 0.75), 450, (& $x 0.75), 200, 25)
    Shot '03-touch-pan'
    [Inj]::TouchDrag((& $x 0.75), 200, (& $x 0.75), 450, 25)
    Shot '04-touch-back'
    [Inj]::Click(333, $tb); Shot '05-undo'
    [Inj]::Click(404, $tb); Shot '06-redo'
    [Inj]::PenStroke((Line (& $x 0.3) 270 (& $x 0.6) 270), $false, 512); Shot '07-stroke-b'
    [Inj]::Click(110, $tb)
    [Inj]::PenStroke((Line (& $x 0.45) 250 (& $x 0.45) 290), $false, 512); Shot '08-erased-with-eraser-tool'
    [Inj]::Click(30, $tb)
    [Inj]::PenStroke((Line (& $x 0.3) 330 (& $x 0.6) 330), $false, 900); Shot '09-stroke-c'
    [Inj]::PenStroke((Line (& $x 0.45) 310 (& $x 0.45) 350), $true, 512); Shot '10-erased-with-pen-eraser'
    [Inj]::Click(69, $tb)
    [Inj]::PenStroke((Line (& $x 0.3) 390 (& $x 0.6) 390), $false, 512); Shot '11-highlighter'
    [Inj]::Click(30, $tb)
    [Inj]::Pinch((& $x 0.5), 330, 150, 400, 25); Shot '12-pinch-zoom-in'
    [Inj]::Pinch((& $x 0.5), 330, 400, 150, 25); Shot '13-pinch-zoom-out'
    [Inj]::Click((& $x 0.5), 30)  # menu bar background: focus
    [Inj]::Ctrl(0x53); Start-Sleep 2; Shot '14-saved'
    Copy-Item "$work\test.xopp" "$out\saved1.xopp"
    [Inj]::Click(179, $tb); Shot '15-page-inserted'
    [Inj]::PostMessage($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $p.WaitForExit(15000) | Out-Null
    "exited: $($p.HasExited)"
    Copy-Item "$work\test.xopp" "$out\saved2.xopp"
    Get-ChildItem $work | Select-Object Name, Length | Format-Table | Out-String
}
catch { "ERROR: $_"; $_.ScriptStackTrace }
finally { Stop-Transcript | Out-Null; Set-Content "$out\done.txt" 'done' }

