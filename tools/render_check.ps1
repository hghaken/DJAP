# Offline render check (docs/design.md section 9, "Offline"): renders synthetic songs through every shipped preset
# with DJAPCmd and checks the results with an independent meter (audiotool.py from the audio-processing
# skill): integrated loudness against the preset's target (or as far as the leveller's range reaches), true peak
# against the ceiling, no samples at full scale; the program's own meter against audiotool; bypass and a neutral
# preset bit-exact. Works in a folder of its own under %TEMP%; never touches music.
# Usage: powershell -ExecutionPolicy Bypass -File tools\render_check.ps1 [-Work <dir>] [-Keep]
# Exit code: number of failed checks.
param(
    [string]$Work = (Join-Path $env:TEMP "DJAP_render_check"),
    [string]$Exe = (Join-Path $PSScriptRoot "..\build\DJAPCmd.exe"),
    [string]$AudioTool = (Join-Path $env:USERPROFILE ".claude\skills\audio-processing\scripts\audiotool.py"),
    [switch]$Keep
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path $Exe)) { throw "not built: $Exe (run build.cmd)" }
if (-not (Test-Path $AudioTool)) { throw "audiotool.py not found: $AudioTool" }
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory $Work | Out-Null
$script:failures = 0
function check($ok, $what) {
    if ($ok) { "  ok    $what" } else { "  FAIL  $what"; $script:failures++ }
}
function tool { python -I $AudioTool @args }
function toolMeasure($file) { (tool measure --json $file) -join "`n" | ConvertFrom-Json }
# the program's own meter: "  stereo   I  LRA  maxM  maxS  peak  truepeak"
function ownMeasure($file) {
    $line = & $Exe --measure $file | Where-Object { $_ -match '^\s+stereo\s' } | Select-Object -First 1
    $v = ($line -split '\s+') | Where-Object { $_ -ne '' }
    [pscustomobject]@{ I = [double]$v[1]; TP = [double]$v[6] }
}

# ---- material: pink-noise songs (1 s silence, 4 s fade-in, 30 s full, 6 s fade-out, 1 s silence) ----
"Material in $Work"
$levels = @(-24, -14, -6) # dBFS RMS per channel
foreach ($l in $levels) { tool gen (Join-Path $Work "in$l.wav") testsong --rate 48000 --dbfs $l --seed 7 | Out-Null }
tool gen (Join-Path $Work "in441.wav") testsong --rate 44100 --dbfs -14 --seed 8 | Out-Null
$inputs = @{}
foreach ($l in $levels) { $inputs[$l] = toolMeasure (Join-Path $Work "in$l.wav") }
foreach ($l in $levels) { "  in$l.wav: I {0:N2} LUFS, true peak {1:N2} dBTP" -f $inputs[$l].integrated_lufs, $inputs[$l].true_peak_dbtp }

# presets: target, leveller range, ceiling, output gain, extra lift (docs/design.md 3.1). The output gain comes after
# the limiter; FM Station's dense 5-band lifts quiet material up to about 2 dB beyond the leveller's range.
$presets = [ordered]@{
    "Gentle" = @(-18, 12, -1, 0, 0); "Light" = @(-16, 9, -1, 0, 0); "Medium" = @(-14, 9, -1, 0, 0)
    "Loud" = @(-12, 12, -1, 0, 0); "Very Loud" = @(-10, 12, -1, 0, 0); "FM Station" = @(-9, 12, -1, -4, 2); "DAB+" = @(-23, 15, -1, 0, 0)
    "Web Stream" = @(-16, 9, -2, 0, 0)
}
$ceiling = -1.0 # Medium (44.1 kHz check)

