# Publishes the current commit to the public GitHub repository as one new commit on its main branch: the code without
# the private working files (agent instructions, the hand-over), under the account's anonymous noreply address. The
# local history stays private and is never pushed.
# Usage: powershell -ExecutionPolicy Bypass -File tools\publish.ps1 [-Message <text>] [-DryRun]
# Safety net: tools\publish-blocklist.txt (git-ignored, one word or phrase per line) - publishing stops when any of
# them occurs in the public files or the message.
param([string]$Message, [switch]$DryRun, [string]$Remote = "origin", [string]$Branch = "main")
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

# nothing personal: the blocklist against every public file and the message
$block = Join-Path $PSScriptRoot "publish-blocklist.txt"
if (-not (Test-Path $block)) { throw "tools\publish-blocklist.txt is missing: make it first (one forbidden word per line)" }
$words = Get-Content $block | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') }
$hits = git grep -n -i -F -f $block $tree -- 2>$null
foreach ($w in $words) { if ($Message -match [regex]::Escape($w)) { $hits += "message: $w" } }
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
if ($parent -and (git rev-parse "$parent^{tree}").Trim() -eq $tree) { Write-Output "nothing new to publish"; return }
# the message through a file without a byte-order mark (PowerShell 5 puts one before piped text)
$msgFile = Join-Path ([IO.Path]::GetTempPath()) ("djap-publish-" + [Guid]::NewGuid() + ".txt")
[IO.File]::WriteAllText($msgFile, $Message, (New-Object Text.UTF8Encoding $false))
try {
    $commit = if ($parent) { (git commit-tree $tree -p $parent -F $msgFile).Trim() } else { (git commit-tree $tree -F $msgFile).Trim() }
} finally {
    Remove-Item $msgFile -ErrorAction SilentlyContinue
}
Write-Output ("public commit {0} ({1} files) by {2}" -f $commit, (git ls-tree -r --name-only $tree).Count, $env:GIT_AUTHOR_EMAIL)
if ($DryRun) { Write-Output "dry run: not pushed"; return }
git push $Remote "${commit}:refs/heads/$Branch"
if ($LASTEXITCODE -ne 0) { throw "push failed" }
