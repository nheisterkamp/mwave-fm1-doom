# Licensing

FM-1 Doom as a whole is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full
text in [LICENSE](LICENSE)). If you share the firmware, share its complete source with it under the same licence.

| Part | From | Licence |
| --- | --- | --- |
| `firmware/hal/`, `firmware/loader/`, `firmware/crt0.S`, `firmware/fm1/usb.c`, `firmware/fm1/ota.c`, `firmware/fm1/libc.c`, `tools/fm1pkg_make.py`, `tools/fm1_install.py`, `tools/lz4blk.py`, `tools/get_toolchain.sh` | [Felucca](https://github.com/hugelton/Felucca), © 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; `usb.c` with its USB names changed, the loader with its include paths | GPL-3.0-only |
| `firmware/doom/` | [doomgeneric](https://github.com/ozkl/doomgeneric) by ozkl, from [Chocolate Doom](https://www.chocolate-doom.org/) by Simon Howard and contributors, from the Doom source by id Software (© 1993–1996 id Software, Inc.); changed for the FM-1 | GPL-2.0-or-later (used here under version 3) |
| `firmware/doom/inflate.c`, `i_music.c`, `i_sound.c`, `doomgeneric.c` and the other new files of the engine | this project | GPL-2.0-or-later |
| everything else: `firmware/fm1/fm1.c`, `lcd_conv.c`, `fm1_input_map.c`, `dpk_proto.c`, `libc_ext.c`, `firmware/app.ld`, `tools/build.py`, `tools/wadpack.py`, `tools/fm1_doom.py`, `tools/gen_font.py`, `host/`, `tests/` | this project | GPL-3.0-only |
| three files the package carries: the SPL `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` (taken from the JieLi AC79 SDK at build time, not in this tree) | JieLi | Apache-2.0, [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt): give it along with every package |

## The game data

Doom's game data (maps, graphics, sounds, music, the instrument bank) is id Software's and is not in this
repository: you supply an IWAD (`wad/`). The shareware IWAD may be shared unmodified; a package or `.dpk` built
from it holds a cut-down, re-encoded part of it, so do not share builds made from it: share the source.
