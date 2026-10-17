#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pack the parts of a Doom IWAD that a set of maps needs into a DPK image for the FM-1.

  tools/wadpack.py IWAD OUT.dpk [--maps E1M1,E1M2] [--report]

The FM-1 has 1 MiB of flash in all, so the whole shareware IWAD (4 MB) cannot go in. This keeps what the
chosen maps reach and nothing else:

  maps      the map lumps
  things    every thing a map spawns in single player, followed through the state tables (firmware/doom/info.c)
            to its sprites, the things it fires or drops, and its sounds
  weapons   the fist and the pistol, plus each weapon a map holds or a monster drops
  walls     the textures the sidedefs name, the other half of each switch, the whole of each animation;
            TEXTURE1 and PNAMES are rewritten to hold just those
  flats     the flats the sectors name, and whole animations
  always    PLAYPAL COLORMAP, the status bar, the menus, the HUD font, the title, the intermission,
            the sky, the sounds of the world (doors, lifts, switches, items, the player)

Lumps are deflated (raw deflate, firmware/fm1/inflate.c) when that saves enough; sounds become 4-bit IMA ADPCM
(firmware/fm1/sound.c). The image is little-endian:

  header   "FDPK" u32 version  u32 numlumps  u32 dir offset  u32 image size  u32 end of part a
           u32 end of part b  u32 crc32 of part a after the header  u32 crc32 of part b  u32 crc32 of part c
  The image is cut in up to three parts that live apart: a in the firmware, b and c in the flash's data areas
  (firmware/fm1/fm1.c); no lump is cut.
  dir      per lump: name[8] u32 offset u32 size u32 stored size (== size: stored raw) u32 codec
  data     4-byte aligned
