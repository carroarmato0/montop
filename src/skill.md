---
name: montop
description: |
  See what a Linux machine is doing with Montop: CPU, GPU, memory, disks,
  network, and which apps and processes are using them. Use for ANY question
  about why the computer is slow, hot, loud, out of memory, or out of disk
  space, what is using the CPU, GPU, memory, disk, or network, or whether
  something is running.
---

# Montop

`montop` reads the kernel's counters and groups processes into the apps and
services they belong to, using systemd's cgroups: a browser's forty helpers are
one app, and a terminal's shells and builds belong to that terminal. No command
needs a display.

## Workflow

1. Start with `montop snapshot --json`. It samples twice, a second apart, and
   prints CPU (total, per core, frequency, temperature, load), GPUs, memory,
   disks, filesystems, network, and the ten busiest apps and processes.
   - `--sort memory|read|write|gpu` ranks by something else than CPU.
   - `--top N` lists more (0 lists all); `--interval MS` measures rates over a
     longer window, which helps when the load comes and goes.
2. For the full picture of what is running, `montop apps --json` lists every
   app and service with its totals, its process IDs, and its top processes.
3. Answer with the app first ("Brave is using 3 cores, mostly one tab's
   renderer"), then the process and command line that explains it.

## Measuring a command

`montop run --json <command>` runs it (as the person would, same directory
and environment) and prints a JSON report on stderr when it ends: wall time,
CPU seconds and cores used, peak memory, disk bytes, seconds spent waiting on
CPU, memory, and disk, and the most processes at once. It counts every process
the command started, even ones that already exited, and exits with the
command's own code. Use it to answer "how much memory does this build need",
"is this test suite CPU- or disk-bound", or to compare two flags.

- High `waited_seconds.cpu` with `cpu.average_percent` near the core count:
  CPU-bound; more parallelism won't help.
- High `waited_seconds.disk`: disk-bound. High `waited_seconds.memory`: the
  machine was short on memory and reclaiming.

## Reading the numbers

- `cpu_percent` is relative to one core: 200 means two cores fully busy. Compare
  against `cpu.cores` before calling something a problem.
- App totals come from the app's cgroups, so they include child processes that
  have already exited (a build's compilers) and count shared memory once. They
  can therefore differ from the sum of the listed processes.
- App memory leaves out the page cache, which the kernel hands back on demand;
  `memory.available_bytes` is what is really free.
- A `null` rate means the kernel did not let this user read it (another user's
  process). Say so rather than reading it as zero.
- `gpus` lists every GPU with what its driver reports: `busy_percent`, its
  own memory (`memory_used_bytes` of `memory_total_bytes`) or, for a GPU that
  shares RAM, what apps' GPU buffers hold (`app_memory_bytes`), and clock,
  temperature, power, and fan where known. `asleep: true` is a discrete GPU
  powered down because nothing uses it: that is normal, not a fault. A `note`
  says why numbers are missing (Apple's GPU on Asahi Linux reports none on
  kernels without `agx_stats`).
- `gpu_percent` on apps and processes is of one GPU, from each driver's
  per-client counters (NVIDIA's through NVML); it's there when `per_app` is
  true for a GPU. Video playback and encoding show up here too.
- `kind` tells apps the person launched (`app`) from their own background
  services (`service`), system services (`system`), and the kernel.

## Pausing and limiting

- `montop pause <app>` freezes every process of an app until
  `montop resume <app>`; `montop limit <app> <percent|off>` caps its CPU in
  percent of one core until logout. `<app>` is a name or `key` from
  `montop apps`, or a PID in the app.
- Both are gentler than killing: nothing is lost, and both are undone with one
  command. Still, they change the person's machine: only run them when asked,
  or after proposing it and getting a yes. Always tell them how to undo it.
- `controllable: false` in `montop apps --json` means Montop refuses: desktop
  services (pausing the compositor would freeze the screen), system services,
  or the app the command runs inside.

## Working with the person

- Never end or kill processes on your own. Name what you would stop and why, and
  let them decide; `montop open` shows the monitor, where `x` ends the selected
  app or process after asking. Suggest pausing or limiting when they want the
  machine back but not to lose the app's work.
- Every command takes `--json`, exits 0 on success and 1 on failure, and writes
  errors to stderr. `montop help <command>` lists a command's options.
