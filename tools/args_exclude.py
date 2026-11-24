import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
build = pathlib.Path(sys.argv[1])
probes = set(re.findall(r"GTAV_PROBE\(([0-9A-F]{8})", (root / "gtav/src/native/d3d_probe_list.inc").read_text()))
hooks = set()
for obj in (build / "CMakeFiles/gtav.dir").rglob("*.o"):
    out = subprocess.run(["nm", "-gU", str(obj)], capture_output=True, text=True, check=True).stdout
    hooks.update(re.findall(r" T _?(sub_[0-9A-F]{8})$", out, re.M))
declared = set()
for src in (root / "gtav/src").rglob("*.cpp"):
    declared.update(re.findall(r"REX_HOOK_RAW\((sub_[0-9A-F]{8})\)", src.read_text()))
hooks |= declared
names = sorted(h for h in hooks if h[4:] not in probes or h in declared)
(root / "gtav/args_in_registers_exclude.txt").write_text("".join(f"{n}\n" for n in names))
print(f"{len(names)} hooked functions ({len(hooks) - len(names)} D3D probes left out)")
