#!/usr/bin/env python3
import re
import sys
from pathlib import Path

MARK = re.compile(r"\[bench\] MARK (\S+)")
SWEEP = re.compile(r"\[native\] SWEEP (\S+) (\S*)\|(\S*): (.*)$")
SIDE = re.compile(r"(?P<fps>[\d.]+) fps \(frame (?P<frame>[\d.]+) ms.*?record (?P<record>[\d.]+).*?GPU (?P<gpu>[\d.]+); "
                  r"per frame (?P<draws>[\d.]+) draws(?:.*?GPU by group (?P<groups>[^)]*))?\)")
GROUP = re.compile(r"(dl [0-9A-F]{2}|copies|other) ([\d.]+)")


def parse_side(text: str) -> dict | None:
    m = SIDE.search(text)
    if not m:
        return None
    side = {k: float(m[k]) for k in ("fps", "frame", "record", "gpu", "draws")}
    side["groups"] = {name: float(v) for name, v in GROUP.findall(m["groups"] or "")}
    return side


def rows(text: str) -> list[dict]:
    out, mark = [], "-"
    for line in text.splitlines():
        if m := MARK.search(line):
            mark = m[1]
            continue
        m = SWEEP.search(line)
        if not m:
            continue
        name, a, b, rest = m[1], m[2], m[3], m[4]
        head_b = f" | {b}: "
        at = rest.rfind(head_b)
        if not rest.startswith(f"{a}: ") or at < 0:
            continue
        sa, sb = parse_side(rest[len(a) + 2:at]), parse_side(rest[at + len(head_b):])
        if sa and sb:
            out.append({"mark": mark, "name": name, "a": a, "b": b, "sa": sa, "sb": sb})
    return out


def main(argv: list[str]) -> int:
    data = rows(Path(argv[0]).read_text(errors="replace"))
    groups = ["dl 12", "dl 01", "dl 07", "dl 0C", "dl 17", "copies", "other"]
    head = f"{'point':<12} {'setting':<36} {'a|b':<14} {'GPU a':>6} {'GPU b':>6} {'b-a':>6} " \
           f"{'fps a':>5} {'fps b':>5} {'rec a':>5} {'rec b':>5} {'draw a':>6} {'draw b':>6}  groups b-a"
    print(head)
    for r in data:
        sa, sb = r["sa"], r["sb"]
        diff = " ".join(f"{g} {sb['groups'].get(g, 0) - sa['groups'].get(g, 0):+.2f}" for g in groups
                        if g in sa["groups"] or g in sb["groups"])
        print(f"{r['mark']:<12} {r['name'][:36]:<36} {(r['a'] or '-') + '|' + (r['b'] or '-'):<14} "
              f"{sa['gpu']:6.2f} {sb['gpu']:6.2f} {sb['gpu'] - sa['gpu']:+6.2f} {sa['fps']:5.1f} {sb['fps']:5.1f} "
              f"{sa['record']:5.1f} {sb['record']:5.1f} {sa['draws']:6.0f} {sb['draws']:6.0f}  {diff}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
