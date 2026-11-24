import bisect
import collections
import re
import subprocess
import sys


def symbols(elf):
    out = subprocess.run(["nm", elf], capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tT":
            rows.append((int(parts[0], 16), parts[2]))
    rows.sort()
    return [a for a, _ in rows], [n for _, n in rows]


def weights(run):
    addrs, names = symbols(f"{run}/gtav.elf")
    pc = collections.Counter()
    stack = collections.Counter()
    playing = False
    samples = running = 0
    for line in open(f"{run}/rex_profile.log", errors="replace"):
        m = re.match(r"==== .*CPU total (\d+)%", line)
        if m:
            playing = int(m.group(1)) >= 150
            continue
        if not playing:
            continue
        m = re.match(r"\s+(\d+) samples, (\d+)% waiting", line)
        if m:
            samples = int(m.group(1))
            running = samples * (100 - int(m.group(2))) / 100
            continue
        m = re.match(r"\s+([\d.]+)%\s+(pc|stack) (.*)", line)
        if not m:
            continue
        share = float(m.group(1)) / 100
        frames = {names[bisect.bisect_right(addrs, int(x, 16)) - 1] for x in re.findall(r"image\+0x([0-9a-f]+)", m.group(3))}
        for name in frames:
            if m.group(2) == "pc":
                pc[name] += share * running
            else:
                stack[name] += share * samples
    return pc, stack


def sections(name):
    m = re.fullmatch(r"(?:__imp__|__fast_)?(sub_[0-9A-F]{8})", name)
    if m:
        return [f".text.__imp__{m.group(1)}", f".text.__fast_{m.group(1)}", f".text.{m.group(1)}"]
    return [f".text.{name}"]


def main():
    out, runs = sys.argv[1], sys.argv[2:]
    pc = collections.Counter()
    stack = collections.Counter()
    for run in runs:
        p, s = weights(run)
        pc.update(p)
        stack.update(s)
    order = sorted(set(pc) | set(stack), key=lambda n: (-pc[n], -stack[n], n))
    seen = set()
    lines = [".text : {", "  KEEP (*(.crt0))", "  *(.text.rex_code_blocks_head)"]
    for name in order:
        for sec in sections(name):
            if sec not in seen:
                seen.add(sec)
                lines.append(f"  *({sec})")
    lines.append("  *(.text.rex_code_blocks_hot_end)")
    lines.append("}")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{out}: {len(seen)} sections from {len(order)} functions")


main()
