$ErrorActionPreference = 'Stop'
$ExpectedCommit = 'a3db93c5b2595a79f65f304114c23aeef8c2139f'
$Root = Split-Path -Parent $PSScriptRoot
$Source = Join-Path $Root 'blender-src'

if (Test-Path $Source) {
  throw "Source directory already exists: $Source"
}

git clone --branch v5.0.1 --depth 1 https://github.com/blender/blender.git $Source
if ($LASTEXITCODE -ne 0) { throw 'Failed to clone Blender.' }
$Commit = (git -C $Source rev-parse HEAD).Trim()
if ($Commit -ne $ExpectedCommit) {
  throw "Unexpected Blender commit $Commit; expected $ExpectedCommit."
}
Write-Host "Blender v5.0.1 ready at $Source"
