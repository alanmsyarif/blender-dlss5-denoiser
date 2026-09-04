$ErrorActionPreference = 'Stop'
$ExpectedCommit = 'a3db93c5b2595a79f65f304114c23aeef8c2139f'
$Root = Split-Path -Parent $PSScriptRoot
$Source = Join-Path $Root 'blender-src'

if (Test-Path $Source) {
  throw "Source directory already exists: $Source"
}

# Clone from projects.blender.org, not the GitHub mirror: the mirror does not
# host Blender's Git LFS objects, so a normal clone dies part way through
# checkout with "Object does not exist on the server: [404]" and leaves an
# empty index.
#
# GIT_LFS_SKIP_SMUDGE keeps the clone fast by checking LFS assets out as
# pointer files, then only the ones the build actually consumes are pulled
# back. Most of the ~6400 LFS files are brush and asset .blend libraries worth
# many GB, but 'release/datafiles' is different: datatoc bakes those into the
# executable, so a pointer file there is compiled in as if it were the asset.
# Blender then starts, fails to parse the 131-byte startup.blend with
# "Failed to read file '', not a blend file", and dies on the null Main with
# EXCEPTION_ACCESS_VIOLATION before Python is even up.
#
# Run 'git -C blender-src lfs pull' for the full asset libraries in a
# packaged build. That is optional; the pull below is not.
$PreviousSkipSmudge = $env:GIT_LFS_SKIP_SMUDGE
$env:GIT_LFS_SKIP_SMUDGE = '1'
try {
  # core.autocrlf must be off or the patch, which is LF, fails every hunk
  # against a CRLF working tree. core.longpaths avoids checkout failures on
  # Blender's deeper paths.
  git clone --branch v5.0.1 --depth 1 `
    --config core.autocrlf=false `
    --config core.longpaths=true `
    https://projects.blender.org/blender/blender.git $Source
  if ($LASTEXITCODE -ne 0) { throw 'Failed to clone Blender.' }
}
finally {
  $env:GIT_LFS_SKIP_SMUDGE = $PreviousSkipSmudge
}
# ~195 files, about 10 MB, against ~20 GB for every LFS object in the repo.
git -C $Source lfs pull --include="release/datafiles/**"
if ($LASTEXITCODE -ne 0) { throw 'Failed to pull release/datafiles LFS objects.' }

$Commit = (git -C $Source rev-parse HEAD).Trim()
if ($Commit -ne $ExpectedCommit) {
  throw "Unexpected Blender commit $Commit; expected $ExpectedCommit."
}
Write-Host "Blender v5.0.1 ready at $Source"
