# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

"""A/B testing of a running game on the device, without touching its controls.

Every subcommand drives the emulator over adb only: it launches games by intent, sets
debug.capy.* properties, edits the app's own settings (driver, resolution...) and takes
screenshots, logcat and image dumps. Results go to --out as PNG grids and a CSV.

  launch   start a game and wait until shader compilation settles
  props    sweep property values (or app options) and compare screenshots with a baseline
  skip     sweep skip_ps/skip_cs over the shaders of one traced frame, ranked by the change
  drivers  the same scene with the stock driver and with another one (e.g. Turnip)
  trace    record one frame trace (CapyPS5-Frame) to a text file
  dumps    save the color and depth targets after the given draws of a frame
  quirks   print the driver and the quirks in effect (CapyPS5-Quirks)

Examples (GOW, a PS4 title in ROMs/PS4/CUSA34386):

  python tools/capy_lab.py launch --title CUSA34386
  python tools/capy_lab.py props --title CUSA34386 --set sample_in_general=0,1 \\
      --set quirk_kill_exports_depth=0,1 --relaunch
  python tools/capy_lab.py skip --kind ps --target 0x22a798000 --top 12
  python tools/capy_lab.py drivers --title CUSA34386 --other Lemon-Ade_Turnip_Driver_Lemon-Ade-v0.0.7
  python tools/capy_lab.py dumps --draws 300 669 1229

Properties are named without the debug.capy. prefix. App options use opt:<Name> (e.g.
opt:ResolutionScale=500,1000); the app's value wins over a property of the same setting, so
options are edited in its settings file and restored afterwards. Changing an option or a
property that is read when shaders compile needs --relaunch.
"""
import argparse
import csv
import io
import re
import subprocess
import sys
import tarfile
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

PACKAGE = "org.capyps5.emulator.debug"
ACTIVITY = PACKAGE + "/org.capyps5.emulator.MainActivity"
PREFS = "shared_prefs/capyps5.xml"
DEFAULT_ROMS = "/storage/1B0C-13F4/ROMs/PS4"
# The performance overlay in the top-left corner (1920x1080 coordinates).
OVERLAY = (0, 0, 520, 120)


class Device:
    def __init__(self, serial: str):
        self.serial = serial

    def adb(self, *args: str, check: bool = True, binary: bool = False):
        result = subprocess.run(["adb", "-s", self.serial, *args], capture_output=True,
                                check=check)
        return result.stdout if binary else result.stdout.decode("utf-8", "replace")

    def shell(self, command: str, check: bool = True) -> str:
        return self.adb("shell", command, check=check)

    def getprop(self, name: str) -> str:
        return self.shell(f"getprop {name}").strip()

    def setprop(self, name: str, value: str) -> None:
        # An empty property cannot be set back to empty: -1 means "decided by default" for
        # the switches that support it, and the tool restores the previous value anyway.
        self.shell(f"setprop {name} {value if value != '' else '-1'}")

    def pid(self) -> str:
        return self.shell(f"pidof {PACKAGE}", check=False).strip()

    def screenshot(self) -> np.ndarray:
        png = self.adb("exec-out", "screencap", "-p", binary=True)
        return np.asarray(Image.open(io.BytesIO(png)).convert("RGB"))

    def logcat(self, *tags: str) -> str:
        filters = [f"{t}:*" for t in tags]
        return self.adb("logcat", "-d", *(["-s", *filters] if filters else []))

    def clear_logcat(self) -> None:
        self.adb("logcat", "-c", check=False)

    def run_as(self, command: str, binary: bool = False):
        return self.adb("exec-out", "run-as", PACKAGE, "sh", "-c", command, binary=binary)

    # The app's settings file (SharedPreferences): read, edit and restore.
    def prefs(self) -> str:
        return self.run_as(f"cat {PREFS}")

    def write_prefs(self, text: str) -> None:
        self.adb("shell", "am", "force-stop", PACKAGE)
        proc = subprocess.run(["adb", "-s", self.serial, "exec-in", "run-as", PACKAGE, "sh", "-c",
                               f"cat > {PREFS}"], input=text.encode("utf-8"), capture_output=True)
        if proc.returncode != 0:
            raise RuntimeError("could not write the app settings: " + proc.stderr.decode())


