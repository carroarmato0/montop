#!/usr/bin/env python3
"""A made-up system for Montop to watch, for screenshots and demos.

Writes a stand-in /proc and /sys under ROOT and keeps their counters moving
four times a second, following a fixed script: a browser streaming video
(decoded on the Radeon 780M, the CPU's own GPU),
music, a video export held to two cores, a paused game store, and a Rust
build in a terminal that takes every core for half a minute, about two
minutes in. Nothing in it comes from the machine it runs on.

    tools/demo/montop-demo.py ROOT &
    MONTOP_ROOT=ROOT XDG_DATA_HOME=ROOT/share XDG_DATA_DIRS=ROOT/share montop open

Standard library only.
"""

import math
import os
import random
import shutil
import sys
import time

TICK = 0.25          # Seconds between updates, Montop' default interval.
HZ = 100             # USER_HZ: ticks per second in /proc.
CORES = 16
PAGE = 4096
UID = os.getuid()    # Montop treats the user manager of its own uid as "yours".
MANAGER = f"/user.slice/user-{UID}.slice/user@{UID}.service"
BUILD_START, BUILD_END = 110.0, 142.0  # The build, in seconds from the start.

random.seed(7)
ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "montop-demo")
PROC, SYS = f"{ROOT}/proc", f"{ROOT}/sys"
GPU_SLOT = "0000:c4:00.0"
GPU = f"{SYS}/devices/pci0000:00/{GPU_SLOT}"


