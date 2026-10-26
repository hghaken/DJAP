# Publishes the current commit to the public GitHub repository as one new commit on its main branch: the code without
# the private working files (agent instructions, the hand-over), under the account's anonymous noreply address. The
# local history stays private and is never pushed.
# Usage: powershell -ExecutionPolicy Bypass -File tools\publish.ps1 [-Message <text>] [-DryRun] [-Release [-Notes <text>]]
#   -Release  also a GitHub release v<version> on the public commit with release\DJAP-Setup-<version>.exe and its
#             .sha256 (what the program's update check finds); the setup must be built from this commit (no +dev)
# Safety net: tools\publish-blocklist.txt (git-ignored, one word or phrase per line) - publishing stops when any of
# them occurs in the public files or the message.
#   -NotesFile <file>  the release text from a file (Markdown, UTF-8): quotes and line breaks in -Notes do not
#             survive "powershell -File" (a release text was cut off at its first double quote)
param([string]$Message, [switch]$DryRun, [switch]$Release, [string]$Notes, [string]$NotesFile,
      [string]$Remote = "origin", [string]$Branch = "main")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$private = @('AGENTS.md', 'CLAUDE.md', '.claude', 'docs/handover.md')

if (git status --porcelain --untracked-files=no) { throw "commit or stash the changes first: the public commit is made from HEAD" }

# the public tree: HEAD without the private files, built in a separate index
$index = Join-Path ([IO.Path]::GetTempPath()) ("djap-publish-" + [Guid]::NewGuid() + ".index")
$env:GIT_INDEX_FILE = $index
try {
    git read-tree HEAD
    git rm --cached -r -q --ignore-unmatch -- @private | Out-Null
    $tree = (git write-tree).Trim()
} finally {
    Remove-Item Env:\GIT_INDEX_FILE
    Remove-Item $index -ErrorAction SilentlyContinue
}

$version = "{0}.{1}" -f (Get-Content VERSION -Raw).Trim(), (git rev-list --count HEAD).Trim()
if (-not $Message) { $Message = "DJAP $version`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" }
if ($NotesFile) { $Notes = [IO.File]::ReadAllText((Resolve-Path $NotesFile).Path) }
if (-not $Notes) { $Notes = "DJAP $version" }

# a release: the setup of exactly this commit, and programs that hold no word of the blocklist either
$setup = Join-Path $root "release\DJAP-Setup-$version.exe"
if ($Release) {
    if (-not (Test-Path $setup)) { throw "no release\DJAP-Setup-$version.exe: run tools\make_installer.ps1 first" }
    $built = (Get-Item "$root\build\DJAP.exe").VersionInfo.ProductVersion
    if ($built -ne $version) { throw "build\DJAP.exe is $built, not ${version}: build this commit and make the setup again" }
    if ((Get-Item $setup).LastWriteTime -lt (Get-Item "$root\build\DJAP.exe").LastWriteTime) {
        throw "the setup is older than build\DJAP.exe: run tools\make_installer.ps1 -NoBuild"
    }
}

