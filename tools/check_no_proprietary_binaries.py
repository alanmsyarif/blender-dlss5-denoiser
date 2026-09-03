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
FORBIDDEN = {"nvngx_dlssnr.dll", "_nvngx.dll"}

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


def find_proprietary(root: Path = ROOT) -> list[Path]:
    return [path for path in committable_files(root) if path.name.lower() in FORBIDDEN]


def main() -> int:
    found = find_proprietary()
    if found:
        print("Proprietary NVIDIA binaries are committable:", file=sys.stderr)
        for path in found:
            print(f"  {path}", file=sys.stderr)
        print("\nRemove them or add them to .gitignore before committing.", file=sys.stderr)
        return 1
    print("No proprietary NVIDIA binaries are committable.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
