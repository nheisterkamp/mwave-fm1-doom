# Doom on the M-VAVE FM-1

Doom running on the M-VAVE FM-1 pocket FM synthesizer: the shareware episode's first map, E1M1 "Hangar", with
its monsters, sounds and status bar, played on the FM-1's keys and knobs, on its 240×240 screen. The music is
played by a small FM synthesizer after the OPL2 chip Doom's music was written for, with Doom's own instrument
bank.

The hardware layer, boot code, update loader and USB stack come from [Felucca](https://github.com/hugelton/Felucca),
the open firmware for the FM-1 that this project learned the hardware from. The engine is
[doomgeneric](https://github.com/ozkl/doomgeneric) (Chocolate Doom), cut down to fit.

> **Status: runs on an FM-1.** Installed from Felucca, game data uploaded, E1M1 played on the device. Off the
> device it is also tested on a 32-bit build with exactly the FM-1's memory (long random play sessions), with
> the shareware demos, and with the upload against the firmware's own code. Installing firmware is at your own
> risk; see [Getting back](#getting-back).

## What fits, and how

The FM-1 has a 240 MHz JieLi AC791N, 512 KB of RAM and **1 MB of flash in all**. The shareware IWAD is 4 MB.

| | |
| --- | --- |
| Flash | the firmware (257 KB of code) and the first 324 KB of game data in the 568 KB app slot; the other 345 KB of game data in the flash above it, sent once over USB (`tools/fm1_doom.py upload`) |
| Game data | `tools/wadpack.py` keeps what the chosen maps need, followed through Doom's state tables: the sprites and only the frames that are shown, the sounds, the textures (each flattened into one patch), the flats, the status bar and menus. Lumps are deflated; sounds are 4-bit ADPCM at 8 kHz; the music, the status bar and the map's static structures stay raw and are read in place from flash |
| RAM | the screen, the renderer's tables and pools (sized from the shareware demos' peaks, and drawing a little wrong instead of stopping if a view needs more), and a 255 KB zone for the level and a cache of unpacked lumps |
| Screen | Doom's 320×200 frame, filtered to 240×180 (Doom's 4:3), between black bands |
| Not in | savegames, the other maps, the title picture (`--title` adds it), the intermission screens (one map ends in a text screen), multiplayer |

More maps can be packed (`--maps E1M1,E1M2`) as long as they fit the flash; the build says when they do not.

## Controls

The keys are laid out for two hands: movement under the left, actions under the right.

```
 left hand                              right hand
   F#3  G#3  A#3        C#4               C#5         D#5
   fwd  fwd  fwd        run               prev weap   next weap
 F3   G3   A3   B3   C4                 B4    C5    D5    E5
 strf turn back turn strf               run   FIRE  USE   FIRE
 left left      right right
```

| Control | In the game | In the menus |
| --- | --- | --- |
| F#3 / G#3 / A#3 | forward | up |
| A3 | back | down |
| G3 / B3 | turn left / right | left / right |
| F3 / C4 | strafe left / right | |
| C#4, B4 (held) | run | |
| C5, E5, **PLAY** | fire | select, yes |
| D5, **REC** | use (doors, switches) | select, yes |
| C#5 / D#5, **OCT−** / **OCT+** | previous / next weapon | |
| **FX SCL ENV LFO EDIT GLO** | weapons 1–6 | |
| **ARP** | weapon 7 | |
| **HOME** | menu | close the menu, no |
| **SEQ** | map | |
| **SELECT**, **KNOB 1–4** | turn (a click is ~4°) | |
| **MASTER** | volume | |

The LEDs glow on every key and button that does something, and light up while held. Holding **HOME** while
switching on starts the data mode (below); **OCT−** and **OCT+** held for 5 seconds enter UBOOT.

## Building

On macOS or Linux, with Docker (the JieLi toolchain is Linux x86-64 only; on macOS each step runs in a
`linux/amd64` container):

```sh
tools/get_toolchain.sh                    # JieLi's clang for the pi32v2, to ~/.jieli/toolchain
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK   # three files of the package
cp /path/to/doom1.wad wad/                # the shareware IWAD (wad/README.md)
python3 tools/build.py
```

It makes `build/doom.fwsc` (the firmware package) and `build/doom.dpk` (the game data). Options:

| | |
| --- | --- |
| `--maps E1M1,E1M2` | the maps to pack (default E1M1) |
| `--title` | the title picture (36 KB of flash) |
| `--sfx-rate 11025` | the sounds at their full rate (more flash) |
| `--lcd-baud 2` | the screen's SPI clock at 20 MHz instead of 12 (a frame in 35 ms instead of 58); the panel is rated lower, so try it and keep it if the picture stays clean |
| `--iwad PATH` | another IWAD |

## Installing

```sh
pip install mido python-rtmidi
python3 tools/fm1_doom.py install         # the firmware, over USB (Felucca's update path)
python3 tools/fm1_doom.py upload          # the game data: once, and after a build with other data
```

`install` works from the stock firmware, from Felucca and from an earlier FM-1 Doom. On the first start the FM-1
has no game data yet and waits in its **data mode** ("THE GAME DATA IS MISSING"); `upload` writes it (about
half a minute), checks it, and restarts the FM-1 into the game. An upload to a running game restarts it in data
mode by itself. `python3 tools/fm1_doom.py info` tells what the FM-1 runs and whether its data is good.

The upload writes the flash above the firmware, where the stock firmware and Felucca keep user patches, presets
and projects: back them up first.

## Getting back

- **Another firmware**: `python3 tools/fm1_install.py FM-1.fwsc` (the official V15 package from M-VAVE), or
  M-VAVE's own updater, or Felucca's installer. FM-1 Doom answers the update requests in the game and in the
  data mode.
- **Data mode**: hold HOME while switching on. No game runs; the installer and the upload work. The FM-1 also
  goes there by itself after two starts in a row that died in their first 30 seconds.
- **UBOOT**: hold OCT− and OCT+ for 5 seconds (in the game or the data mode); after four failed starts in a row
  the FM-1 goes there by itself. From UBOOT, [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter)
  can write the flash.
- A fatal Doom error shows its message for 10 seconds and restarts; the USB serial port (115200 8N1, any rate)
  carries the engine's log.

## Testing without an FM-1

```sh
tests/run_tests.sh            # all; Docker for the 32-bit runs
tests/run_tests.sh quick
```

- `host/`: the engine on this computer, headless (`make -C host`, then `build/host/doom -dpk build/doom.dpk
  -warp 1 1 -frames 400 -shot 100,400 -wav out.wav -stats`). It uses the firmware's own screen conversion and
  control mapping; screenshots are the FM-1's 240×240 picture, input comes from a script (`-script`, e.g.
  `tests/e1m1_walk.txt`, keys or FM-1 controls such as `fm1 down PLAY`).
