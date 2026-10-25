# Runs every unit test program of a build (and with -Renders also tools\render_check.ps1); prints one line per
# program and a summary. Exit code: number of programs that failed.
# Usage: powershell -ExecutionPolicy Bypass -File tools\run_tests.ps1 [-Build build] [-Renders]
param([string]$Build = (Join-Path $PSScriptRoot "..\build"), [switch]$Renders)
$tests = Get-ChildItem -Path $Build -Filter "*_test.exe" | Sort-Object Name
if (-not $tests) { Write-Output "no *_test.exe in $Build (run build.cmd)"; exit 1 }
$failed = 0
foreach ($t in $tests) {
    $start = Get-Date
    $out = & $t.FullName 2>&1
    $code = $LASTEXITCODE
    $secs = ((Get-Date) - $start).TotalSeconds
    if ($code -eq 0) {
        Write-Output ("  ok    {0,-16} {1,6:N1} s" -f $t.BaseName, $secs)
    } else {
        $failed++
        Write-Output ("  FAIL  {0,-16} {1,6:N1} s  ({2} failure(s))" -f $t.BaseName, $secs, $code)
        $out | Where-Object { $_ -match '^FAIL' } | ForEach-Object { "        $_" }
    }
}
if ($Renders) {
    Write-Output "  render check:"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "render_check.ps1") |
        Where-Object { $_ -match 'FAIL|failed check' } | ForEach-Object { "        $_" }
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
Write-Output ("{0} of {1} failed" -f $failed, ($tests.Count + [int]$Renders.IsPresent))
exit $failed
