import argparse
import collections
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser(description="Indirect call promotions (manifest indirect_promotions_file) from indirect_profile counts")
ap.add_argument("profiles", nargs="+", type=pathlib.Path)
ap.add_argument("--gen", type=pathlib.Path, default=root / "gtav/generated/default")
ap.add_argument("--out", type=pathlib.Path, default=root / "gtav/indirect_promotions.txt")
ap.add_argument("--min-share", type=float, default=0.005, help="percent of all indirect calls a site needs")
ap.add_argument("--cover", type=float, default=99.0, help="percent of all indirect calls at which the list stops")
ap.add_argument("--single", type=float, default=90.0, help="percent of a site its top target needs alone")
ap.add_argument("--pair", type=float, default=90.0, help="percent of a site the top two targets need together")
ap.add_argument("--second", type=float, default=5.0, help="percent of a site the second target needs")
ap.add_argument("--dominant", type=float, default=60.0, help="percent of a site a lone guarded target still needs")
a = ap.parse_args()

defined = set()
for f in a.gen.glob("*_recomp.*.cpp"):
    defined.update(int(x, 16) for x in re.findall(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)", f.read_text(), re.M))

sites = collections.defaultdict(collections.Counter)
for path in a.profiles:
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) != 3 or line.startswith("#"):
            continue
        sites[int(parts[0], 16)][int(parts[1], 16)] += int(parts[2])

total = sum(sum(c.values()) for c in sites.values())
if not total:
    sys.exit("indirect_promotions: no calls in the profiles")

rows = []
covered = caught = 0
skipped = collections.Counter()
for site, targets in sorted(sites.items(), key=lambda kv: -sum(kv[1].values())):
    calls = sum(targets.values())
    if 100.0 * covered / total >= a.cover or 100.0 * calls / total < a.min_share:
        skipped["cold"] += 1
        continue
    covered += calls
    ranked = [(t, n) for t, n in targets.most_common() if t in defined]
    if not ranked:
        skipped["not a generated function"] += 1
        continue
    top = ranked[:2]
    if 100.0 * top[0][1] >= a.single * calls:
        chosen = top[:1]
    elif len(top) == 2 and 100.0 * (top[0][1] + top[1][1]) >= a.pair * calls and 100.0 * top[1][1] >= a.second * calls:
        chosen = top
    elif 100.0 * top[0][1] >= a.dominant * calls:
        chosen = top[:1]
    else:
        skipped["polymorphic"] += 1
        continue
    hit = sum(n for _, n in chosen)
    caught += hit
    rows.append((site, [t for t, _ in chosen], calls, hit, len(targets)))

lines = [
    "# Indirect call sites promoted to guarded direct calls (codegen indirect_promotions_file).",
    f"# From {len(a.profiles)} indirect_profile run(s) of the prologue: {total} indirect calls at {len(sites)} sites.",
    f"# {len(rows)} sites, {100.0 * caught / total:.1f} % of all indirect calls hit a guard.",
    "# site target [target]  # calls at the site, share caught, distinct targets seen",
]
for site, chosen, calls, hit, distinct in sorted(rows):
    lines.append(f"{site:08X} {' '.join(f'{t:08X}' for t in chosen)}  # {calls} {100.0 * hit / calls:.1f}% {distinct}")
a.out.write_text("\n".join(lines) + "\n")
print(f"{len(rows)} sites, {100.0 * caught / total:.1f} % of {total} indirect calls caught; skipped {dict(skipped)}")