- Built as 32-bit x86 (`host/Dockerfile.i386`), its structures are the FM-1's size: `tests/run_tests.sh` plays
  three 20 000-frame random sessions (`tests/gen_fuzz.py`) in exactly the firmware's zone.
- `tests/test_upload.py`: `tools/fm1_doom.py upload` against the firmware's upload code on a simulated flash.
- The shareware demos play through with every map packed (`--demos`): the engine's changes leave the game alone.

## Layout

| | |
| --- | --- |
| `firmware/doom/` | the engine (doomgeneric), changed to read a DPK image from flash, to fit the RAM, and with the platform glue (`i_video.c`, `i_sound.c`, `i_music.c`, `i_system.c`, `w_wad.c`, `inflate.c`) |
| `firmware/fm1/` | the FM-1 side: `fm1.c` (boot, screen, sound, keys, USB, data mode), `lcd_conv.c`, `fm1_input_map.c`, `dpk_proto.c`, and Felucca's `usb.c`, `ota.c`, `libc.c` |
| `firmware/hal/`, `firmware/loader/`, `firmware/crt0.S` | Felucca's hardware layer and update loader |
| `firmware/app.ld` | the memory map |
| `tools/` | `build.py`, `wadpack.py` (the game data), `fm1_doom.py` (install, upload), Felucca's `fm1pkg_make.py`, `fm1_install.py`, `lz4blk.py`, `get_toolchain.sh` |
| `host/`, `tests/` | the computer build and the tests |

## Licence

GPL-3.0-only, as Felucca; see [LICENSING.md](LICENSING.md) for the parts. Doom is © id Software; the engine
code is GPL-2.0-or-later. M-VAVE and FM-1 are trademarks of their owners; this project is not affiliated with
or endorsed by them, by id Software, or by Hügelton Instruments.