"""
import argparse
import re
import struct
import sys
import zlib
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOOM = ROOT / "firmware" / "doom"

CODEC_RAW, CODEC_DEFLATE, CODEC_ADPCM = 0, 1, 2
VERSION = 3
MAP_LUMPS = ("THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS", "SSECTORS", "NODES", "SECTORS", "REJECT",
             "BLOCKMAP")

# ---------------------------------------------------------------- engine tables (info.c, sounds.c, d_items.c)


def strip_comments_keep(s):
    return re.sub(r"/\*.*?\*/", "", s, flags=re.S)


def load_tables():
    info = (DOOM / "info.c").read_text(errors="replace")
    sprnames = re.findall(r'"([A-Z0-9]{4})"', re.search(r"sprnames\[\]\s*=\s*\{(.*?)\};", info, re.S).group(1))
    states = []
    for m in re.finditer(r"\{(SPR_\w+),(\d+),(-?\d+),\{(\w+)\},(\w+),\d+,\d+\},?\s*//\s*(\w+)", info):
        states.append({"sprite": m.group(1)[4:], "frame": chr(ord("A") + (int(m.group(2)) & 0x7FFF)),
                       "action": m.group(4), "next": m.group(5), "name": m.group(6)})
    state_index = {s["name"]: i for i, s in enumerate(states)}
    mobjs = OrderedDict()
    body = re.search(r"mobjinfo\[NUMMOBJTYPES\](?:\s+DOOM_RODATA)?\s*=\s*\{(.*)\};", info, re.S).group(1)
    for m in re.finditer(r"\{\s*//\s*(MT_\w+)(.*?)\n\s*\}", body, re.S):
        fields = {}
        for f in re.finditer(r"^\s*([^\n]*?),?\s*//\s*(\w+)\s*$", m.group(2), re.M):
            fields[f.group(2)] = f.group(1).strip().rstrip(",")
        mobjs[m.group(1)] = fields
    sounds_c = (DOOM / "sounds.c").read_text(errors="replace")
    sfx = re.findall(r'SOUND\("(\w+)",', sounds_c)
    items = (DOOM / "d_items.c").read_text(errors="replace")
    weapons = [re.findall(r"\b(S_\w+)", w) for w in re.findall(r"\{\s*//\s*\w+(.*?)\}", items, re.S)]
    return sprnames, states, state_index, mobjs, sfx, weapons


# what an action function spawns or plays besides the mobjinfo fields
ACTION_MOBJS = {
    "A_TroopAttack": ["MT_TROOPSHOT"], "A_HeadAttack": ["MT_HEADSHOT"], "A_BruisAttack": ["MT_BRUISERSHOT"],
    "A_SkelMissile": ["MT_TRACER"], "A_Tracer": ["MT_SMOKE"], "A_FatAttack1": ["MT_FATSHOT"],
    "A_FatAttack2": ["MT_FATSHOT"], "A_FatAttack3": ["MT_FATSHOT"], "A_BspiAttack": ["MT_ARACHPLAZ"],
    "A_CyberAttack": ["MT_ROCKET"], "A_PainShootSkull": ["MT_SKULL"], "A_PainAttack": ["MT_SKULL"],
    "A_PainDie": ["MT_SKULL"], "A_VileTarget": ["MT_FIRE"], "A_FireMissile": ["MT_ROCKET"],
    "A_FirePlasma": ["MT_PLASMA"], "A_FireBFG": ["MT_BFG"], "A_BFGSpray": ["MT_EXTRABFG"],
    "A_BrainSpit": ["MT_SPAWNSHOT"], "A_SpawnFly": ["MT_SPAWNFIRE"], "A_BrainScream": ["MT_ROCKET"],
    "A_BrainExplode": ["MT_ROCKET"],
}
ACTION_SFX = {
    "A_PosAttack": ["pistol"], "A_SPosAttack": ["shotgn"], "A_CPosAttack": ["shotgn"], "A_TroopAttack": ["claw"],
    "A_BruisAttack": ["claw"], "A_XScream": ["slop"], "A_PlayerScream": ["pldeth", "pdiehi"], "A_Hoof": ["hoof"],
    "A_Metal": ["metal"], "A_BabyMetal": ["bspwlk"], "A_SkelWhoosh": ["skeswg"], "A_SkelFist": ["skepch"],
    "A_StartFire": ["flamst"], "A_FireCrackle": ["flame"], "A_VileStart": ["vilatk"], "A_VileAttack": ["barexp"],
    "A_FatRaise": ["manatk"], "A_FirePistol": ["pistol"], "A_FireShotgun": ["shotgn"], "A_FireCGun": ["pistol"],
    "A_FireShotgun2": ["dshtgn"], "A_OpenShotgun2": ["dbopn"], "A_LoadShotgun2": ["dbload"],
    "A_CloseShotgun2": ["dbcls"], "A_Punch": ["punch"], "A_Saw": ["sawful", "sawhit"], "A_BFGsound": ["bfg"],
    "A_BrainAwake": ["bossit"], "A_BrainSpit": ["bospit"], "A_SpawnSound": ["boscub"], "A_BrainPain": ["bospn"],
    "A_BrainScream": ["bosdth"], "A_KeenDie": [], "A_CheckReload": [],
}
# the sound families A_Look / A_Scream pick from at random
SFX_FAMILY = {"posit1": ["posit1", "posit2", "posit3"], "bgsit1": ["bgsit1", "bgsit2"],
              "podth1": ["podth1", "podth2", "podth3"], "bgdth1": ["bgdth1", "bgdth2"]}
# a monster's death drops a weapon or a clip (p_inter.c P_KillMobj)
DROPS = {"MT_WOLFSS": "MT_CLIP", "MT_POSSESSED": "MT_CLIP", "MT_SHOTGUY": "MT_SHOTGUN", "MT_CHAINGUY": "MT_CHAINGUN"}
# a picked-up weapon thing -> weaponinfo index (doomdef.h weapontype_t)
WEAPON_OF = {"MT_SHOTGUN": 2, "MT_CHAINGUN": 3, "MT_MISC27": 4, "MT_MISC28": 5, "MT_MISC25": 6, "MT_MISC26": 7,
             "MT_SUPERSHOTGUN": 8}
# the sounds of the world, the player and the menus: always
SFX_ALWAYS = ["pistol", "pstart", "pstop", "doropn", "dorcls", "stnmov", "swtchn", "swtchx", "plpain", "pldeth",
              "pdiehi", "slop", "itemup", "wpnup", "oof", "telept", "noway", "punch", "tink", "bdopn", "bdcls",
              "itmbk", "getpow", "barexp", "sgcock"]
WEAPON_SFX = {2: ["shotgn", "sgcock"], 4: ["rlaunc", "rxplod"], 5: ["plasma", "firxpl"], 6: ["bfg", "rxplod"],
              7: ["sawup", "sawidl", "sawful", "sawhit"], 8: ["dshtgn", "dbopn", "dbcls", "dbload"]}
# switch pairs that p_switch.c knows (alphSwitchList), as name stems after SW1 / SW2
SWITCH_STEMS = ["BRCOM", "BRN1", "BRN2", "BRNGN", "BROWN", "COMM", "COMP", "DIRT", "EXIT", "GRAY", "GRAY1",
                "METAL", "PIPE", "SLAD", "STARG", "STON1", "STON2", "STONE", "STRTG", "BLUE", "CMT", "GARG",
                "GSTON", "HOT", "LION", "SATYR", "SKIN", "VINE", "WOOD", "PANEL", "ROCK", "MET2", "WDMET",
                "BRIK", "MOD1", "ZIM", "STON6", "TEK", "MARB", "SKULL"]
# p_spec.c animdefs: (is texture, first, last)
ANIMS = [(0, "NUKAGE1", "NUKAGE3"), (0, "FWATER1", "FWATER4"), (0, "SWATER1", "SWATER4"), (0, "LAVA1", "LAVA4"),
         (0, "BLOOD1", "BLOOD3"), (0, "RROCK05", "RROCK08"), (0, "SLIME01", "SLIME04"), (0, "SLIME05", "SLIME08"),
         (0, "SLIME09", "SLIME12"), (1, "BLODGR1", "BLODGR4"), (1, "SLADRIP1", "SLADRIP3"), (1, "BLODRIP1", "BLODRIP4"),
         (1, "FIREWALA", "FIREWALL"), (1, "GSTFONT1", "GSTFONT3"), (1, "FIRELAVA", "FIRELAV3"),
         (1, "FIREMAG1", "FIREMAG3"), (1, "FIREBLU1", "FIREBLU2"), (1, "ROCKRED1", "ROCKRED3"),
         (1, "BFALL1", "BFALL4"), (1, "SFALL1", "SFALL4"), (1, "WFALL1", "WFALL4"), (1, "DBRAIN1", "DBRAIN4")]
# lumps every build carries (when the IWAD has them)
ALWAYS = ["PLAYPAL", "COLORMAP", "GENMIDI", "D_INTRO"]
# the title picture (36 KB packed): only on request; without it the title page is the menu's logo
# the HUD font, the status bar, the automap's marks (exact prefixes: "ST" alone would also take the STEP*
# flats and patches and the STIMA stimpack sprite out of their sections)
STATUS_PREFIX = ["STBAR", "STARMS", "STCFN", "STFB0", "STFDEAD", "STFEVL", "STFGOD", "STFKILL", "STFOUCH",
                 "STFST", "STFTL", "STFTR", "STGNUM", "STKEYS", "STTMINUS", "STTNUM", "STTPRCNT", "STYSNUM"]
ALWAYS_PREFIX = STATUS_PREFIX + ["AMMNUM"]
# the menus that this port has (no load / save / read this / other episodes)
MENU = ["M_DOOM", "M_NGAME", "M_OPTION", "M_QUITG", "M_NEWG", "M_SKILL", "M_JKILL", "M_ROUGH", "M_HURT",
        "M_ULTRA", "M_NMARE", "M_EPISOD", "M_EPI1", "M_SKULL1", "M_SKULL2", "M_OPTTTL", "M_ENDGAM", "M_MESSG",
        "M_MSGON", "M_MSGOFF", "M_DETAIL", "M_GDHIGH", "M_GDLOW", "M_SCRNSZ", "M_SVOL", "M_SFXVOL", "M_MUSVOL",
        "M_THERML", "M_THERMM", "M_THERMR", "M_THERMO", "M_PAUSE", "M_MSENS"]
# the intermission's graphics (episode 1), only with more than one map: one map ends in a text screen
WI_PREFIX = ["WIMAP0", "WIA0", "WIURH", "WISPLAT", "WIOSTK", "WIOSTI", "WIF", "WIENTER", "WIKILLS", "WISCRT2",
             "WITIME", "WIPAR", "WISUCKS", "WIMSTT", "WIPCNT", "WIMINUS", "WINUM", "WICOLON", "WILV0", "WIOSTS",
             "WIFRGS", "WIMSTAR", "WIMSTT", "WISCRT", "WIVCTMS", "WIKILRS", "WIBP", "WIP", "WITOTAL", "WIBAR"]
# the flat behind the finale's text (episode 1)
FINALE_FLAT = "FLOOR4_8"
# held for the whole game from their first use (status bar, palette) or for a whole level (the music,
# the synth's instruments): kept raw, read in place, so they cost flash and no RAM
RAW_PREFIX = STATUS_PREFIX + ["AMMNUM", "PLAYPAL", "COLORMAP", "D_", "GENMIDI"]


# ---------------------------------------------------------------- WAD access

class Wad:
    def __init__(self, path):
        d = Path(path).read_bytes()
        kind, n, off = struct.unpack_from("<4sii", d, 0)
        if kind not in (b"IWAD", b"PWAD"):
            raise SystemExit(f"{path}: not a WAD")
        self.data = d
        self.lumps = []
        for i in range(n):
            o, s, nm = struct.unpack_from("<ii8s", d, off + 16 * i)
            self.lumps.append((nm.rstrip(b"\0").decode("ascii", "replace").upper(), o, s))

    def get(self, name, start=0):
        for i in range(start, len(self.lumps)):
            if self.lumps[i][0] == name:
                return i
        return -1

    def read(self, i):
        _, o, s = self.lumps[i]
        return self.data[o:o + s]

    def named(self, name):
        i = self.get(name)
        return self.read(i) if i >= 0 else None

    def section(self, start_names, end_names):
        """[(index, name)] between the markers"""
        out, inside = [], False
        for i, (nm, _, _) in enumerate(self.lumps):
            if nm in start_names:
                inside = True
                continue
            if nm in end_names:
                inside = False
                continue
            if inside and not nm.endswith("_START") and not nm.endswith("_END"):
                out.append((i, nm))
        return out


def name8(b):
    return b.split(b"\0")[0].decode("ascii", "replace").upper()


def parse_textures(wad):
    """[(name, raw texture record, [patch indexes])] from TEXTURE1 (+ TEXTURE2), and PNAMES"""
    pn = wad.named("PNAMES")
    npn = struct.unpack_from("<i", pn)[0]
    pnames = [name8(pn[4 + 8 * i:12 + 8 * i]) for i in range(npn)]
    textures = []
    for lump in ("TEXTURE1", "TEXTURE2"):
        t = wad.named(lump)
        if t is None:
            continue
        n = struct.unpack_from("<i", t)[0]
        offs = struct.unpack_from(f"<{n}i", t, 4)
        for o in offs:
            name = name8(t[o:o + 8])
            npatch = struct.unpack_from("<h", t, o + 20)[0]
            rec = t[o:o + 22 + 10 * npatch]
            patches = [struct.unpack_from("<hhhhh", t, o + 22 + 10 * k)[2] for k in range(npatch)]
            textures.append((name, rec, patches))
    return textures, pnames


def patch_pixels(data):
    """a patch -> width, height, columns of pixels (None = transparent)"""
    wd, ht = struct.unpack_from("<hh", data)
    cols = struct.unpack_from(f"<{wd}I", data, 8)
    img = [[None] * ht for _ in range(wd)]
    for x, o in enumerate(cols):
        while data[o] != 255:
            top, n = data[o], data[o + 1]
            for k in range(n):
                if top + k < ht:
                    img[x][top + k] = data[o + 3 + k]
            o += n + 4
    return wd, ht, img


def encode_patch(w, h, grid):
    """columns of pixels -> a patch (posts of <= 254, no offsets)"""
    cols = []
    for x in range(w):
        col, y = bytearray(), 0
        while y < h:
            if grid[x][y] is None:
                y += 1
                continue
            s = y
            while y < h and grid[x][y] is not None and y - s < 254:
                y += 1
            col += bytes([s, y - s, 0]) + bytes(grid[x][s:y]) + b"\0"
        cols.append(bytes(col) + b"\xff")
    table, body, ofs = b"", b"", 8 + 4 * w
    for c in cols:
        table += struct.pack("<I", ofs + len(body))
        body += c
    return struct.pack("<hhhh", w, h, 0, 0) + table + body


def flatten_textures(wad, textures, pnames, keep):
    """Each texture drawn into one patch of its own (named TEX_nnnn), and TEXTURE1 / PNAMES that use them:
    every column has one patch, so the engine never builds a composite (r_data.c R_GenerateComposite), which
    on the FM-1 cost a texture's patches unpacked all at once and again whenever the composite was purged.
    Returns TEXTURE1, PNAMES and [(patch name, patch)]."""
    patch_lumps = {}
    for i, n in wad.section(("P_START", "PP_START", "P1_START", "P2_START"), ("P_END", "PP_END", "P1_END", "P2_END")):
        patch_lumps.setdefault(n, i)
    cache, recs, pn, out = {}, [], [], []
    for name, rec, patches in textures:
        if name not in keep:
            continue
        w, h = struct.unpack_from("<hh", rec, 12)
        grid = [[None] * h for _ in range(w)]
        for k in range(struct.unpack_from("<h", rec, 20)[0]):
            ox, oy, pi = struct.unpack_from("<hhh", rec, 22 + 10 * k)
            pname = pnames[pi]
            if pname not in cache:
                i = patch_lumps.get(pname, wad.get(pname))
                if i < 0:
                    raise SystemExit(f"patch {pname} is missing")
                cache[pname] = patch_pixels(wad.read(i))
            pw, ph, img = cache[pname]
            for x in range(pw):
                if 0 <= ox + x < w:
                    for y in range(ph):
                        if 0 <= oy + y < h and img[x][y] is not None:
                            grid[ox + x][oy + y] = img[x][y]
        pname = f"TEX_{len(pn):04d}"
        pn.append(pname)
        out.append((pname, encode_patch(w, h, grid)))
        # the record: name, masked, width, height, columndirectory, one patch at 0, 0
        recs.append(rec[:20] + struct.pack("<h", 1) + struct.pack("<hhhhh", 0, 0, len(pn) - 1, 1, 0))
    head = struct.pack("<i", len(recs))
    off = 4 + 4 * len(recs)
    table = b""
    for r in recs:
        table += struct.pack("<i", off)
        off += len(r)
    tex1 = head + table + b"".join(recs)
    pnl = struct.pack("<i", len(pn)) + b"".join(n.encode().ljust(8, b"\0") for n in pn)
    return tex1, pnl, out


def build_textures(textures, pnames, keep):
    """TEXTURE1 + PNAMES holding only the textures in keep (original order), patches re-indexed"""
    kept = [t for t in textures if t[0] in keep]
    used = []
    for _, _, ps in kept:
        for p in ps:
            if pnames[p] not in used:
                used.append(pnames[p])
    index = {n: i for i, n in enumerate(used)}
    recs = []
    for name, rec, ps in kept:
        r = bytearray(rec)
        for k, p in enumerate(ps):
            struct.pack_into("<h", r, 22 + 10 * k + 4, index[pnames[p]])
        recs.append(bytes(r))
    head = struct.pack("<i", len(recs))
    off = 4 + 4 * len(recs)
    table = b""
    for r in recs:
        table += struct.pack("<i", off)
        off += len(r)
    tex1 = head + table + b"".join(recs)
    pn = struct.pack("<i", len(used)) + b"".join(n.encode().ljust(8, b"\0") for n in used)
    return tex1, pn, used


# ---------------------------------------------------------------- closure

def closure(wad, maps, tables):
    sprnames, states, sidx, mobjs, sfx, weapons = tables
    by_ednum = {}
    for mt, f in mobjs.items():
        try:
            by_ednum[int(f.get("doomednum", "-1"))] = mt
        except ValueError:
            pass
    need_mobj, need_wpn, need_tex, need_flat, need_sfx = set(), {0, 1}, set(), set(), set(SFX_ALWAYS)
    for m in maps:
        base = wad.get(m)
        if base < 0:
            raise SystemExit(f"map {m} is not in the IWAD")
        lumps = {wad.lumps[base + 1 + k][0]: wad.read(base + 1 + k) for k in range(len(MAP_LUMPS))}
        th = lumps["THINGS"]
        for k in range(0, len(th) - 9, 10):
            x, y, ang, typ, flags = struct.unpack_from("<hhhhh", th, k)
            if flags & 16:                    # MTF_NOTSINGLE: multiplayer only
                continue
            if typ in by_ednum:
                need_mobj.add(by_ednum[typ])
        sd = lumps["SIDEDEFS"]
        for k in range(0, len(sd) - 29, 30):
            for o in (4, 12, 20):
                n = name8(sd[k + o:k + o + 8])
                if n != "-":
                    need_tex.add(n)
        se = lumps["SECTORS"]
        for k in range(0, len(se) - 25, 26):
            need_flat.add(name8(se[k + 4:k + 12]))
            need_flat.add(name8(se[k + 12:k + 20]))
    need_tex.add("SKY1")
    need_flat.add("F_SKY1")
    need_mobj |= {"MT_PUFF", "MT_BLOOD", "MT_TFOG", "MT_IFOG"}
    # things -> states -> sprites, spawned things, sounds; weapons the same way
    need_spr, seen_states = set(), set()
    work = list(need_mobj)

    def follow(state):
        while state in sidx and state != "S_NULL" and state not in seen_states:
            seen_states.add(state)
            st = states[sidx[state]]
            need_spr.add((st["sprite"], st["frame"]))
            for mt in ACTION_MOBJS.get(st["action"], []):
                if mt not in need_mobj:
                    need_mobj.add(mt)
                    work.append(mt)
            need_sfx.update(ACTION_SFX.get(st["action"], []))
            state = st["next"]

    done = set()
    while work:
        mt = work.pop()
        if mt in done:
            continue
        done.add(mt)
        f = mobjs[mt]
        for k, v in f.items():
            if k.endswith("state"):
                follow(v)
            elif k.endswith("sound") and v.startswith("sfx_") and v != "sfx_None":
                n = v[4:]
                need_sfx.update(SFX_FAMILY.get(n, [n]))
        if mt in DROPS and DROPS[mt] not in need_mobj:
            need_mobj.add(DROPS[mt])
            work.append(DROPS[mt])
        if mt in WEAPON_OF:
            need_wpn.add(WEAPON_OF[mt])
    for w in sorted(need_wpn):
        for s in weapons[w]:
            follow(s)
        need_sfx.update(WEAPON_SFX.get(w, []))
        for mt in {4: ["MT_ROCKET"], 5: ["MT_PLASMA"], 6: ["MT_BFG", "MT_EXTRABFG"]}.get(w, []):
            if mt not in done:
                need_mobj.add(mt)
                work.append(mt)
        while work:                          # weapons' projectiles
            mt = work.pop()
            if mt in done:
                continue
            done.add(mt)
            for k, v in mobjs[mt].items():
                if k.endswith("state"):
                    follow(v)
                elif k.endswith("sound") and v.startswith("sfx_") and v != "sfx_None":
                    need_sfx.update(SFX_FAMILY.get(v[4:], [v[4:]]))
    # switches: both halves; animations: the whole range
    for stem in SWITCH_STEMS:
        a, b = "SW1" + stem, "SW2" + stem
        if a in need_tex or b in need_tex:
            need_tex |= {a, b}
    return need_mobj, need_wpn, need_spr, need_tex, need_flat, need_sfx


def expand_anims(order, need, is_tex, anims):
    for t, first, last in anims:
        if t != is_tex or first not in order or last not in order:
            continue
        i, j = order.index(first), order.index(last)
        rng = order[i:j + 1]
        if need & set(rng):
            need |= set(rng)


# ---------------------------------------------------------------- map lumps

# These map lumps never change while a level plays: the engine reads them in place (p_setup.c), so they
# are stored in its own layout, raw: VERTEXES as vertex_t (fixed_t x, y), NODES as node_t (fixed_t x y dx dy,
# bbox[2][4], u16 children[2]), BLOCKMAP and REJECT as they are.
MAP_IN_PLACE = ("VERTEXES", "NODES", "BLOCKMAP", "REJECT")


def map_lump(name, d):
    if name == "VERTEXES":
        return b"".join(struct.pack("<ii", x << 16, y << 16) for x, y in struct.iter_unpack("<hh", d[:len(d) // 4 * 4]))
    if name == "NODES":
        out = bytearray()
        for k in range(len(d) // 28):
            v = struct.unpack_from("<hhhh8h2H", d, 28 * k)
            out += struct.pack("<4i8i2H", *(c << 16 for c in v[:12]), *v[12:])
        return bytes(out)
    return d


# ---------------------------------------------------------------- codecs

IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
            88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
            658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
            3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
            16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def ima_encode(pcm16):
    """4-bit IMA ADPCM, low nibble first; the decoder (sound.c) starts at predictor 0, index 0"""
    pred, idx, out, nib = 0, 0, bytearray(), []
    for s in pcm16:
        step = IMA_STEP[idx]
        diff = s - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            code |= 4
            diff -= step
            delta += step
        step >>= 1
        if diff >= step:
            code |= 2
            diff -= step
            delta += step
        step >>= 1
        if diff >= step:
            code |= 1
            delta += step
        pred = pred - delta if code & 8 else pred + delta
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + IMA_INDEX[code & 7]))
        nib.append(code)
    if len(nib) & 1:
        nib.append(0)
    for k in range(0, len(nib), 2):
        out.append(nib[k] | nib[k + 1] << 4)
    return bytes(out)


def lowpass_resample(pcm, src_rate, dst_rate):
    """[-32768..32767] at src_rate -> dst_rate: windowed-sinc low-pass at 0.45 dst_rate, then linear"""
    import math
    if dst_rate >= src_rate:
        return list(pcm), src_rate
    fc = 0.45 * dst_rate / src_rate
    taps = 31
    h = []
    for k in range(taps):
        x = k - (taps - 1) / 2
        sinc = 2 * fc if x == 0 else math.sin(2 * math.pi * fc * x) / (math.pi * x)
        h.append(sinc * (0.54 - 0.46 * math.cos(2 * math.pi * k / (taps - 1))))
    g = sum(h)
    h = [v / g for v in h]
    n = len(pcm)
    filt = []
    for i in range(n):
        acc = 0.0
        for k in range(taps):
            j = i + k - (taps - 1) // 2
            if 0 <= j < n:
                acc += h[k] * pcm[j]
        filt.append(acc)
    out = []
    step = src_rate / dst_rate
    t = 0.0
    while t < n - 1:
        i = int(t)
        f = t - i
        out.append(int(round(filt[i] * (1 - f) + filt[i + 1] * f)))
        t += step
    return [max(-32768, min(32767, v)) for v in out], dst_rate


def convert_sound(raw, mode, rate):
    """DMX sound lump -> our sound lump: u16 codec (0 PCM u8, 2 ADPCM) u16 rate u32 samples, then the data"""
    fmt, src_rate, n = struct.unpack_from("<HHI", raw)
    if fmt != 3 or n < 32 or 8 + n > len(raw):
        return None
    pcm8 = raw[8 + 16:8 + n - 16]               # DMX pads 16 bytes at each end
    pcm, r = lowpass_resample([(b - 128) << 8 for b in pcm8], src_rate, rate)
    if mode == "pcm8":
        return struct.pack("<HHI", 0, r, len(pcm)) + bytes(((v >> 8) + 128) & 0xFF for v in pcm)
    return struct.pack("<HHI", 2, r, len(pcm)) + ima_encode(pcm)


def deflate(b):
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    return c.compress(b) + c.flush()


# ---------------------------------------------------------------- pack

def pack(wad_path, maps, sfx_mode, sfx_rate, split, verbose, demos=False, title=False):
    wad = Wad(wad_path)
    tables = load_tables()
    need_mobj, need_wpn, need_spr, need_tex, need_flat, need_sfx = closure(wad, maps, tables)
    textures, pnames = parse_textures(wad)
    tex_order = [t[0] for t in textures]
    flat_lumps = wad.section(("F_START", "FF_START"), ("F_END", "FF_END"))
    flat_order = [n for _, n in flat_lumps]
    need_flat.add(FINALE_FLAT)
    expand_anims(tex_order, need_tex, 1, ANIMS)
    expand_anims(flat_order, need_flat, 0, ANIMS)
    missing = sorted(t for t in need_tex if t not in tex_order)
    if missing:
        print(f"wadpack: textures not in the IWAD (dropped): {' '.join(missing)}")
        need_tex -= set(missing)
    tex1, pnames_lump, flat_patches = flatten_textures(wad, textures, pnames, need_tex)
    intermission = len(maps) > 1

    out = []                                  # (name, bytes, category)

    def add(name, data, cat):
        out.append((name, data, cat))

    names_present = {n for n, _, _ in wad.lumps}
    for n in ALWAYS + (["D_INTER"] if intermission else []) + (["D_VICTOR"] if "E1M8" in maps else []) + \
            (["TITLEPIC"] if title else []):
        if n in names_present:
            add(n, wad.named(n), "always")
    for n in MENU:
        if n in names_present:
            add(n, wad.named(n), "ui")
    for i, (n, _, size) in enumerate(wad.lumps):
        if size and (any(n.startswith(p) for p in ALWAYS_PREFIX)
                     or intermission and any(n.startswith(p) for p in WI_PREFIX)):
            add(n, wad.read(i), "ui")
    if demos:                                 # the IWAD's demos (tests: their maps must be in too)
        for n in ("DEMO1", "DEMO2", "DEMO3", "DEMO4"):
            if n in names_present:
                add(n, wad.named(n), "demos")
    for m in maps:
        base = wad.get(m)
        add(m, b"", "map")
        for k, n in enumerate(MAP_LUMPS):
            add(n, map_lump(n, wad.read(base + 1 + k)), "map")
        mus = "D_" + m
        if mus in names_present:
            add(mus, wad.named(mus), "music")
    add("TEXTURE1", tex1, "walls")
    add("PNAMES", pnames_lump, "walls")
    add("P_START", b"", "walls")
    for n, d in flat_patches:
        add(n, d, "walls")
    add("P_END", b"", "walls")
    add("F_START", b"", "flats")
    for i, n in flat_lumps:
        if n in need_flat:
            add(n, wad.read(i), "flats")
    add("F_END", b"", "flats")
    add("S_START", b"", "sprites")
    for i, n in wad.section(("S_START", "SS_START"), ("S_END", "SS_END")):
        if (n[:4], n[4]) in need_spr or (len(n) > 6 and (n[:4], n[6]) in need_spr):
            add(n, wad.read(i), "sprites")
    add("S_END", b"", "sprites")
    for n in sorted(need_sfx):
        raw = wad.named("DS" + n.upper())
        if raw is None:
            continue
        c = convert_sound(raw, sfx_mode, sfx_rate)
        if c is not None:
            add("DS" + n.upper(), c, "sounds")

    # dedupe by name (first wins; map lumps repeat per map); a name in two sections is a packing error
    seen, lumps = set(), []
    for n, d, cat in out:
        if cat != "map" and n in seen:
            first = next(c for m, _, c in out if m == n)
            if first != cat:
                raise SystemExit(f"wadpack: {n} would be in both {first} and {cat}")
            continue
        seen.add(n)
        lumps.append((n, d, cat))
    head = 40
    dir_off = head
    pos = dir_off + 24 * len(lumps)
    blob = bytearray()
    entries = []
    stats = {}
    caps = list(split or [])                  # room in part a, part b (part c: the rest)
    cuts = []                                 # image offsets where part b, part c begin
    for n, d, cat in lumps:
        codec, stored = CODEC_RAW, d
        if cat == "sounds":
            codec = CODEC_ADPCM if struct.unpack_from("<H", d)[0] == 2 else CODEC_RAW
        elif len(d) >= 64 and not any(n.startswith(p) for p in RAW_PREFIX) and \
                not (cat == "map" and n in MAP_IN_PLACE):
            z = deflate(d)
            if len(z) < len(d) * 0.9:
                codec, stored = CODEC_DEFLATE, z
        while (pos + len(blob)) % 4:
            blob.append(0)
        while len(cuts) < len(caps) and pos + len(blob) + len(stored) - (cuts[-1] if cuts else 0) > caps[len(cuts)]:
            cuts.append(pos + len(blob))        # this lump and the rest go to the next part
            if cuts[0] < dir_off + 24 * len(lumps):
                raise SystemExit("wadpack: the directory does not fit the first part")
        entries.append((n, pos + len(blob), len(d), len(stored), codec))
        blob += stored
        st = stats.setdefault(cat, [0, 0, 0])
        st[0] += 1
        st[1] += len(d)
        st[2] += len(stored)
    while len(blob) % 4:
        blob.append(0)
    total = pos + len(blob)
    while len(cuts) < 2:
        cuts.append(total)
    end_a, end_b = cuts
    dirb = b"".join(struct.pack("<8sIIII", n.encode().ljust(8, b"\0"), o, sz, cs, c) for n, o, sz, cs, c in entries)
    image = bytearray(head) + dirb + bytes(blob)
    assert len(image) == total
    crc_a = zlib.crc32(bytes(image[head:end_a]))
    crc_b = zlib.crc32(bytes(image[end_a:end_b]))
    crc_c = zlib.crc32(bytes(image[end_b:]))
    struct.pack_into("<4sIIIIIIIII", image, 0, b"FDPK", VERSION, len(entries), dir_off, total, end_a, end_b,
                     crc_a, crc_b, crc_c)
    report = [f"wadpack: {len(entries)} lumps, {total} bytes ({', '.join(maps)}); "
              f"parts {end_a} + {end_b - end_a} + {total - end_b} B"]
    for cat, (k, sz, z) in sorted(stats.items(), key=lambda t: -t[1][2]):
        report.append(f"  {cat:8s} {k:4d} lumps {sz:8d} -> {z:8d} B")
    if verbose:
        report.append("  mobjs: " + " ".join(sorted(need_mobj)))
        report.append("  sprites: " + " ".join(sorted(sp + "".join(sorted(f for s2, f in need_spr if s2 == sp))
                                                      for sp in {sp for sp, _ in need_spr})))
        report.append("  weapons: " + " ".join(map(str, sorted(need_wpn))))
        report.append("  sounds: " + " ".join(sorted(need_sfx)))
        report.append(f"  textures {len(need_tex)}, flats {len(need_flat)}")
    return bytes(image), report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iwad", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--maps", default="E1M1", help="comma-separated, e.g. E1M1,E1M2 (default E1M1)")
    ap.add_argument("--sfx", choices=("adpcm", "pcm8"), default="adpcm")
    ap.add_argument("--sfx-rate", type=int, default=8000, help="sample rate of the sounds (default 8000)")
    ap.add_argument("--split", type=lambda v: [int(x, 0) for x in v.split(",")],
                    help="room in part a[,part b]: the image is cut where a lump would not fit")
    ap.add_argument("--max", type=lambda v: int(v, 0), help="fail if the image is bigger")
    ap.add_argument("--demos", action="store_true", help="add the IWAD's demos (for tests)")
    ap.add_argument("--title", action="store_true", help="add the title picture (TITLEPIC)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    maps = [m.strip().upper() for m in a.maps.split(",") if m.strip()]
    img, report = pack(a.iwad, maps, a.sfx, a.sfx_rate, a.split, a.verbose, a.demos, a.title)
    print("\n".join(report))
    if a.max is not None and len(img) > a.max:
        raise SystemExit(f"wadpack: {len(img)} B is more than the {a.max} B there is room for: fewer maps")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(img)
    return 0


if __name__ == "__main__":
    sys.exit(main())
