"""Fail if a proprietary NVIDIA runtime could leave this machine.

Scope is deliberately "everything git would include in a commit": tracked files
plus untracked files that are not ignored. A DLL sitting in an ignored path such
as runtime/ is the documented local setup (see RUNTIME_SETUP.md), not a
redistribution risk, so it must not fail this check.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Named so the failure message can say exactly what was found. The magic-number
# check below is what actually provides the guarantee: matching on names alone
# missed a renamed copy, and "nvngx_dlssnr - Copy.dll" redistributes just as
# much of NVIDIA's runtime as the original does.
FORBIDDEN = {"nvngx_dlssnr.dll", "_nvngx.dll"}

# This repository is source-only, so no committable file should ever be a
# Windows PE image. Broader and simpler than enumerating filenames.
PE_MAGIC = b"MZ"

# Only used when git is unavailable; keeps the walk away from build output.
SKIP_DIRS = {".git", "blender-src", "build", "dist", "__pycache__", "bin", "caller"}


def committable_files(root: Path) -> list[Path]:
    """Files git would include in a commit: tracked, plus unignored untracked."""
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return [
            path
            for path in root.rglob("*")
            if path.is_file() and not SKIP_DIRS.intersection(path.relative_to(root).parts)
        ]
    return [root / name for name in result.stdout.split("\0") if name]


def is_pe_binary(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(2) == PE_MAGIC
    except OSError:
        # A path git lists but we cannot open is not something we can ship.
        return False


def find_proprietary(root: Path = ROOT) -> list[tuple[Path, str]]:
    found = []
    for path in committable_files(root):
        if path.name.lower() in FORBIDDEN:
            found.append((path, "matches a known NVIDIA runtime filename"))
        elif is_pe_binary(path):
            found.append((path, "is a Windows PE binary"))
    return found


def main() -> int:
    found = find_proprietary()
    if found:
        print("Binaries are committable:", file=sys.stderr)
        for path, reason in found:
            print(f"  {path} ({reason})", file=sys.stderr)
        print("\nRemove them or add them to .gitignore before committing.", file=sys.stderr)
        return 1
    print("No proprietary NVIDIA binaries are committable.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
