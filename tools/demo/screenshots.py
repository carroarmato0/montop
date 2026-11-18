#!/usr/bin/env python3
"""Takes the README screenshots on a Hyprland desktop, from the demo system.

Opens two fullscreen Montop windows on empty workspaces (Tokyo Night and
Catppuccin Latte), both watching montop-demo.py, waits until the graphs are
full and the demo's build spike is a minute in the past, then drives them
from the keyboard (wtype) and the pointer (hyprctl) and saves each shot with
grim. Hands off the mouse while it runs: about four minutes.

    tools/demo/screenshots.py [OUTPUT_DIR]     # default: docs/screenshots

Needs hyprctl, wtype, and grim, and a built build/montop. Run it under
`systemd-inhibit --what=idle` so the screen doesn't lock halfway.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MONTOP = os.path.join(REPO, "build", "montop")
OUT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "docs", "screenshots"))
DARK, LIGHT = 8, 9  # Workspaces; empty ones are best.
SETTLE = 190        # Seconds of history before the first shot.
THEMES = "/usr/share/omarchy/themes"


def hyprctl(*args):
    return subprocess.run(["hyprctl", *args], capture_output=True, text=True, check=True).stdout


def clients():
    return json.loads(hyprctl("-j", "clients"))


def keys(*sequence):
    """wtype: plain text, or ("key", name) for a named key."""
    for item in sequence:
        if isinstance(item, tuple):
            subprocess.run(["wtype", "-k", item[1]], check=True)
        else:
            subprocess.run(["wtype", item], check=True)
        time.sleep(0.35)


def shoot(name):
    time.sleep(1.2)  # Let the graphs slide and tooltips settle.
    path = os.path.join(OUT, name)
    subprocess.run(["grim", "-o", monitor(), path], check=True)
    print("saved", path, flush=True)


def monitor():
    return next(m["name"] for m in json.loads(hyprctl("-j", "monitors")) if m["focused"])


# Hyprland 0.56 and later take Lua for dispatchers.
def focus_workspace(workspace):
    hyprctl("dispatch", f'hl.dsp.focus({{ workspace = "{workspace}" }})')


def pointer(x, y):
    hyprctl("dispatch", f"hl.dsp.cursor.move({{ x = {x}, y = {y} }})")


def launch(command, workspace):
    hyprctl("eval", f'hl.exec_cmd([[{command}]], {{ workspace = "{workspace} silent", fullscreen = true }})')


def show(workspace):
    focus_workspace(workspace)
    time.sleep(1.0)


def accent_line_x(shot, color, top, bottom, left, right):
    """The x of the rewind cursor: a one-pixel column of the accent color."""
    with open(shot, "rb") as f:
        data = f.read()
    # Binary PPM: "P6\nW H\n255\n" then RGB.
    header, rest = data.split(b"\n", 3)[:3], data.split(b"\n", 3)[3]
    width = int(header[1].split()[0])
    target = bytes(int(color[i:i + 2], 16) for i in (1, 3, 5))
    best, best_count = None, 0
    for x in range(left, right):
        count = sum(1 for y in range(top, bottom, 4)
                    if rest[(y * width + x) * 3:(y * width + x) * 3 + 3] == target)
        if count > best_count:
            best, best_count = x, count
    return best


def main():
    for tool in ("hyprctl", "wtype", "grim"):
        if not shutil.which(tool):
            sys.exit(f"screenshots.py needs {tool}")
    if not os.path.exists(MONTOP):
        sys.exit("Build Montop first: bin/build")
    os.makedirs(OUT, exist_ok=True)

    scratch = tempfile.mkdtemp(prefix="montop-shots-")
    root = os.path.join(scratch, "root")
    # A theme directory per window, shaped like Omarchy's current/ state.
    themes = {}
    for name in ("tokyo-night", "catppuccin-latte"):
        state = os.path.join(scratch, name)
        os.makedirs(state)
        os.symlink(os.path.join(THEMES, name), os.path.join(state, "theme"))
        themes[name] = state

    # A config home of its own, so your saved settings don't change the shots.
    environment = (f"MONTOP_ROOT={root} XDG_DATA_HOME={root}/share XDG_DATA_DIRS={root}/share "
                   f"XDG_CONFIG_HOME={scratch}/config")
    before = {c["pid"] for c in clients()}
    home = json.loads(hyprctl("-j", "activeworkspace"))["id"]
    generator = subprocess.Popen([sys.executable, os.path.join(HERE, "montop-demo.py"), root],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(2)
        started = time.monotonic()
        for workspace, theme in ((DARK, "tokyo-night"), (LIGHT, "catppuccin-latte")):
            launch(f"env {environment} MONTOP_THEME={themes[theme]} {MONTOP} open", workspace)
        time.sleep(5)
        windows = [c for c in clients() if c["pid"] not in before and c["class"] == "montop"]
        if len(windows) != 2:
            sys.exit(f"Expected two Montop windows, found {len(windows)}")
        print(f"Waiting {SETTLE} s for the graphs to fill...", flush=True)
        time.sleep(max(0, SETTLE - (time.monotonic() - started)))

        # 1. The overview: apps by CPU, the browser opened to its tabs.
        show(DARK)
        pointer(1900, 1060)
        keys(("k", "Down"), ("k", "Down"), "l")
        shoot("overview.png")

        # 2. Rewind to the build: who did it, and what it ran.
        keys("h", "[", "[", "[", "[", "[", "[")
        keys(("k", "Home"), "l")
        probe = os.path.join(scratch, "probe.ppm")
        subprocess.run(["grim", "-t", "ppm", "-o", monitor(), probe], check=True)
        x = accent_line_x(probe, "#7aa2f7", 80, 250, 30, 1500)
        if x:
            # Two steps, so the window sees the pointer move and shows its tooltip.
            pointer(x - 6, 190)
            time.sleep(0.3)
            pointer(x, 200)
        shoot("rewind.png")
        pointer(1900, 1060)
        keys(("k", "Escape"), ("k", "Home"))

        # 3. Limiting an app's CPU, next to a paused one.
        keys("c")
        shoot("limit.png")
        keys(("k", "Escape"))

        # 4. Choosing the disks to show: the USB backup drive hidden.
        keys("d", "j", ("k", "space"))
        shoot("disks.png")
        keys(("k", "space"), ("k", "Escape"))

        # 5. The same, following a light theme.
        show(LIGHT)
        pointer(1900, 1060)
        keys(("k", "Down"), ("k", "Down"), "l")
        shoot("light.png")
    finally:
        for c in clients():
            if c["pid"] not in before and c["class"] == "montop":
                os.kill(c["pid"], 15)
        generator.terminate()
        focus_workspace(home)
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