def set_pref(text: str, key: str, value: str) -> str:
    """Sets a <long> or <string> entry of a SharedPreferences XML, adding it if missing."""
    pattern = re.compile(r'<(long|int|string) name="%s"[^>]*?(/>|>.*?</string>)' % re.escape(key))
    found = pattern.search(text)
    kind = found.group(1) if found else ("long" if re.fullmatch(r"-?\d+", value) else "string")
    entry = (f'<string name="{key}">{value}</string>' if kind == "string"
             else f'<{kind} name="{key}" value="{value}" />')
    if found:
        return text[:found.start()] + entry + text[found.end():]
    return text.replace("</map>", f"    {entry}\n</map>")


# Launching and waiting.
def launch(dev: Device, args) -> None:
    game_dir = args.dir or f"{args.roms}/{args.title}"
    elf = args.elf or f"{game_dir}/eboot.bin"
    dev.adb("shell", "am", "force-stop", PACKAGE)
    dev.clear_logcat()
    dev.adb("shell", "am", "start", "-n", ACTIVITY, "--es", "launch_elf", elf, "--es", "launch_dir",
            game_dir)
    print(f"launched {elf}", flush=True)
    wait_settled(dev, args.quiet, args.timeout)
    if args.warmup:
        print(f"warm-up {args.warmup} s", flush=True)
        time.sleep(args.warmup)


def wait_settled(dev: Device, quiet: int, timeout: int) -> None:
    """Waits until no new pipeline compiles for `quiet` seconds (or the game dies)."""
    start = time.time()
    last_count, last_change = -1, time.time()
    while time.time() - start < timeout:
        if not dev.pid():
            raise RuntimeError("the emulator is not running (crashed or closed)")
        count = dev.logcat("CapyPS5-Compile").count("\n")
        if count != last_count:
            last_count, last_change = count, time.time()
        elif time.time() - last_change >= quiet:
            print(f"settled after {time.time() - start:.0f} s ({count} compile log lines)",
                  flush=True)
            return
        time.sleep(5)
    print("timeout waiting for compilation to settle", flush=True)


# Screenshots and metrics.
def capture(dev: Device, shots: int, interval: float):
    frames = []
    for i in range(shots):
        frames.append(dev.screenshot())
        if i + 1 < shots:
            time.sleep(interval)
    return frames


def mask_overlay(img: np.ndarray) -> np.ndarray:
    out = img.copy()
    sx, sy = img.shape[1] / 1920, img.shape[0] / 1080
    x0, y0, x1, y1 = OVERLAY
    out[int(y0 * sy):int(y1 * sy), int(x0 * sx):int(x1 * sx)] = 0
    return out


def metrics(frames, baseline, region):
    """Mean over the shots: white/black fractions in `region` and the change from baseline."""
    whites, blacks, diffs = [], [], []
    for f in frames:
        f = mask_overlay(f)
        h, w = f.shape[:2]
        x0, y0, x1, y1 = region
        sx, sy = w / 1920, h / 1080
        crop = f[int(y0 * sy):int(y1 * sy), int(x0 * sx):int(x1 * sx)].astype(np.int32).mean(2)
        whites.append(float((crop > 225).mean()))
        blacks.append(float((crop < 20).mean()))
        if baseline is not None:
            diffs.append(float(np.abs(f.astype(np.int32) - mask_overlay(baseline).astype(np.int32)).mean()))
    return {
        "white": np.mean(whites),
        "black": np.mean(blacks),
        "diff": np.mean(diffs) if diffs else 0.0,
    }


def grid(images, labels, path: Path, cell=(640, 360), columns=3) -> None:
    rows = (len(images) + columns - 1) // columns
    sheet = Image.new("RGB", (cell[0] * min(columns, len(images)), cell[1] * rows), "black")
    draw = ImageDraw.Draw(sheet)
    for i, (img, label) in enumerate(zip(images, labels)):
        x, y = cell[0] * (i % columns), cell[1] * (i // columns)
        sheet.paste(Image.fromarray(img).resize(cell), (x, y))
        draw.rectangle([x, y + cell[1] - 22, x + cell[0], y + cell[1]], fill="black")
        draw.text((x + 6, y + cell[1] - 18), label, fill="yellow")
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)
    print(f"grid: {path}", flush=True)


def write_csv(rows, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["variant", "white", "black", "diff_from_baseline"])
        for label, m in rows:
            writer.writerow([label, f"{m['white']:.4f}", f"{m['black']:.4f}", f"{m['diff']:.2f}"])
    print(f"table: {path}", flush=True)


def noise_row(dev: Device, args, baseline):
    """The same scene again, unchanged: how much the image moves by itself (animation)."""
    time.sleep(args.delay)
    again = capture(dev, args.shots, args.interval)
    return ("noise (no change)", metrics(again, baseline[0], args.region))


