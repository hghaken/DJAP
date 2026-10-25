# Listening renders: music copies through three presets, 24-bit WAV, into $Out.
# -Tracks: names (without .mp3) of the copies to render; default every .mp3 in $Music.
param([string]$Music, [string]$Exe, [string]$Out, [string[]]$Tracks)
if (-not $Tracks) { $Tracks = Get-ChildItem $Music -Filter *.mp3 | Sort-Object Name | ForEach-Object { $_.BaseName } }
$presets = "Gentle", "Medium", "Very Loud"
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($t in $Tracks) {
    foreach ($p in $presets) {
        $name = "{0} - {1}.wav" -f $t, $p
        & $Exe --render (Join-Path $Music "$t.mp3") (Join-Path $Out $name) --a $p --pcm24 | Out-Null
        Write-Output $name
    }
}
Write-Output "done"
