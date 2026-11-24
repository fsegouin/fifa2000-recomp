# FIFA 2000 native port

A static recompilation of **FIFA 2000** for the PlayStation (Europe, SLES-02315)
into a native program, built on a fork of
[psxrecomp](https://github.com/fsegouin/psxrecomp) with
[recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui). It builds and
runs natively on Apple Silicon Macs, and the framework also supports Windows
and Linux.

| | |
|---|---|
| Disc | FIFA 2000 (Europe) (En,De,Es,Nl,Sv) (Rev 1), single data track |
| Players | 2 |
| Publisher | EA Sports, 1999 |

## What it adds

- **Native code.** The main program is recompiled ahead of time; the match
  and front-end overlays are compiled to native code as they load.
- **Widescreen** (the *Widescreen* mod, on by default, 16:9 or Fit to Window).
  Matches render a genuinely wider view: the pitch, lines, players, stands and
  sky fill the frame. Menus, loading screens and videos stay 4:3.
- **Higher resolution and filtering.** Rendering at the display's resolution,
  PGXP vertex precision and perspective-correct textures, bilinear filtering,
  FXAA and mipmaps, all adjustable in the launcher's Display and PGXP pages.

## Legal

You need your own copy of the game. Disc images are never committed (`disc/`,
`*.bin` and `*.cue` are ignored). `generated/` holds the game code translated
from your disc, so it is not published either: every checkout generates it
locally. The build runs on the bundled open-source OpenBIOS; no console BIOS
is needed.

## Building

Requirements: CMake, Ninja, pkg-config, Python 3 and a C/C++ compiler (on
macOS: the Xcode command-line tools and `brew install cmake ninja pkg-config`).

```sh
git clone --recurse-submodules https://github.com/fsegouin/fifa2000-recomp.git
cd fifa2000-recomp
./psxrecomp/tools/ci/build_emitters.sh
python3 psxrecomp/psxrecomp_cli.py generate --config game.toml --project-root . --disc /path/to/your/game.cue
python3 psxrecomp/psxrecomp_cli.py rebuild --config game.toml --project-root . --build-dir build-release --no-pgo
```

`rebuild` also stages the overlay toolchain beside the program so streamed
overlays become native code. Run `build-release/FIFA2000` and pick your disc
in the launcher, or skip the launcher with
`build-release/FIFA2000 --no-launcher --disc /path/to/your/game.cue`.

## How the widescreen works

FIFA 2000 culls and clips everything to its 4:3 screen in several places. Each
change below is inert at 4:3, where the original code runs unchanged.

| What | Where |
|---|---|
| Pitch, crowd and model rejects in the match engine | `game.toml`, `[widescreen.cull]` |
| Pitch-line clipping, reimplemented with wider bounds | `src/mods/fifa2000_pitch_lines.c` |
| The camera view cone that decides which players are drawn | `src/mods/fifa2000_player_cull.c` |
| The sky backdrop, and detecting when the 3D match is on screen | `src/mods/fifa2000_sky.c` |
| The scoreboard, anchored to the wide frame's top-left corner | `src/mods/fifa2000_scoreboard.c` |
| Aspect choice and which frames present wide | `src/mods/fifa2000_widescreen_plugin.c` |

The mod package that exposes the setting is
`mods/preloaded/packages/fifa2000.enhancement.widescreen`.

## Symbols and framework pin

Symbols: `symbols.toml`, then `python3 tools/sync_symbols.py` writes
`psx_symbols.h` (see `psxrecomp/docs/SYMBOLS.md`). The `psxrecomp` submodule
tracks the fork's `ws-cull-bgez-high-half` branch; bump it deliberately.