def print_rows(rows) -> None:
    for label, m in rows:
        print(f"  {label:<44} white {m['white']:.3f}  black {m['black']:.3f}  diff {m['diff']:6.2f}")


# Variants: properties and app options.
def parse_sets(items):
    variants = []
    for item in items:
        name, _, values = item.partition("=")
        for value in values.split(","):
            variants.append((name.strip(), value.strip()))
    return variants


def apply_variant(dev: Device, name: str, value: str, prefs_base: str):
    """Sets one property or app option; returns how to undo it."""
    if name.startswith("opt:") or name.startswith("pref:"):
        key = ("opt_" + name[4:]) if name.startswith("opt:") else name[5:]
        dev.write_prefs(set_pref(prefs_base, key, value))
        return lambda: dev.write_prefs(prefs_base)
    prop = name if name.startswith("debug.") else "debug.capy." + name
    previous = dev.getprop(prop)
    dev.setprop(prop, value)
    return lambda: dev.setprop(prop, previous)


def needs_launch(args) -> bool:
    return bool(args.title or args.dir or args.elf)


def cmd_props(dev: Device, args) -> None:
    out = Path(args.out)
    prefs_base = dev.prefs()
    uses_options = any(n.startswith(("opt:", "pref:")) for n, _ in parse_sets(args.set))
    if (uses_options or args.relaunch) and not (needs_launch(args) and args.relaunch):
        sys.exit("app options and --relaunch need --relaunch with --title (or --dir/--elf): "
                 "editing the app settings closes the game")
    if args.relaunch:
        launch(dev, args)
    baseline = capture(dev, args.shots, args.interval)
    rows = [("baseline", metrics(baseline, baseline[0], args.region)), noise_row(dev, args, baseline)]
    images, labels = [baseline[0]], ["baseline"]
    try:
        for name, value in parse_sets(args.set):
            label = f"{name}={value}"
            print(f"variant {label}", flush=True)
            undo = apply_variant(dev, name, value, prefs_base)
            try:
                if args.relaunch:
                    launch(dev, args)
                else:
                    time.sleep(args.delay)
                frames = capture(dev, args.shots, args.interval)
            finally:
                undo()
            rows.append((label, metrics(frames, baseline[0], args.region)))
            images.append(frames[0])
            labels.append(label)
    finally:
        if uses_options:
            dev.write_prefs(prefs_base)
    print_rows(rows)
    write_csv(rows, out / "props.csv")
    grid(images, labels, out / "props.png")
    if args.relaunch and needs_launch(args):
        print("relaunching with the original settings", flush=True)
        launch(dev, args)


# Frame traces.
def record_trace(dev: Device, path: Path, start: int = 0, max_lines: int = 200000,
                 seconds: int = 25) -> str:
    saved = {p: dev.getprop(f"debug.capy.{p}") for p in ("trace_from", "trace_max")}
    dev.setprop("debug.capy.trace_from", str(start))
    dev.setprop("debug.capy.trace_max", str(max_lines))
    dev.setprop("debug.capy.frame_trace", "0")
    dev.clear_logcat()
    dev.setprop("debug.capy.frame_trace", "1")
    deadline = time.time() + seconds
    text = ""
    while time.time() < deadline:
        time.sleep(3)
        text = dev.logcat("CapyPS5-Frame")
        if "frame end" in text:
            break
    dev.setprop("debug.capy.frame_trace", "0")
    for p, v in saved.items():
        dev.setprop(f"debug.capy.{p}", v)
    lines = [re.sub(r"^.*?CapyPS5-Frame: ", "", l) for l in text.splitlines() if "CapyPS5-Frame" in l]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")
    ends = [l for l in lines if l.startswith("frame end")]
    print(f"trace: {path} ({len(lines)} lines{', ' + ends[0] if ends else ', no frame end'})",
          flush=True)
    return "\n".join(lines)


def cmd_trace(dev: Device, args) -> None:
    record_trace(dev, Path(args.out) / "frame.txt", args.start, args.max_lines, args.seconds)


def shaders_in_trace(text: str, kind: str, target: str):
    """Shader addresses of one stage in a trace, optionally only draws to one rt0."""
    found = []
    for line in text.splitlines():
        if kind == "cs" and line.startswith("dispatch "):
            m = re.search(r"cs=(0x[0-9a-f]+)", line)
        elif kind == "ps" and line.startswith("draw "):
            if target and f"rt0={target}" not in line:
                continue
            m = re.search(r"ps=(0x[0-9a-f]+)", line)
        else:
            continue
        if m and m.group(1) != "0x0" and m.group(1) not in found:
            found.append(m.group(1))
    return found


