#!/usr/bin/env python3
"""[r]evo Slicer: w paczce zostają tylko profile Revotech + biblioteka filamentów Orki.
Uruchamiane w CI przed budowaniem – w repo zostawiamy pełne profile, żeby co tydzień czysto łączyć zmiany z Orki."""
import shutil
from pathlib import Path

P = Path(__file__).resolve().parents[2] / "resources/profiles"
KEEP = {"Revotech", "OrcaFilamentLibrary"}
for f in P.glob("*.json"):
    if f.stem in KEEP or f.name == "blacklist.json":
        continue
    f.unlink()
    d = P / f.stem
    if d.is_dir():
        shutil.rmtree(d)
print("profile:", sorted(x.stem for x in P.glob("*.json")))
