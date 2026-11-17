---
name: montop-dev
description: Develop, test, package and release Montop, the Omarchy system monitor in this repository, and keep it compliant with Omarchy's package repository (omacom/omarchy-pkgs). Use for any change to Montop' code, QML, translations, packaging, screenshots, README, version, or release. Triggers: montop, package-check, omarchy-pkgs, release, bump the version, PKGBUILD, screenshots.
---

# Developing Montop

Montop follows Omarchy's own apps (Omacut, Hype, Monologue): C++ and Qt Quick,
qmake6, one binary, theme and language followed live. The global
`omarchy-app` skill holds the general conventions; this one holds what is
specific to Montop.

## Layout

- `src/` flat: `monitor.*` (sampling, rewind, pause/limit), `gpu.*` (GPU
  devices, fdinfo parsing, NVML), `processmodel.*`
  (rows and roles for QML), `procfs.*` (parsers), `apps.*` (cgroups and
  desktop names), `run.*` (`montop run`), `cli.*` (commands), `theme.*`,
  `translator.*`, QML panels, `i18n/{en,nl}.json`.
- `tests/montop_tests.cpp`: Qt Test, compiles `../src/*.cpp` directly. A new
  source file goes in `montop.pro` and `tests/montop_tests.pro`.
- `pkgbuild/PKGBUILD`: builds this checkout for `bin/install`.
- `pkgbuild/omarchy-pkgs/`: the recipe for omacom/omarchy-pkgs, built from
  the GitHub release tarball. It is the source of truth for the PR there.
- `tools/demo/`: the made-up demo system and the screenshot driver.

## Loop

1. `bin/build`, then try `build/montop open` or a CLI command.
2. `bin/test` after every backend change. All tests must pass.
   `bin/lint` after a QML change: qmllint against the generated `Montop`
   module must report nothing. QML reaches C++ only through that module
   (`QML_ELEMENT`), never through context properties, and delegates are
   `Bound`. `Monitor`, `Theme` and `Translator` are singletons handing out
   main()'s objects through the `QML_FOREIGN` stand-ins in `qmlsingletons.h`:
   `QML_SINGLETON` on a default-constructible class makes Qt build a fresh,
   idle copy and ignore its `create()`. Lint can't see that; look at the
   window.
3. Every visible string goes through `qsTr`/`tr` and gets a Dutch entry in
   `src/i18n/nl.json`; `shippedCatalogsCoverEveryString` fails otherwise.
4. `bin/package-check` before committing a packaging change, and before a
   release: it builds this checkout in omarchy-pkgs' own builder container
   (rootless Podman), runs `check()` (the test suite) there, and checks the
   result the way their CI does. New files must be `git add`ed to be included.
5. `bin/install` (needs a real terminal for sudo) to use the change.

## Rules

- Never change Omarchy or Hyprland config. Never run `montop skill install`
  or `remove` on the user's machine without approval: they change `~/.agents`
  and the agents' skills folders. Test them with a temporary home, as
  `linksAndRemovesTheSkill` does.
- The agent skill is `src/skill.md`: bundled in the binary (`montop skill`
  prints it) and installed by the package as
  `/usr/share/montop/skills/montop/SKILL.md`. A package never writes into home
  folders; `montop skill install` links the packaged copy per user, into the
  same folders Omarchy links its own skills into, and leaves anything that
  isn't Montop' alone.
- Pause and limit act only on `app.slice` and `background.slice` units, never
  on `session.slice`, system services, or the unit Montop runs in. Keep the
  undo-on-close behaviour of the window.
- The window's settings (panels, sort, grouping, kernel threads, interval)
  are kept with QtCore's `Settings` in `~/.config/montop/settings.conf`
  (`Main.qml`); momentary state (pause, filter, rewind) isn't. A new setting
  goes there too.
- Screenshots and README examples come from the demo system, never from the
  user's machine: `tools/demo/screenshots.py` under
  `systemd-inhibit --what=idle`, hands off the mouse for four minutes.
- Commit messages are short and imperative.

## GPUs