def cmd_skip(dev: Device, args) -> None:
    out = Path(args.out)
    text = record_trace(dev, out / "frame.txt")
    shaders = shaders_in_trace(text, args.kind, args.target)
    if args.limit:
        shaders = shaders[:args.limit]
    prop = "debug.capy.skip_" + args.kind
    print(f"{len(shaders)} {args.kind} shaders to skip one at a time", flush=True)
    baseline = capture(dev, args.shots, args.interval)
    base_metrics = metrics(baseline, baseline[0], args.region)
    noise = noise_row(dev, args, baseline)
    print(f"noise floor: diff {noise[1]['diff']:.2f} (changes below it are animation)", flush=True)
    rows, shots = [], {}
    previous = dev.getprop(prop)
    try:
        for i, address in enumerate(shaders):
            dev.setprop(prop, address)
            time.sleep(args.delay)
            frames = capture(dev, args.shots, args.interval)
            m = metrics(frames, baseline[0], args.region)
            rows.append((address, m))
            shots[address] = frames[0]
            print(f"  [{i + 1}/{len(shaders)}] {address} diff {m['diff']:.2f} "
                  f"white {m['white']:.3f} black {m['black']:.3f}", flush=True)
    finally:
        dev.setprop(prop, previous or "0")
    rows.sort(key=lambda r: -r[1]["diff"])
    print("largest changes:")
    print_rows([("baseline", base_metrics), noise] + rows[:args.top])
    write_csv([("baseline", base_metrics), noise] + rows, out / f"skip_{args.kind}.csv")
    top = rows[:args.top]
    grid([baseline[0]] + [shots[a] for a, _ in top],
         ["baseline"] + [f"skip {a} diff {m['diff']:.1f}" for a, m in top], out / f"skip_{args.kind}.png")


def cmd_drivers(dev: Device, args) -> None:
    if not needs_launch(args):
        sys.exit("drivers needs --title (or --dir/--elf): the game is relaunched per driver")
    out = Path(args.out)
    prefs_base = dev.prefs()
    title = args.title or Path(args.dir or args.elf).parts[-1]
    key = f"game_{title}_gpu_driver"
    images, labels, rows = [], [], []
    baseline = None
    try:
        for driver in ["", args.other]:
            name = driver or "system (stock)"
            print(f"driver {name}", flush=True)
            dev.write_prefs(set_pref(prefs_base, key, driver))
            launch(dev, args)
            frames = capture(dev, args.shots, args.interval)
            baseline = baseline if baseline is not None else frames[0]
            rows.append((name, metrics(frames, baseline, args.region)))
            images.extend(frames[:3])
            labels.extend([f"{name} #{i}" for i in range(len(frames[:3]))])
    finally:
        dev.write_prefs(prefs_base)
    print_rows(rows)
    write_csv(rows, out / "drivers.csv")
    grid(images, labels, out / "drivers.png")


def load_dump(path: Path):
    data = path.read_bytes()
    w, h, fmt, bpp = np.frombuffer(data, np.uint32, 4, 8)
    raw = np.frombuffer(data, np.uint8, offset=24)[: int(w) * int(h) * int(bpp)]
    return int(w), int(h), int(fmt), int(bpp), raw


def dump_to_image(path: Path) -> np.ndarray:
    w, h, fmt, bpp, raw = load_dump(path)
    if fmt in (126, 130) and bpp == 4:  # D32 depth: nonlinear view, 0 (far) black
        depth = np.clip(np.nan_to_num(raw.view(np.float32).reshape(h, w)), 0, 1) ** 0.25
        gray = (depth * 255).astype(np.uint8)
        return np.stack([gray] * 3, -1)
    if fmt == 83 and bpp == 4:  # R16G16F (velocity): magnitude-scaled red/green
        rg = np.nan_to_num(raw.view(np.float16).reshape(h, w, 2).astype(np.float32))
        scale = max(np.percentile(np.abs(rg), 99), 1e-6)
        rgb = np.zeros((h, w, 3), np.float32)
        rgb[:, :, :2] = np.clip(np.abs(rg) / scale, 0, 1)
        return (rgb * 255).astype(np.uint8)
    if bpp == 8:  # RGBA16F
        rgb = np.nan_to_num(raw.view(np.float16).reshape(h, w, 4)[:, :, :3].astype(np.float32))
        rgb = np.clip(rgb / max(np.percentile(rgb, 99), 1e-6), 0, 1) ** (1 / 2.2)
        return (rgb * 255).astype(np.uint8)
    channels = raw.reshape(h, w, bpp)
    rgb = channels[:, :, :3] if bpp >= 3 else np.repeat(channels[:, :, :1], 3, 2)
    return rgb.astype(np.uint8)


