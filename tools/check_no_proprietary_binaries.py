from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN = {"nvngx_dlssnr.dll", "_nvngx.dll"}
found = [path for path in ROOT.rglob("*") if path.is_file() and path.name.lower() in FORBIDDEN]
if found:
    raise SystemExit("Proprietary NVIDIA binaries found:\n" + "\n".join(map(str, found)))
print("No proprietary NVIDIA binaries found.")