def write(path, text):
    """Replaces a file whole, so Montop never reads half of one."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.{os.getpid()}.tmp"
    with open(temporary, "w") as f:
        f.write(text)
    os.replace(temporary, path)


def wave(t, period, low, high, phase=0.0):
    return low + (high - low) * (0.5 + 0.5 * math.sin(2 * math.pi * t / period + phase))


def noisy(value, spread=0.15):
    return max(0.0, value * random.uniform(1 - spread, 1 + spread))


def building(t):
    return BUILD_START <= t < BUILD_END


# --- Who's running ------------------------------------------------------------

def app(name, suffix):
    return f"{MANAGER}/app.slice/app-Hyprland-{name}-{suffix}.scope"


CGROUPS = {
    "chromium": app("chromium", "4f1a90c2"),
    "foot": app("foot", "2b77e0d1"),
    "obsidian": app("obsidian", "9c03aa41"),
    "spotify": app("spotify", "51e2b7f8"),
    "steam": app("steam", "7da4c613"),
    "omacut": app("omacut", "c8e1f20b"),
    "signal": app("signal", "0b9d3e57"),
    "hyprland": f"{MANAGER}/session.slice/wayland-wm@hyprland.desktop.service",
    "pipewire": f"{MANAGER}/session.slice/pipewire.service",
    "wireplumber": f"{MANAGER}/session.slice/wireplumber.service",
    "portal": f"{MANAGER}/session.slice/xdg-desktop-portal.service",
    "manager": f"{MANAGER}/init.scope",
    "networkmanager": "/system.slice/NetworkManager.service",
    "journald": "/system.slice/systemd-journald.service",
    "bluetooth": "/system.slice/bluetooth.service",
    "upower": "/system.slice/upower.service",
}
LIMITS = {"omacut": 200}  # Percent of one core, as `montop limit` sets it.
FROZEN = {"steam"}


class Process:
    def __init__(self, pid, ppid, comm, command, group, user=UID, threads=1,
                 cpu=lambda t: 0.0, memory=50, read=lambda t: 0.0, written=lambda t: 0.0,
                 gpu=None, vram=0):
        self.pid, self.ppid, self.comm, self.command = pid, ppid, comm, command
        self.group, self.user, self.threads = group, user, threads
        self.cpu, self.memory, self.read_rate, self.write_rate = cpu, memory, read, written
        # Percent of the GPU, and MiB of GPU buffers; None for no GPU at all.
        self.gpu, self.vram = gpu, vram
        self.gpu_ns = 0.0
        self.now_gpu = 0.0
        self.ticks = 0.0
        self.read = self.written = 0.0
        self.now_cpu = 0.0
        self.now_read = self.now_written = 0.0
        self.kernel = ppid == 2


def page_load(t):
    """A burst now and then, as a tab loads a page."""
    for start in (24, 58, 83, 151, 172):
        if start <= t < start + 2.5:
            return 160 * math.exp(-(t - start))
    return 0.0


def renderer(pid, title, base):
    return Process(pid, 2210, "chromium", f"/usr/lib/chromium/chromium --type=renderer --lang=en-US "
                   f"--renderer-client-id={pid % 97} --shared-files  # {title}", "chromium", threads=19,
                   cpu=lambda t, b=base, p=pid: noisy(wave(t, 23 + p % 7, b * 0.4, b, p)), memory=180 + pid % 9 * 40)


PROCESSES = [
    # The compositor and the desktop's own services.
    Process(1063, 1, "Hyprland", "Hyprland --watchdog-fd 4", "hyprland", threads=14,
            cpu=lambda t: noisy(wave(t, 9, 1.5, 4.0)), memory=310,
            gpu=lambda t: noisy(wave(t, 9, 4, 9) + (3 if building(t) else 0)), vram=96),
    Process(1190, 1063, "quickshell", "quickshell -n -p /usr/share/omarchy/shell", "hyprland", threads=22,
            cpu=lambda t: noisy(1.2), memory=240, gpu=lambda t: noisy(1.2), vram=38),
    Process(1201, 1, "pipewire", "/usr/bin/pipewire", "pipewire", threads=3, cpu=lambda t: noisy(1.1), memory=18),
    Process(1202, 1, "wireplumber", "/usr/bin/wireplumber", "wireplumber", threads=6, cpu=lambda t: noisy(0.3), memory=22),
    Process(1240, 1, "xdg-desktop-por", "/usr/lib/xdg-desktop-portal", "portal", threads=5, cpu=lambda t: 0.05, memory=14),
    Process(1001, 1, "systemd", "/usr/lib/systemd/systemd --user", "manager", threads=1, cpu=lambda t: 0.05, memory=11),

    # The system's.
    Process(612, 1, "NetworkManager", "/usr/bin/NetworkManager --no-daemon", "networkmanager", user=0, threads=4,
            cpu=lambda t: noisy(0.3), memory=21),
    Process(388, 1, "systemd-journal", "/usr/lib/systemd/systemd-journald", "journald", user=0,
            cpu=lambda t: noisy(0.4 + (2.5 if building(t) else 0)), memory=28,
            written=lambda t: noisy(60e3)),
    Process(655, 1, "bluetoothd", "/usr/lib/bluetooth/bluetoothd", "bluetooth", user=0, cpu=lambda t: 0.02, memory=6),
    Process(702, 1, "upowerd", "/usr/lib/upowerd", "upower", user=0, threads=4, cpu=lambda t: 0.02, memory=8),

    # Apps.
    Process(2210, 1, "chromium", "/usr/lib/chromium/chromium --ozone-platform=wayland", "chromium", threads=48,
            cpu=lambda t: noisy(wave(t, 31, 4, 9) + page_load(t)), memory=420, read=lambda t: noisy(20e3)),
    Process(2241, 2210, "chromium", "/usr/lib/chromium/chromium --type=gpu-process --ozone-platform=wayland",
            "chromium", threads=12, cpu=lambda t: noisy(wave(t, 13, 8, 16)), memory=390,
            gpu=lambda t: noisy(wave(t, 17, 16, 27) + page_load(t) / 8), vram=210),
    renderer(2263, "YouTube", 22),
    renderer(2288, "GitHub", 4),
    renderer(2301, "Omarchy Manual", 1.5),
    renderer(2319, "Hacker News", 0.8),
    Process(2350, 2210, "chromium", "/usr/lib/chromium/chromium --type=utility --utility-sub-type=network.mojom.NetworkService",
            "chromium", threads=9, cpu=lambda t: noisy(1.6), memory=95),
    Process(2402, 1, "foot", "foot --server", "foot", threads=3,
            cpu=lambda t: noisy(0.8 + (3 if building(t) else 0)), memory=64),
    Process(2415, 2402, "bash", "/bin/bash", "foot", memory=6),
    Process(2416, 2402, "bash", "/bin/bash", "foot", memory=6),
    Process(2490, 2416, "nvim", "nvim src/sampler.rs", "foot", threads=4, cpu=lambda t: noisy(0.4), memory=88),
    Process(2733, 1, "obsidian", "/usr/lib/electron/electron /usr/lib/obsidian/app.asar", "obsidian", threads=31,
            cpu=lambda t: noisy(0.4), memory=610, gpu=lambda t: noisy(0.4), vram=24),
    Process(2810, 1, "spotify", "/opt/spotify/spotify", "spotify", threads=42,
            cpu=lambda t: noisy(3.2), memory=480, read=lambda t: noisy(8e3), gpu=lambda t: noisy(0.9), vram=31),
    Process(2902, 1, "steam", "/usr/lib/steam/steam", "steam", threads=56, memory=880,
            gpu=lambda t: 0.0, vram=118),
    Process(2961, 2902, "steamwebhelper", "/usr/lib/steam/steamwebhelper --type=renderer", "steam", threads=21, memory=310),
    Process(3104, 1, "omacut", "omacut trip-to-the-coast.mov", "omacut", threads=11, cpu=lambda t: noisy(1.5), memory=220,
            gpu=lambda t: noisy(2.4), vram=64),
    Process(3120, 3104, "ffmpeg", "ffmpeg -ss 00:01:12 -to 00:04:40 -i trip-to-the-coast.mov -c:v libx264 -preset slow "
            "-crf 18 -c:a copy trip-to-the-coast-cut.mp4", "omacut", threads=17,
            cpu=lambda t: noisy(560, 0.05), memory=740,
            read=lambda t: noisy(14e6), written=lambda t: noisy(6e6)),
    Process(3211, 1, "signal-desktop", "/usr/lib/signal-desktop/signal-desktop", "signal", threads=28,
            cpu=lambda t: noisy(0.3), memory=350, gpu=lambda t: noisy(0.2), vram=18),

    # A few kernel threads, hidden unless asked for.
    Process(2, 0, "kthreadd", "", "", user=0),
    Process(171, 2, "kworker/u33:1-events_unbound", "", "", user=0, cpu=lambda t: noisy(0.2)),
    Process(214, 2, "irq/171-nvme0q1", "", "", user=0, cpu=lambda t: 1.5 if building(t) else 0.05),
]

CRATES = ["serde", "tokio", "hyper", "regex", "syn", "rayon", "clap", "image", "zstd", "sampler",
          "ring", "rustls", "tracing", "futures", "chrono", "bytes", "mio", "quote", "proc_macro2", "libc"]


def build_processes(t, alive):
    """cargo, and a rustc per crate that comes and goes, as a real build does."""
    wanted = {}
    if building(t):
        wanted[2620] = Process(2620, 2416, "cargo", "cargo build --release", "foot", threads=16,
                               cpu=lambda t: noisy(6), memory=160)
        elapsed = t - BUILD_START
        remaining = BUILD_END - t
        # Compilers ramp up as cargo works out what it can build in
        # parallel, the count wobbles as crates wait on each other, and the
        # last seconds are one rustc linking the binary.
        if remaining < 4:
            busy = 1
        else:
            busy = min(14, int(elapsed * 4)) - int(3 * (0.5 + 0.5 * math.sin(elapsed * 1.3)))
            busy = max(1, busy - random.randint(0, 1))
        for slot in range(busy):
            generation = int((elapsed + slot * 0.7) / 3.2)
            pid = 5000 + slot * 40 + generation % 40
            crate = "sampler" if remaining < 4 else CRATES[(slot + generation * 3) % len(CRATES)]
            kind = "bin" if remaining < 4 else "lib"
            wanted[pid] = Process(pid, 2620, "rustc", f"rustc --crate-name {crate} --edition=2021 -C opt-level=3 "
                                  f"--crate-type {kind} src/{'main' if kind == 'bin' else 'lib'}.rs", "foot", threads=2,
                                  cpu=lambda t: noisy(97, 0.03), memory=random.randint(200, 900),
                                  read=lambda t: noisy(3e6), written=lambda t: noisy(9e6))
    for pid, process in wanted.items():
        if pid not in alive:
            alive[pid] = process
    for pid in [p for p in alive if p not in wanted]:
        del alive[pid]
        shutil.rmtree(f"{PROC}/{pid}", ignore_errors=True)


# --- The machine --------------------------------------------------------------

def setup():
    shutil.rmtree(ROOT, ignore_errors=True)
    write(f"{ROOT}/etc/passwd", f"root:x:0:0::/root:/bin/bash\nalex:x:{UID}:{UID}::/home/alex:/bin/bash\n")
    write(f"{PROC}/sys/kernel/hostname", "omarchy\n")
    write(f"{PROC}/sys/kernel/osrelease", "6.17.4-arch1-1\n")
    write(f"{PROC}/cpuinfo", "".join(f"processor\t: {i}\nmodel name\t: AMD Ryzen 9 7940HS w/ Radeon 780M Graphics\n\n"
                                     for i in range(CORES)))
    write(f"{PROC}/self/mounts", "/dev/nvme0n1p2 / ext4 rw,relatime 0 0\n/dev/nvme0n1p1 /boot vfat rw,relatime 0 0\n")
    write(f"{SYS}/block/nvme0n1/dev", "259:0\n")
    write(f"{SYS}/block/nvme0n1/size", "2000409264\n")
    write(f"{SYS}/block/nvme0n1/device/model", "Samsung SSD 990 PRO 1TB\n")
    write(f"{SYS}/block/nvme0n1/wwid", "eui.0025384b31a04c7e\n")
    # A USB backup drive, plugged in and idle: the one hidden in disks.png.
    # It has no WWID, so udev's by-id name (with its serial) stands for it.
    write(f"{SYS}/block/sda/dev", "8:0\n")
    write(f"{SYS}/block/sda/size", "3907029168\n")
    write(f"{SYS}/block/sda/device/model", "Elements 25A3\n")
    for target, links in (("sda", ["usb-WD_Elements_25A3_57584B314139383832374136-0:0"]),
                          ("nvme0n1p2", ["../by-uuid/6f1c2a9e-4d7b-4e0a-9f3c-2b8d1e5a7c40"]),
                          ("nvme0n1p1", ["../by-uuid/7A3E-1C52"])):
        for link in links:
            path = os.path.normpath(f"{ROOT}/dev/disk/by-id/{link}")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            os.symlink(f"../../{target}", path)
    write(f"{SYS}/class/net/wlan0/operstate", "up\n")
    write(f"{SYS}/class/hwmon/hwmon3/name", "k10temp\n")
    write(f"{SYS}/class/hwmon/hwmon3/temp1_label", "Tctl\n")
    # The Ryzen's own GPU, a Radeon 780M, with its card and render nodes.
    write(f"{GPU}/uevent", f"DRIVER=amdgpu\nPCI_SLOT_NAME={GPU_SLOT}\n")
    write(f"{GPU}/product_name", "AMD Radeon 780M\n")
    write(f"{GPU}/mem_info_vram_total", f"{2048 << 20}\n")  # The carve-out the firmware sets.
    for node in ("card1", "renderD128"):
        os.makedirs(f"{SYS}/class/drm/{node}", exist_ok=True)
        os.symlink(GPU, f"{SYS}/class/drm/{node}/device")
    names = {"chromium": "Chromium", "foot": "Foot", "obsidian": "Obsidian", "spotify": "Spotify",
             "steam": "Steam", "omacut": "Omacut", "signal": "Signal"}
    for app_id, name in names.items():
        write(f"{ROOT}/share/applications/{app_id}.desktop", f"[Desktop Entry]\nType=Application\nName={name}\n")
    write(f"{ROOT}/share/wayland-sessions/hyprland.desktop", "[Desktop Entry]\nName=Hyprland\n")
    for path in CGROUPS.values():
        os.makedirs(f"{SYS}/fs/cgroup{path}", exist_ok=True)


class Counters:
    def __init__(self):
        self.cores = [[0.0, 0.0] for _ in range(CORES)]  # busy, idle ticks
        self.disk_read = self.disk_written = self.disk_busy = 0.0
        self.rx = self.tx = 0.0
        self.load = 2.0
        self.cgroups = {key: {"usec": 0.0, "read": 0.0, "written": 0.0} for key in CGROUPS}


def tick(t, counters, alive):
    build_processes(t, alive)
    processes = PROCESSES + list(alive.values())

    # CPU: each process's share of one core, capped by its app's limit and
    # zero while its app is frozen.
    by_group = {}
    for p in processes:
        p.now_cpu = 0.0 if p.group in FROZEN else p.cpu(t)
        by_group.setdefault(p.group, []).append(p)
    for group, members in by_group.items():
        limit = LIMITS.get(group)
        total = sum(p.now_cpu for p in members)
        if limit and total > limit:
            for p in members:
                p.now_cpu *= limit / total * random.uniform(0.985, 0.999)
    busy = sum(p.now_cpu for p in processes) / 100 + 0.05

    # Spread the load over the cores, unevenly, as a scheduler would.
    weights = [1 + 0.6 * math.sin(i * 1.7 + t / 5) + random.uniform(0, 0.3) for i in range(CORES)]
    scale = busy / sum(weights)
    per_core = [min(0.99, w * scale) for w in weights]
    leftover = busy - sum(per_core)
    for i in range(CORES):
        if leftover <= 0:
            break
        extra = min(0.99 - per_core[i], leftover)
        per_core[i] += extra
        leftover -= extra
    span = HZ * TICK
    for i, share in enumerate(per_core):
        counters.cores[i][0] += share * span
        counters.cores[i][1] += (1 - share) * span
    totals = [sum(c[0] for c in counters.cores), sum(c[1] for c in counters.cores)]
    lines = [f"cpu  {int(totals[0] * 0.85)} 0 {int(totals[0] * 0.15)} {int(totals[1])} 0 0 0 0 0 0"]
    for i, (b, idle) in enumerate(counters.cores):
        lines.append(f"cpu{i} {int(b * 0.85)} 0 {int(b * 0.15)} {int(idle)} 0 0 0 0 0 0")
    write(f"{PROC}/stat", "\n".join(lines) + "\nprocs_running 3\n")

    counters.load += (busy - counters.load) * (1 - math.exp(-TICK / 60))
    write(f"{PROC}/loadavg", f"{counters.load:.2f} {counters.load * 0.8:.2f} {counters.load * 0.6:.2f} 3/912 {3500 + int(t)}\n")
    write(f"{PROC}/uptime", f"{11523 + t:.2f} {150000 + t * 10:.2f}\n")
    load = busy / CORES
    write(f"{SYS}/class/hwmon/hwmon3/temp1_input", f"{int((46 + 38 * load + random.uniform(-0.6, 0.6)) * 1000)}\n")
    for i, share in enumerate(per_core):
        write(f"{SYS}/devices/system/cpu/cpu{i}/cpufreq/scaling_cur_freq", f"{int((1400 + 3600 * share) * 1000)}\n")

    # Memory, disk, and network for every process and its app.
    held = 0
    for p in processes:
        p.now_read, p.now_written = p.read_rate(t), p.write_rate(t)
        if p.group in FROZEN:
            p.now_read = p.now_written = 0.0
        p.now_gpu = 0.0 if p.gpu is None or p.group in FROZEN else p.gpu(t)
        p.gpu_ns += p.now_gpu / 100 * TICK * 1e9
        p.ticks += p.now_cpu / 100 * span
        p.read += p.now_read * TICK
        p.written += p.now_written * TICK
        held += p.memory
        if p.group:
            c = counters.cgroups[p.group]
            c["usec"] += p.now_cpu / 100 * TICK * 1e6
            c["read"] += p.now_read * TICK
            c["written"] += p.now_written * TICK
        write_process(p)

    disk_read = sum(p.now_read for p in processes) + noisy(30e3)
    disk_written = sum(p.now_written for p in processes) + noisy(80e3)
    counters.disk_read += disk_read * TICK
    counters.disk_written += disk_written * TICK
    counters.disk_busy += min(1.0, (disk_read + disk_written) / 400e6 + 0.01) * TICK * 1000
    write(f"{PROC}/diskstats", f" 259 0 nvme0n1 812345 0 {int(counters.disk_read / 512)} 0 "
                               f"923456 0 {int(counters.disk_written / 512)} 0 0 {int(counters.disk_busy)} 0 0 0 0 0 0 0\n"
                               "   8 0 sda 1204 0 98304 0 310 0 2048 0 0 1840 0 0 0 0 0 0 0\n")

    streaming = 6e6 if 0.35 < (t / 9) % 1 < 0.55 else 0.4e6   # Video arrives in bursts.
    rx = noisy(streaming + 120e3) + (25e6 if 60 < t < 72 else 0)  # A download early on.
    tx = noisy(90e3) + (1.2e6 if building(t) else 0)
    counters.rx += rx * TICK
    counters.tx += tx * TICK
    write(f"{PROC}/net/dev", "Inter-|   Receive                                                |  Transmit\n"
                             " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets\n"
                             f" wlan0: {int(counters.rx)} 0 0 0 0 0 0 0 {int(counters.tx)} 0 0 0 0 0 0 0\n")

    # The GPU: busy as its apps add up to, its memory, clock and sensors.
    gpu_busy = min(100.0, sum(p.now_gpu for p in processes))
    vram = sum(p.vram for p in processes if p.gpu is not None) + 40
    write(f"{GPU}/gpu_busy_percent", f"{int(round(gpu_busy))}\n")
    write(f"{GPU}/mem_info_vram_used", f"{vram << 20}\n")
    write(f"{GPU}/hwmon/hwmon5/temp1_input", f"{int((44 + 0.35 * gpu_busy + 16 * load + random.uniform(-0.5, 0.5)) * 1000)}\n")
    write(f"{GPU}/hwmon/hwmon5/power1_input", f"{int((4.5 + 0.22 * gpu_busy + random.uniform(-0.3, 0.3)) * 1e6)}\n")
    write(f"{GPU}/hwmon/hwmon5/freq1_input", f"{int((800 + 19 * gpu_busy + random.uniform(-20, 20)) * 1e6)}\n")

    total_kb = 32 * 1024 * 1024
    used_kb = (held + 1900) * 1024
    write(f"{PROC}/meminfo", f"MemTotal: {total_kb} kB\nMemFree: {total_kb - used_kb - 9_400_000} kB\n"
                             f"MemAvailable: {total_kb - used_kb} kB\nBuffers: 210000 kB\nCached: 8900000 kB\n"
                             f"SReclaimable: 290000 kB\nSwapTotal: 8388604 kB\nSwapFree: 8388604 kB\n")

    for key, path in CGROUPS.items():
        members = by_group.get(key, [])
        c = counters.cgroups[key]
        base = f"{SYS}/fs/cgroup{path}"
        anon = sum(p.memory for p in members) * 1024 * 1024 * 0.92
        write(f"{base}/cpu.stat", f"usage_usec {int(c['usec'])}\nuser_usec {int(c['usec'] * 0.85)}\n"
                                  f"system_usec {int(c['usec'] * 0.15)}\n")
        write(f"{base}/memory.stat", f"anon {int(anon)}\nfile {int(anon * 0.3)}\nkernel {int(anon * 0.04)}\nshmem {int(anon * 0.04)}\n")
        write(f"{base}/io.stat", f"259:0 rbytes={int(c['read'])} wbytes={int(c['written'])} rios=0 wios=0 dbytes=0 dios=0\n")
        write(f"{base}/cgroup.events", f"populated 1\nfrozen {1 if key in FROZEN else 0}\n")
        limit = LIMITS.get(key)
        write(f"{base}/cpu.max", f"{limit * 1000} 100000\n" if limit else "max 100000\n")


def write_process(p):
    base = f"{PROC}/{p.pid}"
    state = "R" if p.now_cpu > 50 else "S"
    if p.group in FROZEN:
        state = "S"
    utime, stime = int(p.ticks * 0.85), int(p.ticks * 0.15)
    rss = p.memory * 1024 * 1024 // PAGE
    write(f"{base}/stat", f"{p.pid} ({p.comm[:15]}) {state} {p.ppid} {p.pid} {p.pid} 0 -1 4194560 0 0 0 0 "
                          f"{utime} {stime} 0 0 20 0 {p.threads} 0 100 {rss * PAGE * 2} {rss} 0\n")
    if not os.path.exists(f"{base}/cgroup"):
        write(f"{base}/cmdline", p.command.replace(" ", "\0") + "\0" if p.command else "")
        write(f"{base}/cgroup", f"0::{CGROUPS[p.group] if p.group else '/'}\n")
        write(f"{base}/status", f"Name:\t{p.comm[:15]}\nUid:\t{p.user}\t{p.user}\t{p.user}\t{p.user}\n")
    if not p.kernel:
        write(f"{base}/io", f"rchar: 0\nwchar: 0\nsyscr: 0\nsyscw: 0\nread_bytes: {int(p.read)}\n"
                            f"write_bytes: {int(p.written)}\ncancelled_write_bytes: 0\n")
    if p.gpu is not None:
        # An open render node, as the kernel describes it to its owner.
        if not os.path.islink(f"{base}/fd/12"):
            os.makedirs(f"{base}/fd", exist_ok=True)
            os.symlink("/dev/dri/renderD128", f"{base}/fd/12")
        write(f"{base}/fdinfo/12", f"pos:\t0\nflags:\t02100002\ndrm-driver:\tamdgpu\ndrm-client-id:\t{p.pid}\n"
                                   f"drm-pdev:\t{GPU_SLOT}\ndrm-memory-vram:\t{p.vram * 1024} KiB\n"
                                   f"drm-memory-gtt: \t{p.vram * 256} KiB\ndrm-memory-cpu: \t0 KiB\n"
                                   f"drm-engine-gfx:\t{int(p.gpu_ns)} ns\n")


def main():
    setup()
    counters, alive = Counters(), {}
    start = time.monotonic()
    # MONTOP_DEMO_SKIP=90 starts the script 90 seconds in, to get to the
    # build sooner.
    head_start = float(os.environ.get("MONTOP_DEMO_SKIP", "0"))
    t = 0.0
    while t < head_start:
        tick(t, counters, alive)
        t += TICK
    print(f"montop-demo: writing {ROOT}", flush=True)
    while True:
        tick(t, counters, alive)
        t += TICK
        time.sleep(max(0.0, start + (t - head_start) - time.monotonic()))


if __name__ == "__main__":
    main()