def cmd_dumps(dev: Device, args) -> None:
    out = Path(args.out)
    saved = {p: dev.getprop(f"debug.capy.{p}") for p in ("trace_from", "dump_from", "dump_files")}
    dev.run_as("rm -rf files/watch")
    try:
        dev.setprop("debug.capy.trace_from", "999999")  # dump without logging the frame
        for draw in args.draws:
            dev.setprop("debug.capy.dump_from", str(draw))
            dev.setprop("debug.capy.dump_files", str(args.count))
            dev.setprop("debug.capy.frame_trace", "0")
            time.sleep(1)
            dev.setprop("debug.capy.frame_trace", "1")
            time.sleep(args.delay)
    finally:
        dev.setprop("debug.capy.frame_trace", "0")
        for p, v in saved.items():
            dev.setprop(f"debug.capy.{p}", v or "0")
    archive = dev.run_as("tar cf - files/watch", binary=True)
    target = out / "dumps"
    target.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(target, filter="data")
    files = sorted((target / "files" / "watch").glob("f*.img"))
    images = [dump_to_image(f) for f in files]
    grid(images, [f.stem for f in files], out / "dumps.png", columns=2)


def cmd_quirks(dev: Device, args) -> None:
    for line in dev.logcat("CapyPS5-Quirks").splitlines():
        if "CapyPS5-Quirks" in line:
            print(re.sub(r"^.*?CapyPS5-Quirks: ", "", line))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default="282f344c")
    parser.add_argument("--out", default="capy_lab_out")
    sub = parser.add_subparsers(dest="command", required=True)

    def game(p):
        p.add_argument("--title", help=f"game folder under --roms ({DEFAULT_ROMS})")
        p.add_argument("--roms", default=DEFAULT_ROMS)
        p.add_argument("--dir", help="game directory on the device (instead of --title)")
        p.add_argument("--elf", help="executable (default: <dir>/eboot.bin)")
        p.add_argument("--quiet", type=int, default=60,
                       help="seconds without new pipeline compiles that count as settled")
        p.add_argument("--timeout", type=int, default=1800)
        p.add_argument("--warmup", type=int, default=60, help="extra seconds after settling")

    def shots(p):
        p.add_argument("--shots", type=int, default=3)
        p.add_argument("--interval", type=float, default=4.0)
        p.add_argument("--delay", type=float, default=6.0, help="seconds after a change")
        p.add_argument("--region", type=lambda s: tuple(int(v) for v in s.split(",")),
                       default=(0, 120, 1920, 1080), help="x0,y0,x1,y1 in 1920x1080 coordinates")

    p = sub.add_parser("launch"); game(p)
    p = sub.add_parser("props"); game(p); shots(p)
    p.add_argument("--set", action="append", required=True, help="name=v1,v2 (repeatable)")
    p.add_argument("--relaunch", action="store_true", help="relaunch the game for every variant")
    p = sub.add_parser("skip"); shots(p)
    p.add_argument("--kind", choices=["ps", "cs"], default="ps")
    p.add_argument("--target", default="", help="only pixel shaders of draws to this rt0")
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--top", type=int, default=8)
    p = sub.add_parser("drivers"); game(p); shots(p)
    p.add_argument("--other", required=True, help="installed driver name (as in the app)")
    p = sub.add_parser("trace")
    p.add_argument("--start", type=int, default=0, help="first draw to log")
    p.add_argument("--max-lines", type=int, default=200000)
    p.add_argument("--seconds", type=int, default=25)
    p = sub.add_parser("dumps")
    p.add_argument("--draws", type=int, nargs="+", required=True)
    p.add_argument("--count", type=int, default=1, help="consecutive draws per dump")
    p.add_argument("--delay", type=float, default=6.0)
    sub.add_parser("quirks")

    args = parser.parse_args()
    dev = Device(args.device)
    commands = {"launch": launch, "props": cmd_props, "skip": cmd_skip, "drivers": cmd_drivers,
                "trace": cmd_trace, "dumps": cmd_dumps, "quirks": cmd_quirks}
    commands[args.command](dev, args)


if __name__ == "__main__":
    main()
