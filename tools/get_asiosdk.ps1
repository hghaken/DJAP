# Puts the Steinberg ASIO SDK into third_party\asiosdk (not in the repository: THIRD_PARTY.md). Two ways:
#   -From <folder>  copy an SDK that is already on this computer (any folder that contains common\asio.h)
#   (no -From)      download it - only after a yes to the question that shows the address - and check the zip
#                   against the expected SHA-256 (the 2.3.4 release this project was built with)
# Usage: powershell -ExecutionPolicy Bypass -File tools\get_asiosdk.ps1 [-From <folder>] [-Url <zip url>]
#        [-Sha256 <hash>] [-Target <folder>] [-Force]
param(
    [string]$From = "",
    [string]$Url = "https://www.steinberg.net/asiosdk",
    [string]$Sha256 = "D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA",
    [string]$Target = (Join-Path $PSScriptRoot "..\third_party\asiosdk"),
    [switch]$Force
)
$ErrorActionPreference = "Stop"
$Target = [System.IO.Path]::GetFullPath($Target)
if ((Test-Path (Join-Path $Target "common\asio.cpp")) -and -not $Force) {
    Write-Output "The ASIO SDK is already in $Target (use -Force to replace it)."
    exit 0
}

# the folder inside $root that holds common\asio.h
function sdkRoot($root) {
    $h = Get-ChildItem -Path $root -Recurse -Filter "asio.h" -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq "common" } | Select-Object -First 1
    if (-not $h) { return $null }
    return $h.Directory.Parent.FullName
}

function install($src, $note) {
    foreach ($need in "common\asio.cpp", "host\asiodrivers.cpp", "host\pc\asiolist.cpp") {
        if (-not (Test-Path (Join-Path $src $need))) { throw "not a usable ASIO SDK: $src has no $need" }
    }
    # the GPL v3 option came with SDK 2.3.4: an older SDK may not be used in DJAP (licence reviews, 2026-10-28)
    $lic = Join-Path $src "LICENSE.txt"
    if (-not (Test-Path $lic) -or (Get-Content -Raw $lic) -notmatch 'General Public License \(GPL\) Version 3') {
        throw "this ASIO SDK has no GPL v3 licence option (it came with version 2.3.4): DJAP cannot use it"
    }
    if (Test-Path $Target) { Remove-Item -Recurse -Force $Target }
    New-Item -ItemType Directory -Force $Target | Out-Null
    foreach ($item in "common", "host", "LICENSE.txt", "README.md", "changes.txt") {
        $p = Join-Path $src $item
        if (Test-Path $p) { Copy-Item -Recurse -Force $p $Target }
    }
    Set-Content -Encoding ASCII -Path (Join-Path $Target "VERSION.txt") -Value $note
    Write-Output "ASIO SDK installed in $Target ($note)"
}

if ($From) {
    $root = sdkRoot $From
    if (-not $root) { throw "no ASIO SDK (common\asio.h) under $From" }
    install $root "copied from $root (GPL v3 option)"
    exit 0
}

Write-Output "This downloads the Steinberg ASIO SDK (a zip of about 1-3 MB) from:"
Write-Output "  $Url"
Write-Output "and installs it in $Target. The SDK is used under its GPL v3 licence option (THIRD_PARTY.md)."
$answer = Read-Host "Download it now? (y/n)"
if ($answer -notmatch '^(y|yes|j|ja)$') { Write-Output "Nothing downloaded."; exit 1 }
$tmp = Join-Path $env:TEMP ("asiosdk_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $tmp | Out-Null
try {
    $zip = Join-Path $tmp "asiosdk.zip"
    Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
    $hash = (Get-FileHash -Algorithm SHA256 $zip).Hash
    Write-Output ("Downloaded {0:N0} bytes, SHA-256 {1}" -f (Get-Item $zip).Length, $hash)
    if ($Sha256 -and $hash -ne $Sha256.ToUpper() -and -not $Force) {
        throw "the zip is not the expected release (SHA-256 $Sha256); check it and run again with -Force to use it"
    }
    Expand-Archive -Path $zip -DestinationPath (Join-Path $tmp "x")
    $root = sdkRoot (Join-Path $tmp "x")
    if (-not $root) { throw "the zip holds no ASIO SDK (common\asio.h)" }
    install $root "downloaded from $Url, SHA-256 $hash (GPL v3 option)"
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
