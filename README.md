# Touhou 8: Imperishable Night — Nintendo Switch Port

_(東方永夜抄　～ Imperishable Night)_

![Platform](https://img.shields.io/badge/Platform-Nintendo%20Switch-e60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)![Status](https://img.shields.io/badge/Status-Playable-green?style=for-the-badge)![License](https://img.shields.io/badge/License-MIT-blue?style=for-the-badge)

A native homebrew port of ZUN's 2004 bullet hell danmaku classic **Touhou 8: Imperishable Night** for the **Nintendo Switch** (Horizon OS).

Built on the [N0zoM1z0/th08](https://github.com/N0zoM1z0/th08) decompilation (`port/portable-64bit` branch), this port runs the game through SDL2 on an OpenGL ES 3 context over Nouveau/Mesa and paces itself to a locked 60 FPS on Horizon — no Linux, Box64 or Wine involved.

Companion to the [Touhou 6](https://github.com/Swiizyu/th06-switch) and [Touhou 7](https://github.com/Swiizyu/th07-switch) Switch ports.

* * *

## ✨ Key Features

- 🚀 **Locked 60 FPS:** Horizon's EGL implementation does not block on swap, so the renderer would otherwise free-run while the logic ticked at 60. The port paces presentation against an absolute deadline, giving stable frames and noticeably less battery drain.
- 🌙 **OLED-Friendly Pillarboxing:** The original 640×480 playfield is centred inside the Switch's 1280×720 display with pure black (`#000000`) bars.
- 🔊 **Native-Rate Audio:** Sound is delivered through an SDL2 audio callback at the game's own 44.1 kHz 16-bit stereo rate — no resampling anywhere in the path. BGM plays straight out of `thbgm.dat`.
- 🎮 **Fixed, Sane Controls:** Joy-Con (handheld, grip, detached) and Pro Controller via SDL2's gamepad API.
- 🌏 **Language-Aware Title:** hbmenu shows the original Japanese title on consoles set to 日本語 and the romanised one everywhere else, filled across all 16 NACP language slots.
- 📁 **Flexible Data Location:** The game data can sit in `sd:/switch/th08/`, `sd:/th08/`, `sd:/games/th08/`, `sd:/roms/th08/` or `sd:/switch/touhou8/`, plus `touhou 8`, `imperishable night` and `in` variants.
- 💾 **Saves Next to the Data:** `th08.cfg`, `score.dat`, replays and snapshots are written into the same SD folder the game loaded from.

* * *

## 📥 Installation Guide

> ⚠️ **Disclaimer:** In compliance with ZUN's guidelines and copyright law, this repository contains **ONLY the homebrew engine code**. No game assets are distributed. You must legally own a copy of _Touhou 8: Imperishable Night v1.00d_.

### 1. SD Card File Structure

1. Ensure your Nintendo Switch is running custom firmware (Atmosphère CFW).
2. Download the latest `touhou08.nro` from the [Releases](https://github.com/Swiizyu/th08-switch/releases) tab (or build from source).
3. Create a folder named `sd:/switch/th08/` and copy the following into it:

```
sd:/switch/th08/
    ├── touhou08.nro          # Nintendo Switch homebrew executable
    ├── th08.dat              # Main game archive
    ├── thbgm.dat             # Background music archive
    └── msgothic.ttc          # Japanese font (ships with the Windows release)
```

The loader also accepts `sd:/th08/`, `sd:/touhou8/`, `sd:/switch/touhou8/`, `sd:/games/th08/`, `sd:/roms/th08/` in any capitalisation — or simply the folder the NRO was launched from.

### 2. Music

No extra soundtrack download is needed: Imperishable Night ships its BGM inside `thbgm.dat`, and the port plays it directly. MIDI mode is unavailable (Horizon has no system synthesizer) and is not needed.

### 3. Launching

Run `touhou08.nro` from the **Homebrew Menu (hbmenu)**, **Sphaira launcher**, or a home screen forwarder.

* * *

## 🕹 Controls

| Nintendo Switch Button | Action |
| :-- | :-- |
| **Left Stick / D-Pad** | Character Movement |
| **A** | Shoot / Confirm |
| **B** | Bomb / Cancel |
| **L** | Focus (Precision Slow-Motion Movement) |
| **R** | Skip Dialogue (hold) |
| **+ (Plus)** | Pause / In-Game Menu |

Every other button is intentionally inert. The layout is fixed in code rather than read from `th08.cfg`, so it behaves identically on every controller.

* * *

## ⚠️ Known Issues

while the game is **playable**. A few issues are known - all of them are inherited from the upstream decompilation rather than the Switch layer (they reproduce identically in the upstream web build of the same source):

- The **pre-boss dialogue** plays over a solid black background instead of the stage behind it.
- **Background flickering** on one of the stages.
- **Items past the point-of-collection line are auto-attracted even below full power** — in the original, auto-collection only triggers at max power.

Huge thanks to the decompilation's author for the remarkable reconstruction work this port stands on — as soon as these are addressed upstream, this port picks the fixes up with a plain rebuild.

* * *

## 🛠 Building from Source

### Automated Build (GitHub Actions)

This repository includes a CI pipeline (`.github/workflows/build-switch.yml`). Push or fork the repository and the workflow compiles `th08.nro` inside the official `devkitpro/devkita64` container, uploading it as a downloadable artifact.

### Local Build (Linux / macOS / WSL)

1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `devkitA64` and `libnx`.
2. Install the required Switch portlibs:

```
sudo dkp-pacman -Syu
sudo dkp-pacman -S switch-dev switch-mesa switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-freetype switch-harfbuzz switch-libpng switch-libjpeg-turbo switch-libwebp switch-libavif switch-bzip2
```

3. Build:

```
export DEVKITPRO=/opt/devkitpro
./scripts/build_switch.sh          # configures (CMake + Ninja) and builds
```

The result is `build-switch/th08.nro`.

If the devkitPro pacman mirrors are unreachable, `scripts/pull_dkp.sh` pulls the exact same toolchain (devkitA64, libnx and the Switch portlibs) out of the official `devkitpro/devkita64` Docker image into `/opt/devkitpro` instead.

* * *

## 📂 What This Fork Changes

Everything Switch-specific is confined to `src/modern/switch/` or guarded by `#ifdef __SWITCH__`, keeping rebases onto upstream cheap:

| File | Purpose |
| :-- | :-- |
| `src/modern/switch/switch_compat.cpp` | platform layer: SD data folder discovery, files/streams/events, gamepad, SDL audio, GDI bitmap shims, CP932 |
| `src/modern/switch/gles_ffp.{hpp,cpp}` | fixed-function pipeline → OpenGL ES 3 shim |
| `src/modern/switch/switch_runtime.cpp` | entry point, startup hooks, crash reporter (`modern-crash.txt`, written into the data folder) |
| `src/modern/linux/d3d8_compat.cpp` | FBO-backed D3D8 device, fog, 4:3 letterboxing + 60 FPS pacer, GPU `CopyRects` blits, texture upload paths |
| `src/modern/switch/cp932_table.h` | generated CP932→Unicode table for the Shift-JIS text renderer |
| `scripts/build_switch.sh` | one-shot configure + build |
| `scripts/pull_dkp.sh` | devkitPro toolchain from the Docker image (mirror fallback) |
| `scripts/nacp_lang.py` | per-language NACP titles |
| `CMakeLists.txt` | `NINTENDO_SWITCH` platform branch |

A few defensive guards were also added around the game's own data access: some of the original game data references sprite/script/effect indices past the end of their arrays. On Windows those out-of-bounds reads silently land in adjacent memory, but on Horizon they fault — so the port bounds-checks them and skips the invalid entry, matching the observable behaviour of the original.

* * *

## 🤝 Credits & Acknowledgments

- **ZUN / Team Shanghai Alice** — original creator and developer of the Touhou Project series.
- **[N0zoM1z0](https://github.com/N0zoM1z0/th08)** (and KSS) — the Touhou 8 decompilation and its cross-platform `port` branches.
- **Switchbrew & devkitPro Team** — the open-source `libnx` SDK and Switch toolchain.

## License

MIT — same as the upstream decompilation. See [LICENSE](LICENSE).
