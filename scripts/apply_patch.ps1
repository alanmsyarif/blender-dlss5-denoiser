$ErrorActionPreference = 'Stop'
$ExpectedCommit = 'a3db93c5b2595a79f65f304114c23aeef8c2139f'
$Root = Split-Path -Parent $PSScriptRoot
$Source = Join-Path $Root 'blender-src'
$Patch = Join-Path $Root 'patches\blender-v5.0.1-dlss5nr.patch'

if (!(Test-Path (Join-Path $Source '.git'))) { throw 'Run scripts\fetch_blender.ps1 first.' }
$Commit = (git -C $Source rev-parse HEAD).Trim()
if ($Commit -ne $ExpectedCommit) { throw "Expected Blender v5.0.1 commit $ExpectedCommit, got $Commit." }

$Status = git -C $Source status --porcelain
if ($Status) {
  # Not '2>$null': in PowerShell 5.1 redirecting a native command's stderr
  # raises NativeCommandError, which $ErrorActionPreference='Stop' turns into a
  # terminating error even when git exits 0.
  & git -C $Source apply --reverse --check $Patch *>&1 | Out-Null
  if ($LASTEXITCODE -eq 0) {
    Write-Host 'Patch is already applied.'
    exit 0
  }
  throw 'Blender checkout has unrelated changes. Use a clean checkout.'
}

git -C $Source apply --check $Patch
if ($LASTEXITCODE -ne 0) { throw 'Patch does not apply cleanly.' }
git -C $Source apply $Patch
if ($LASTEXITCODE -ne 0) { throw 'Patch application failed.' }
Write-Host 'DLSS 5 NR patch applied.'