"`nPresets (48 kHz): output I against the target - or input I +- the leveller range when that is as far as it goes"
"  {0,-10} {1,7} | {2,-46}" -f "preset", "target", "input -24 / -14 / -6 dBFS RMS: I, TP (audiotool)"
foreach ($p in $presets.Keys) {
    $target = $presets[$p][0]; $range = $presets[$p][1]; $gain = $presets[$p][3]; $lift = $presets[$p][4]
    $pc = $presets[$p][2] + $gain
    $cells = @()
    foreach ($l in $levels) {
        $out = Join-Path $Work ("{0}_{1}.wav" -f ($p -replace '[ +]', ''), $l)
        & $Exe --render (Join-Path $Work "in$l.wav") $out --a $p --ca Neutral | Out-Null # not the colour of the settings
        $m = toolMeasure $out
        $inI = $inputs[$l].integrated_lufs
        $expect = [math]::Max($inI - $range, [math]::Min($inI + $range + $lift, $target)) + $gain
        $cells += "{0,6:N2} {1,6:N2}" -f $m.integrated_lufs, $m.true_peak_dbtp
        $script:rows += , @($p, $l, $m, $expect, $out, $pc, ($target + $gain))
    }
    "  {0,-10} {1,7:N1} | {2}" -f $p, ($target + $gain), ($cells -join "  |")
}
foreach ($r in $script:rows) {
    $p, $l, $m, $expect, $out, $pc, $aim = $r
    # within 1 LU of what the leveller reaches, or closer to the target (a compressor may take a loud input further)
    $lo = [math]::Min($expect, $aim) - 1.0; $hi = [math]::Max($expect, $aim) + 1.0
    check ($m.integrated_lufs -ge $lo -and $m.integrated_lufs -le $hi) ("{0} at {1} dBFS: I {2:N2} LUFS, expected {3:N2} +- 1 (or nearer {4:N1})" -f $p, $l, $m.integrated_lufs, $expect, $aim)
    check ($m.true_peak_dbtp -le $pc + 0.1) ("{0} at {1} dBFS: true peak {2:N2} dBTP <= {3:N1} + 0.1" -f $p, $l, $m.true_peak_dbtp, $pc)
    check ($m.full_scale_samples -eq 0) ("{0} at {1} dBFS: no samples at full scale ({2})" -f $p, $l, $m.full_scale_samples)
}

"`nThe program's meter against audiotool (same files)"
foreach ($r in $script:rows | Select-Object -First 6) {
    $p, $l, $m, $expect, $out, $pc, $aim = $r
    $o = ownMeasure $out
    check ([math]::Abs($o.I - $m.integrated_lufs) -le 0.1 -and [math]::Abs($o.TP - $m.true_peak_dbtp) -le 0.3) `
        ("{0} at {1}: I {2:N2} / {3:N2} LUFS, true peak {4:N2} / {5:N2} dBTP (program / audiotool)" -f $p, $l, $o.I, $m.integrated_lufs, $o.TP, $m.true_peak_dbtp)
}

"`n44.1 kHz"
& $Exe --render (Join-Path $Work "in441.wav") (Join-Path $Work "Medium_441.wav") --a Medium --ca Neutral | Out-Null
$m = toolMeasure (Join-Path $Work "Medium_441.wav")
check ([math]::Abs($m.integrated_lufs - (-14)) -le 1.0 -and $m.true_peak_dbtp -le $ceiling + 0.1) ("Medium at 44.1 kHz: I {0:N2} LUFS, true peak {1:N2} dBTP" -f $m.integrated_lufs, $m.true_peak_dbtp)

"`nBit-exact paths (audiotool compare)"
& $Exe --render (Join-Path $Work "in-14.wav") (Join-Path $Work "bypass.wav") --a off | Out-Null
$c = tool compare (Join-Path $Work "in-14.wav") (Join-Path $Work "bypass.wav")
check (($c -match 'bit-exact.*: yes') -and ($c -match 'delay of b against a: 0 frames')) "deck A off: output = input, no delay"
# a preset with every stage off
$ini = Join-Path $Work "neutral.ini"
$switches = ("LevellerOn", "CompressorOn", "DensityOn", "BassOn", "ExciterOn", "DeEsserOn", "WidthOn", "ClipperOn", "LimiterOn")
$neutral = "[Preset Neutral]`r`n" + (($switches | ForEach-Object { "$_=0" }) -join "`r`n") + "`r`n"
[System.IO.File]::WriteAllText($ini, $neutral)
& $Exe --render (Join-Path $Work "in-24.wav") (Join-Path $Work "neutral.wav") --a Neutral --ca Neutral --ini $ini | Out-Null
$c = tool compare (Join-Path $Work "in-24.wav") (Join-Path $Work "neutral.wav")
check ($c -match "bit-exact.*: yes") "every stage off (processing on): output = input (peaks under full scale: -24 dBFS song; the clip guard holds +-1 when processing is on)"
# delay kept: exactly the look-ahead (1.5 ms = 72 frames at 48 kHz)
& $Exe --render (Join-Path $Work "in-14.wav") (Join-Path $Work "delay.wav") --a off --keep-delay | Out-Null
$c = tool compare (Join-Path $Work "in-14.wav") (Join-Path $Work "delay.wav")
check (($c -match 'delay of b against a: 72 frames') -and ($c -match 'bit-exact.*: yes')) "--keep-delay: 72 frames late, bit-exact"

"`n$script:failures failed check(s)"
if (-not $Keep) { Remove-Item -Recurse -Force $Work }
exit $script:failures
