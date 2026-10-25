# Renders every music copy through the shipped presets with DJAPCmd and collects the render report into a CSV.
# -Settings: a test settings file (e.g. changed shipped presets for a before / after comparison).
param([string]$Music, [string]$Exe, [string]$Csv, [string]$Tmp, [string]$Settings)
$presets = "Gentle", "Light", "Medium", "Loud", "Very Loud", "FM Station", "DAB+", "Web Stream"
New-Item -ItemType Directory -Force $Tmp | Out-Null
$rows = @()
$files = Get-ChildItem $Music -Filter *.mp3 | Sort-Object Name
$n = 0
foreach ($f in $files) {
    $n++
    foreach ($p in $presets) {
        $out = Join-Path $Tmp "r.wav"
        # the preset alone, not the settings' colour (an argument list: `if` returns $null for an empty array)
        $cmd = @('--render', $f.FullName, $out, '--a', $p, '--ca', 'Neutral')
        if ($Settings) { $cmd += @('--ini', $Settings) } # not $Ini: PowerShell names ignore case, $inI is below
        $rep = & $Exe @cmd 2>&1
        $line = $rep | Where-Object { $_ -match 'in\s+(-?[\d.]+|-inf)\s+LUFS\s+->\s+out\s+(-?[\d.]+|-inf)\s+LUFS, LRA ([\d.]+) -> ([\d.]+) LU, true peak (-?[\d.]+) dBTP' } | Select-Object -First 1
        if (-not $line) { # a failed render: no row (else $Matches would still hold the last song's values)
            Write-Output ("render failed: {0} / {1}: {2}" -f $f.BaseName, $p, ($rep -join ' '))
            continue
        }
        $null = $line -match 'in\s+(-?[\d.]+)\s+LUFS\s+->\s+out\s+(-?[\d.]+)\s+LUFS, LRA ([\d.]+) -> ([\d.]+) LU, true peak (-?[\d.]+) dBTP'
        $inI = $Matches[1]; $outI = $Matches[2]; $lraIn = $Matches[3]; $lraOut = $Matches[4]; $tp = $Matches[5]
        $g = $rep | Where-Object { $_ -match 'leveller ([+-][\d.]+) dB on average, compressor up to ([\d.]+) dB, limiter up to ([\d.]+) dB \(([\d.]+) dB on average\)' } | Select-Object -First 1
        $null = $g -match 'leveller ([+-][\d.]+) dB on average, compressor up to ([\d.]+) dB, limiter up to ([\d.]+) dB \(([\d.]+) dB on average\)'
        $rows += [pscustomobject]@{
            file = $f.BaseName; preset = $p; inI = $inI; outI = $outI; lraIn = $lraIn; lraOut = $lraOut; tp4 = $tp
            leveller = $Matches[1]; compMax = $Matches[2]; limMax = $Matches[3]; limMean = $Matches[4]
        }
        Remove-Item $out -ErrorAction SilentlyContinue
    }
    Write-Output ("{0}/{1} {2}" -f $n, $files.Count, $f.BaseName)
    $rows | Export-Csv -NoTypeInformation -Encoding UTF8 $Csv
}
Write-Output "done"
