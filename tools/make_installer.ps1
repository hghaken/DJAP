# Builds the setup program: release\DJAP-Setup-<version>.exe (installer\DJAP.iss, Inno Setup 6).
# Usage: powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1 [-NoBuild]
#   -NoBuild  package the programs already in build\ (default: build.cmd first)
# Inno Setup is needed on the building PC only (winget install JRSoftware.InnoSetup --scope user; ask before downloading).
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $NoBuild) {
    & cmd /c "`"$root\build.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build.cmd failed ($LASTEXITCODE)" }
}

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found: install it first (winget install JRSoftware.InnoSetup --scope user)' }

$version = (Get-Item "$root\build\DJAP.exe").VersionInfo.ProductVersion
if ($version -match '\+dev') { Write-Warning "version ${version}: the working tree has uncommitted changes" }

& $iscc /Qp "$root\installer\DJAP.iss"
if ($LASTEXITCODE -ne 0) {
    # "Resource update error: EndUpdateResource failed" now and then: the virus scanner holds the new file a moment
    Write-Warning "ISCC failed ($LASTEXITCODE); trying once more in 3 s"
    Start-Sleep -Seconds 3
    & $iscc /Qp "$root\installer\DJAP.iss"
}
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
$setup = Join-Path $root "release\DJAP-Setup-$version.exe"
Write-Host ("{0} ({1:N1} MB)" -f $setup, ((Get-Item $setup).Length / 1MB))
# the symbol files stay here, not in the (public) setup: they hold the build machine's paths; --diag reports of this
# version are read with them
$symbols = Join-Path $root "release\DJAP-$version-symbols"
New-Item -ItemType Directory -Force $symbols | Out-Null
Copy-Item "$root\build\DJAP.pdb", "$root\build\DJAPCmd.pdb", "$root\build\DJAP.exe", "$root\build\DJAPCmd.exe" $symbols
Write-Host "symbols: $symbols"
