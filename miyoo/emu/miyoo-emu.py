#!/usr/bin/env python3
"""Tests the port manager on an emulated Miyoo Mini (Plus): the ARM binary under qemu-arm
(Cortex-A7), with the panel, buttons and ioctl emulated by emu_shim.c.

    miyoo-emu.py run [scenario ...] [--bin B] [--out O] [-j N]
    miyoo-emu.py list

Scenarios are miyoo/emu/scenarios/*.scn. Each run writes:
<out>/<scenario>/: log.txt, shots/*.png, contact.png, sdcard/, and <out>/summary.{md,json}.
Exit code: 0 all passed, 1 something failed.
"""

import argparse
import hashlib
import json
import os
import re
import glob
import shlex
import shutil
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
PANEL_W, PANEL_H = 640, 480

KEYS = {"A", "B", "X", "Y", "L1", "R1", "L2", "R2", "UP", "DOWN", "LEFT", "RIGHT",
        "START", "SELECT", "MENU", "POWER", "VOLUP", "VOLDOWN"}


class Scenario:
    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]
        self.args = []
        self.frames = 0
        self.min_frames = None
        self.timeout = 180
        self.shots = []
        self.shot_every = 0
        self.nonblank = []
        self.events = []
        self.env = {}
        self.exit = 0
        self.title = ""
        self.assert_exists = []
        self.assert_contains = []
        self.assert_not_exists = []
        self.assert_not_contains = []
        self.assert_log = []
        self.assert_log_not = []
        self.assert_log_order = []
        self.files = []  # (path, text or None to remove) set up before the run
        self.offline = False
        self.parse()

    def parse(self):
        with open(self.path, encoding="utf-8") as f:
            lines = f.read().splitlines()
        for n, raw in enumerate(lines, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                if not self.title and raw.strip().startswith("#"):
                    self.title = raw.strip().lstrip("#").strip()
                continue
            for part in line.split(" / "):
                self.directive(shlex.split(part), n)
        if self.min_frames is None:
            self.min_frames = self.frames

    def directive(self, w, n):
        def fail(msg):
            raise SystemExit(f"{self.path}:{n}: {msg}")

        def key(k):
            k = k.upper()
            if k not in KEYS:
                fail(f"unknown button {k}")
            return k

        d, a = w[0], w[1:]
        try:
            if d == "args":
                self.args += a
            elif d == "frames":
                self.frames = int(a[0])
            elif d == "min_frames":
                self.min_frames = int(a[0])
            elif d == "timeout":
                self.timeout = int(a[0])
            elif d == "shots":
                for s in a:
                    if s.startswith("every:"):
                        self.shot_every = int(s[6:])
                    else:
                        self.shots.append(int(s))
            elif d == "nonblank":
                self.nonblank += [int(s) for s in a]
            elif d == "press":
                f = int(a[0])
                k = key(a[1])
                self.events += [(f, k, 1), (f, k, 0)]
            elif d in ("down", "up"):
                self.events.append((int(a[0]), key(a[1]), 1 if d == "down" else 0))
            elif d == "env":
                for env_entry in a:
                    if "=" in env_entry:
                        k, v = env_entry.split("=", 1)
                        self.env[k] = v
            elif d == "exit":
                self.exit = int(a[0])
            elif d == "assert_exists":
                self.assert_exists.append(a[0])
            elif d == "assert_contains":
                self.assert_contains.append((a[0], " ".join(a[1:])))
            elif d == "assert_not_exists":
                self.assert_not_exists.append(a[0])
            elif d == "assert_not_contains":
                self.assert_not_contains.append((a[0], " ".join(a[1:])))
            elif d == "assert_log":
                self.assert_log.append(" ".join(a))
            elif d == "assert_log_not":
                self.assert_log_not.append(" ".join(a))
            elif d == "assert_log_order":
                self.assert_log_order.append(a)
            elif d == "file":
                self.files.append((a[0], " ".join(a[1:]).replace("\\n", "\n")))
            elif d == "rm":
                self.files.append((a[0], None))
            elif d == "offline":
                self.offline = True
            else:
                fail(f"unknown directive {d}")
        except (IndexError, ValueError):
            fail("bad arguments: " + " ".join(w))


def find_scenarios(names):
    folder = os.path.join(HERE, "scenarios")
    if not names:
        if not os.path.isdir(folder):
            return []
        return [os.path.join(folder, f) for f in sorted(os.listdir(folder)) if f.endswith(".scn")]
    out = []
    for n in names:
        if os.path.exists(n):
            out.append(n)
        elif os.path.exists(os.path.join(folder, n + ".scn")):
            out.append(os.path.join(folder, n + ".scn"))
        else:
            raise SystemExit(f"no scenario {n}")
    return out


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():m.end() + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def image_stats(rgb):
    sample = [rgb[i:i + 3] for i in range(0, len(rgb), 3 * 97)]
    counts = {}
    for p in sample:
        counts[p] = counts.get(p, 0) + 1
    return len(counts), max(counts.values()) / len(sample)


def contact_sheet(path, shots, cols=4):
    if not shots:
        return
    tw, th = PANEL_W // 2, PANEL_H // 2
    rows = (len(shots) + cols - 1) // cols
    cols = min(cols, len(shots))
    W, H = cols * tw + (cols - 1) * 4, rows * th + (rows - 1) * 4
    sheet = bytearray(b"\x20" * (W * H * 3))
    for i, (_, rgb) in enumerate(shots):
        ox, oy = (i % cols) * (tw + 4), (i // cols) * (th + 4)
        for y in range(th):
            src = rgb[(y * 2) * PANEL_W * 3:(y * 2 + 1) * PANEL_W * 3]
            half = b"".join(src[x * 6:x * 6 + 3] for x in range(tw))
            o = ((oy + y) * W + ox) * 3
            sheet[o:o + tw * 3] = half
    write_png(path, W, H, bytes(sheet))


def find_tool(names):
    for n in names:
        if n and (os.path.isfile(n) or shutil.which(n)):
            return n if os.path.isfile(n) else shutil.which(n)
    return None


def run_scenario(sc, opts):
    out = os.path.join(opts.out, sc.name)
    res = {"name": sc.name, "title": sc.title, "status": "pass", "problems": [], "warnings": []}

    shutil.rmtree(out, ignore_errors=True)
    shots_dir = os.path.join(out, "shots")
    sdcard = os.path.join(out, "sdcard")
    app_dir = os.path.join(out, "app")
    os.makedirs(shots_dir, exist_ok=True)
    os.makedirs(app_dir, exist_ok=True)

    # 1. Copy test SD card template
    template_sd = os.path.join(HERE, "test_sd")
    if os.path.isdir(template_sd):
        shutil.copytree(template_sd, sdcard)
    else:
        os.makedirs(sdcard, exist_ok=True)

    # Every file executable, as on the card's FAT (rwxrwxrwx): Port Manager must not take that
    # for a sign of program files (it once deleted a port's ROM and saves that way); this also
    # covers the helper scripts in .tmp_update/bin
    bin_dir = os.path.join(sdcard, ".tmp_update", "bin")
    for dirpath, _, files in os.walk(sdcard):
        for fname in files:
            try:
                os.chmod(os.path.join(dirpath, fname), 0o777)
            except OSError:
                pass

    # 2. Copy binary and catalog into app_dir
    app_bin = os.path.join(app_dir, "portmanager")
    shutil.copy2(opts.bin, app_bin)
    try:
        os.chmod(app_bin, 0o755)
    except OSError:
        pass

    cat_src = os.path.join(REPO, "catalog.json")
    if os.path.isfile(cat_src):
        shutil.copy2(cat_src, os.path.join(app_dir, "catalog.json"))

    # "app/..." is Port Manager's own folder, anything else is on the card
    def where(p):
        return os.path.join(app_dir, p[4:]) if p.startswith("app/") else os.path.join(sdcard, p)

    # The scenario's own state: "{pid}" is a live process (this runner), e.g. for a lock
    for p, text in sc.files:
        full = where(p)
        if text is None:
            if os.path.isdir(full):
                shutil.rmtree(full)
            elif os.path.exists(full):
                os.remove(full)
            continue
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text.replace("{pid}", str(os.getpid())))
        os.chmod(full, 0o777)

    # 3. Write scripted input
    script = os.path.join(out, "input.txt")
    with open(script, "w") as f:
        for frame, k, down in sorted(sc.events, key=lambda e: e[0]):
            f.write(f"{frame} {k} {down}\n")

    # 4. Prepare environment
    env = dict(os.environ)
    target_env = {
        "MIYOO_EMU_DIR": shots_dir,
        "MIYOO_EMU_INPUT": script,
        "MIYOO_EMU_SHOTS": ",".join([str(s) for s in sorted(set(sc.shots))] + ([f"every:{sc.shot_every}"] if sc.shot_every else [])),
        "MIYOO_EMU_QUIT": str(sc.frames),
        "PM_SD": os.path.abspath(sdcard),
        "PATH": f"{os.path.abspath(bin_dir)}:{os.environ.get('PATH', '')}",
    }
    if sc.offline:
        target_env["EMU_OFFLINE"] = "1"  # the wget / nslookup stubs fail as with no Wi-Fi
    target_env.update(sc.env)
    env.update(target_env)

    # 5. Launch under qemu-arm
    cmd = [opts.qemu, "-cpu", "cortex-a7", os.path.abspath(app_bin)] + sc.args
    res["command"] = " ".join([os.path.abspath(app_bin)] + sc.args)
    log_path = os.path.join(out, "log.txt")
    t0 = time.time()
    with open(log_path, "w") as log:
        log.write("$ " + " ".join(cmd) + "\n")
        log.flush()
        try:
            p = subprocess.run(cmd, cwd=app_dir, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=sc.timeout)
            code = p.returncode
        except subprocess.TimeoutExpired:
            code = None
    res["wall_s"] = round(time.time() - t0, 1)

    with open(log_path, errors="replace") as f:
        text = f.read()

    frames = 0
    for m in re.finditer(r"emu: (?:frame|exit after|all scripted events consumed \(frame) (\d+)", text):
        frames = max(frames, int(m.group(1)))
    res["frames"] = frames
    res["exit_code"] = code

    m = re.findall(r"heap peak ([\d.]+) MB", text)
    heap = float(m[-1]) if m else None
    res["heap_peak_mb"] = heap

    if code is None:
        res["hung"] = True
        res["problems"].append(f"hung: no exit after {sc.timeout} s (last frame {frames})")
    elif code != sc.exit:
        res["problems"].append(f"exit code {code}, expected {sc.exit}")

    crash = re.search(r"emu: CRASH signal (\d+) at frame (\d+)", text)
    if crash or "uncaught target signal" in text:
        res["problems"].append("crash in emulated binary")

    if sc.min_frames and frames < sc.min_frames:
        res["problems"].append(f"only {frames} frames shown, expected at least {sc.min_frames}")

    # Check assertions on the SD card
    # Check assertions (paths may hold glob patterns, e.g. a backup's time stamp)
    def found(p):
        if "*" in p:
            return glob.glob(where(p))
        return [where(p)] if os.path.exists(where(p)) else []

    def read(p):
        hits = found(p)
        if not hits:
            return None
        with open(hits[0], errors="replace") as f:
            return f.read()

    for p in sc.assert_exists:
        if not found(p):
            res["problems"].append(f"assertion failed: {p} does not exist")
    for p, txt in sc.assert_contains:
        c = read(p)
        if c is None:
            res["problems"].append(f"assertion failed: {p} does not exist (cannot check contents)")
        elif txt not in c:
            res["problems"].append(f"assertion failed: '{txt}' not found in {p} (got: {c.strip()[:200]})")
    for p, txt in sc.assert_not_contains:
        c = read(p)
        if c is None:
            res["problems"].append(f"assertion failed: {p} does not exist (cannot check contents)")
        elif txt in c:
            res["problems"].append(f"assertion failed: '{txt}' found in {p} but should not be")
    for p in sc.assert_not_exists:
        if found(p):
            res["problems"].append(f"assertion failed: {p} exists but should not")

    # ... and on Port Manager's log (its lines show as "pm: ..." in log.txt)
    for txt in sc.assert_log:
        if txt not in text:
            res["problems"].append(f"assertion failed: log has no '{txt}'")
    for txt in sc.assert_log_not:
        if txt in text:
            res["problems"].append(f"assertion failed: log has '{txt}' but should not")
    for seq in sc.assert_log_order:
        pos = 0
        for txt in seq:
            i = text.find(txt, pos)
            if i < 0:
                res["problems"].append(f"assertion failed: log has no '{txt}' after '{seq[0]}' (order: {' -> '.join(seq)})")
                break
            pos = i + len(txt)

    # Screenshots
    shots, res["shots"] = [], {}
    for name in sorted(os.listdir(shots_dir)):
        if not name.endswith(".ppm"):
            continue
        frame = int(re.search(r"(\d+)", name).group(1))
        w, h, rgb = read_ppm(os.path.join(shots_dir, name))
        png = os.path.join(shots_dir, f"frame_{frame:05d}.png")
        write_png(png, w, h, rgb)
        os.remove(os.path.join(shots_dir, name))
        colours, top = image_stats(rgb)
        res["shots"][frame] = {"md5": hashlib.md5(rgb).hexdigest(), "colours": colours, "top_share": round(top, 3)}
        shots.append((frame, rgb))

    for f in sc.nonblank:
        s = res["shots"].get(f)
        if not s:
            res["problems"].append(f"no screenshot of frame {f}")
        elif s["colours"] <= 1:
            res["problems"].append(f"frame {f} is blank (one colour)")

    if shots and all(s["colours"] <= 1 for s in res["shots"].values()):
        res["problems"].append("every screenshot is blank")

    contact_sheet(os.path.join(out, "contact.png"), shots)

    if res["problems"]:
        res["status"] = "fail"
    return res


def report(results, opts):
    icon = {"pass": "PASS", "fail": "FAIL", "skip": "skip"}
    md = ["## Port Manager on the emulated Miyoo Mini", "",
          f"Binary `{os.path.relpath(opts.bin, REPO) if opts.bin.startswith(REPO) else opts.bin}`, qemu-arm Cortex-A7.",
          "",
          "| scenario | result | frames | exit | wall | heap peak |",
          "|---|---|---|---|---|---|"]
    for r in results:
        heap = f"{r['heap_peak_mb']:.1f} MB" if r.get("heap_peak_mb") is not None else ""
        md.append(f"| {r['name']} | {icon[r['status']]} | {r.get('frames', '')} | {r.get('exit_code', '')} | "
                  f"{r.get('wall_s', '')} s | {heap} |")
    for r in results:
        if r["problems"] or r["warnings"]:
            md += ["", f"### {r['name']}: {icon[r['status']]}"]
            md += [f"- {p}" for p in r["problems"]]
            md += [f"- warning: {w}" for w in r["warnings"][:10]]
    text = "\n".join(md) + "\n"

    with open(os.path.join(opts.out, "summary.md"), "w") as f:
        f.write(text)
    with open(os.path.join(opts.out, "summary.json"), "w") as f:
        json.dump({"results": results}, f, indent=1)
    print(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["run", "list"])
    ap.add_argument("scenarios", nargs="*")
    ap.add_argument("--bin", default=os.path.join(REPO, "build", "miyoo_emu", "portmanager_emu"),
                    help="the emulator build of the port manager")
    ap.add_argument("--out", default=os.path.join(HERE, "results"))
    ap.add_argument("--qemu", default=None, help="qemu-arm (default: from PATH)")
    opts = ap.parse_args()

    paths = find_scenarios(opts.scenarios)
    if opts.command == "list":
        for p in paths:
            sc = Scenario(p)
            print(f"{sc.name:24} {sc.title}")
        return 0

    opts.qemu = opts.qemu or find_tool(["qemu-arm", "qemu-arm-static"])
    if not opts.qemu:
        raise SystemExit("qemu-arm not found (apt install qemu-user, or miyoo/emu/setup.sh)")
    opts.bin = os.path.abspath(opts.bin)
    if not os.path.isfile(opts.bin):
        raise SystemExit(f"no binary {opts.bin}")
    opts.out = os.path.abspath(opts.out)
    os.makedirs(opts.out, exist_ok=True)

    scenarios = [Scenario(p) for p in paths]
    results = []
    for sc in scenarios:
        res = run_scenario(sc, opts)
        results.append(res)
    report(results, opts)
    return 1 if any(r["status"] == "fail" for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