# nothing personal: the blocklist against every public file and the message
$block = Join-Path $PSScriptRoot "publish-blocklist.txt"
if (-not (Test-Path $block)) { throw "tools\publish-blocklist.txt is missing: make it first (one forbidden word per line)" }
$words = Get-Content $block | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') }
$hits = @(git grep -n -i -F -f $block $tree -- 2>$null) # an array: += on $null would join the texts
foreach ($w in $words) { if ($Message -match [regex]::Escape($w)) { $hits += "message: $w" } }
foreach ($w in $words) { if ($Release -and $Notes -match [regex]::Escape($w)) { $hits += "notes: $w" } }
if ($Release) { # the setup is compressed: its programs are checked (ASCII and UTF-16 text)
    $plugin = "VST3\Release\DJAP.vst3\Contents\x86_64-win\DJAP.vst3", "VST3\Release\DJAP.vst3\Contents\Resources\moduleinfo.json"
    foreach ($exe in @("DJAP.exe", "DJAPCmd.exe") + ($plugin | Where-Object { Test-Path "$root\build\$_" })) {
        $bytes = [IO.File]::ReadAllBytes("$root\build\$exe")
        $text = [Text.Encoding]::GetEncoding(28591).GetString($bytes) + [Text.Encoding]::Unicode.GetString($bytes)
        foreach ($w in $words) { if ($text.IndexOf($w, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $hits += "${exe}: $w" } }
    }
}
if ($hits) { $hits | Select-Object -First 20 | ForEach-Object { Write-Output "  $_" }; throw "blocked words in the public version" }
foreach ($p in $private) { if (git ls-tree -r --name-only $tree -- $p) { throw "a private file is in the public tree: $p" } }

# the author: the GitHub account's noreply address, never the local git e-mail
$id = (gh api user --jq .id).Trim() # one field per call: PowerShell 5 strips the quotes of a jq string
$login = (gh api user --jq .login).Trim()
if (-not $id -or -not $login) { throw "gh is not signed in to GitHub (gh auth login)" }
$env:GIT_AUTHOR_NAME = $login; $env:GIT_COMMITTER_NAME = $login
$env:GIT_AUTHOR_EMAIL = "$id+$login@users.noreply.github.com"; $env:GIT_COMMITTER_EMAIL = $env:GIT_AUTHOR_EMAIL

$parent = $null # the public branch's tip; none: the first publish (no remote yet, or an empty repository)
if ((git remote) -contains $Remote) {
    $ErrorActionPreference = 'Continue' # git writes to stderr when the branch does not exist yet
    git fetch -q $Remote $Branch 2>&1 | Out-Null
    $fetched = $LASTEXITCODE -eq 0
    $ErrorActionPreference = 'Stop'
    if ($fetched) { $parent = (git rev-parse FETCH_HEAD).Trim() }
}

# the release on the public commit: the setup and its checksum (GitHub also publishes a digest of each file)
function New-Release([string]$target) {
    $tag = "v$version"
    $ErrorActionPreference = 'Continue'
    gh release view $tag 2>&1 | Out-Null
    $exists = $LASTEXITCODE -eq 0
    $ErrorActionPreference = 'Stop'
    if ($exists) { throw "release $tag exists already" }
    $hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
    $dir = Join-Path ([IO.Path]::GetTempPath()) ("djap-release-" + [Guid]::NewGuid())
    New-Item -ItemType Directory $dir | Out-Null
    try {
        $sum = Join-Path $dir "DJAP-Setup-$version.exe.sha256"
        $notesFile = Join-Path $dir "notes.txt"
        $utf8 = New-Object Text.UTF8Encoding $false
        [IO.File]::WriteAllText($sum, "$hash  DJAP-Setup-$version.exe`n", $utf8)
        [IO.File]::WriteAllText($notesFile, $Notes, $utf8)
        if ($DryRun) { Write-Output "dry run: release $tag on $target not made (setup sha256 $hash)"; return }
        gh release create $tag $setup $sum --target $target --title "DJAP $version" --notes-file $notesFile
        if ($LASTEXITCODE -ne 0) { throw "gh release create failed" }
    } finally {
        Remove-Item $dir -Recurse -ErrorAction SilentlyContinue
    }
    Write-Output "release $tag (setup sha256 $hash)"
}

if ($parent -and (git rev-parse "$parent^{tree}").Trim() -eq $tree) {
    Write-Output "nothing new to publish"
    if ($Release) { New-Release $parent }
    return
}
# the message through a file without a byte-order mark (PowerShell 5 puts one before piped text)
$msgFile = Join-Path ([IO.Path]::GetTempPath()) ("djap-publish-" + [Guid]::NewGuid() + ".txt")
[IO.File]::WriteAllText($msgFile, $Message, (New-Object Text.UTF8Encoding $false))
try {
    $commit = if ($parent) { (git commit-tree $tree -p $parent -F $msgFile).Trim() } else { (git commit-tree $tree -F $msgFile).Trim() }
} finally {
    Remove-Item $msgFile -ErrorAction SilentlyContinue
}
Write-Output ("public commit {0} ({1} files) by {2}" -f $commit, (git ls-tree -r --name-only $tree).Count, $env:GIT_AUTHOR_EMAIL)
if ($DryRun) {
    Write-Output "dry run: not pushed"
    if ($Release) { New-Release $commit }
    return
}
git push $Remote "${commit}:refs/heads/$Branch"
if ($LASTEXITCODE -ne 0) { throw "push failed" }
if ($Release) { New-Release $commit }
