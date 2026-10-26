# Puts Steinberg's plug-in SDK (VST 3 SDK v3.8.1, MIT licence; THIRD_PARTY.md) into third_party\vst3sdk, which the
# build needs for the plug-in (without it everything else builds). A shallow git clone of the release tag with its
# submodules, from github.com/steinbergmedia/vst3sdk (about 220 MB, most of it documentation), checked against the
# commit this project was built with.
# Usage: powershell -ExecutionPolicy Bypass -File tools\get_vst3sdk.ps1 [-Yes] [-Target <folder>]
#   without -Yes it only says what it would download
param([switch]$Yes, [string]$Target = (Join-Path $PSScriptRoot "..\third_party\vst3sdk"))
$ErrorActionPreference = "Stop"
$Url = "https://github.com/steinbergmedia/vst3sdk.git"
$Tag = "v3.8.1_build_84"
$Commit = "3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96"
$Target = [System.IO.Path]::GetFullPath($Target)

if (Test-Path (Join-Path $Target "CMakeLists.txt")) {
    Write-Output "The SDK is already in $Target."
    exit 0
}
if (-not $Yes) {
    Write-Output "Would download $Url (tag $Tag, with submodules, about 220 MB) into $Target."
    Write-Output "Run again with -Yes to do it."
    exit 0
}
$ErrorActionPreference = "Continue" # git reports its progress on stderr
git clone --quiet --depth 1 --branch $Tag --recurse-submodules --shallow-submodules $Url $Target 2>&1 | Out-Null
$ErrorActionPreference = "Stop"
if ($LASTEXITCODE -ne 0) { throw "git clone failed ($LASTEXITCODE)" }
$head = (git -C $Target rev-parse HEAD).Trim()
if ($head -ne $Commit) { throw "the tag $Tag is commit $head, not the expected $Commit: check the SDK before using it" }
Write-Output "VST 3 SDK $Tag in $Target (commit $head)."
