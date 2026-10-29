param(
    [string]$MsysRoot = 'C:\msys64',
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'Programs\Xournal++ Fork')
)

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $repo 'build'
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) { throw "MSYS2 bash not found: $bash" }

$env:MSYSTEM = 'UCRT64'
$env:PATH = "$(Join-Path $MsysRoot 'ucrt64\bin');$(Join-Path $MsysRoot 'usr\bin');$env:PATH"

& cmake -S $repo -B $build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_GTEST=ON -DDOWNLOAD_GTEST=on
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

# Keep the configured Ninja build tree so subsequent runs rebuild changed files only.
& cmake --build $build --target translations --parallel ([Environment]::ProcessorCount)
if ($LASTEXITCODE -ne 0) { throw 'Translation build failed' }
& cmake --build $build --parallel ([Environment]::ProcessorCount)
if ($LASTEXITCODE -ne 0) { throw 'Xournal++ build failed' }

$repoMsys = '/' + $repo.Substring(0, 1).ToLowerInvariant() + $repo.Substring(2).Replace('\', '/')
$buildMsys = "$repoMsys/build"
$packageCommand = "cd '$buildMsys' && ../windows-setup/package.sh"
& $bash --login -c $packageCommand
if ($LASTEXITCODE -ne 0) { throw 'Portable package build failed' }

& (Join-Path $PSScriptRoot 'install-fork.ps1') -BuildDirectory $build -InstallDirectory $InstallDirectory
if ($LASTEXITCODE -ne 0) { throw 'Fork installation failed' }