- Devices are `/sys/class/drm/card*` that have a render node; per-driver
  readers live in `Gpus::sampleDevice` (`src/gpu.cpp`). Per-app usage comes
  from `/proc/<pid>/fdinfo` (`drm-engine-*` ns, or `drm-cycles-*` over
  `drm-total-cycles-*` for xe), de-duplicated by device and `drm-client-id`,
  in `Monitor::sampleGpuClients`. NVIDIA's proprietary driver reports only
  through NVML, loaded with `dlopen` behind the `gpu::Nvml` interface.
- Never wake a sleeping GPU: when `power/runtime_status` is `suspended`, read
  nothing else for it, NVML included.
- Keep the scan cheap: fd lists are cached per PID and re-listed adaptively
  (new processes, then every 15th scan for GPU users, 30th for the rest).
  Measure with a harness before and after changing it; it costs about 0.5 ms
  per scan on a desktop with 330 processes.
- Hardware without a test machine is covered by sysfs/fdinfo fixtures in
  `tests/montop_tests.cpp` (`readsGpuDevices`, `monitorPutsGpuUsageDownToApps`,
  `namesArmMachines`, `readsAppleGpuStats`). Apple Silicon (Asahi) reports GPU
  busy time and power in `device/agx_stats` on newer kernels (no per-app usage),
  and no CPU temperature; the window says so rather than showing zeros.

## Version and release

The version is set in `montop.pro` (`VERSION = x.y.z`) and as `pkgver` in both
PKGBUILDs; `bin/package-check` refuses a mismatch.

1. Set the version in the three places; reset `pkgrel=1`. Update README if
   behaviour changed. `bin/test`, `bin/package-check`. Commit.
2. Push, then `git tag -a vX.Y.Z -m "Montop X.Y.Z"` and push the tag. The
   Release workflow checks the tag against `montop.pro`, builds and tests it
   in omarchy-pkgs' builder, and publishes the release with generated notes
   and the x86_64 package attached. Rewrite the notes in plain words with
   `gh release edit vX.Y.Z --notes-file ...`.
3. In `pkgbuild/omarchy-pkgs/`, run `updpkgsums` (delete the downloaded
   tarball afterwards), then `bin/package-check --release` to build exactly
   what will ship. Commit the new checksum.
4. Once Montop is in omarchy-pkgs, its `upstream.watch` picks up the GitHub
   release on its own and a bot opens the update PR; nothing else is needed.

## GitHub

- Workflows: Test (build and tests in `archlinux:latest`, on every push and
  PR), Package (`bin/package-check` on ubuntu-latest's rootless Podman, when
  packaging or the build changes), Release (on `v*` tags). Dependabot keeps
  the pinned actions current.
- Issue forms in `.github/ISSUE_TEMPLATE` (bug, idea; no blank issues),
  `pull_request_template.md`, and `SECURITY.md` (private advisories).
- The default branch is `main`.

## omarchy-pkgs compliance

What their README and CI require, and what the recipe already does:

- One package per PR, branched from `master`, files only under
  `pkgbuilds/montop/`: `PKGBUILD`, `montop.install`, `.omarchy/package.json`.
  The PR says what the package is and how it was tested.
- `arch=('x86_64' 'aarch64')`; source pinned to a release tag with its
  sha256; every file owned by root (`install -D`, no vendor tarballs).
- `.omarchy/package.json`: `"source": "local"`, `"auto_merge": true`, and an
  `upstream.watch` on `carroarmato0/montop` with pattern
  `v(?P<version>[0-9]+(?:\.[0-9]+)*)`. Check it with
  `python build/omarchy-pkgs/helpers/upstream-watch.py check build/omarchy-pkgs/pkgbuilds/montop`.
- Not on the fast ring: it is compiled here and links Qt, which their
  `bin/check-fast-ring` refuses. It reaches rc and stable by promotion.
- A packaging change at the same version bumps `pkgrel`, or it never ships.
  Never set `epoch`.
- A non-draft PR that only touches `pkgbuilds/` merges itself once green, so
  open it as a draft unless the user says otherwise. Outside contributors'
  builds wait for a maintainer's `build-approved` label.
- Opening a PR, pushing, tagging, and releasing are outward-facing: confirm
  with the user first.
