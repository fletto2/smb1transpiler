# smb1transpiler

Rebuild the a2vera Apple II + VERA port of *Super Mario Bros 1* from your own ROMs, using
only a C compiler. No assembler, no Python, no emulator.

The port draws SMB1 with *Super Mario All-Stars* artwork. Neither ROM may be redistributed,
so this repository ships no Nintendo data at all, down to individual tiles and palette
entries. What it ships is metadata: where in each ROM the needed blocks live, how to convert
them, and the port's own 6502 code. Supply the two dumps and the tool derives the rest.

> Noncommercial use only: research, education, preservation and hobby use. See
> [License](#license) and [Credits](#credits).

```sh
make                                    # or: cc -O2 -o smb1transpiler *.c
./smb1transpiler                        # writes out/smb1_vera.dsk
```

```
NES  SMB1 : smb1.nes  (40976 bytes, CHR at 0x8010)
SNES SMAS : smas.sfc  (2097152 bytes)
...
wrote out/smb1_vera.dsk (143360 bytes)
  CRC32 8875B7F8 -- matches the shipped disk
```

With no arguments the tool writes a 143,360-byte `.dsk` that is md5-identical to the shipped
`smb1_vera.dsk` (`94b439ba`). It checks that itself and exits nonzero on a mismatch, because
a disk that is nearly right still boots into garbage.

Other flags: `--verify tiles_vera.bin` for a per-block tileset report, `--vram vram.dat` to
overwrite a reference stream instead of building one, `--out DIR`, and `--force` to build
from a dump whose fingerprint does not match.

## Input

The two ROMs are found by extension rather than filename, so name your dumps whatever you
like:

| pattern | what | how it is validated |
|---|---|---|
| `*.nes` | Super Mario Bros. | iNES magic, 2×16 KB PRG + 1×8 KB CHR, then a CRC32 of the PRG and CHR |
| `*.sfc` `*.smc` `*.bin` (the SNES TOSEC set uses `.bin`) | Super Mario All-Stars (USA) | 2 MB LoROM; the SNES header's checksum/complement pair at `$00:FFDC`; then a CRC32 of the three graphics banks. A 512-byte copier header is detected by size remainder and skipped. |

Container validation is structural on purpose: no title string is compared, so nothing
identifying has to be embedded. The CRCs cover the data, and a CRC is a fingerprint. It
cannot reconstruct a byte of anything.

### Which dumps work

Structural validation alone turned out to be nowhere near enough. Running every Super Mario
Bros. dump in the NES TOSEC set (108 of them) through the tool *before* the CRC gate existed:

| outcome | count |
|---|---|
| byte-exact shipped disk | 5 |
| failed loudly (a relocation rule hit a byte it did not expect) | 14 |
| rejected at the door (not 2 PRG + 1 CHR) | 5 |
| accepted, and silently produced a different, wrong disk | **84** |

Hacks, translations and bad dumps all satisfy "iNES magic + 2 PRG + 1 CHR", and what comes
out the far end is a disk that boots and then misbehaves, which no exit code reports. With
the CRC gate the same 108 dumps give 5 exact, 98 rejected by fingerprint, 5 rejected
structurally, and no silent wrong disks.

The 5 that work share one `(PRG, CHR)` CRC32 pair, differing only in iNES header padding and
trailing junk. No other dump in the set shares it:

```
PRG CRC32 5CF548D3   CHR CRC32 867B51AD
  811b027eaf99c2def7b933c5208636de  Super Mario Bros. (1985-09-13)(Nintendo)(JP-US)
  8d5b58ccffe1ebefcd3c4a28fdae3aab  Super Mario Bros. (1985)(Nintendo)(PlayChoice-10)
  a71fc3709ae3c0a49c3a00b44d7de85f  ...(JP-US)[h Morgan][enlarged rom]
  3c89d9e821a2b2000aae1b8ddc864ac3  ...(JP-US)[h Vimm][iNES title]
  93b3db665cb10efe8fa8b0a076d17920  ...(JP-US)[h][iNES title]
```

The PAL/EU dumps fail loudly on relocation rule 3, because the port's tables are measured
against the NTSC code.

On the SNES side the checksum/complement test also false-positives: *Metal Warriors (US)* is
a 2 MB LoROM that passes it, and the bank CRCs reject it on 3 of 5 blocks. Verified working
are the plain 2 MB image as `.sfc` or `.bin`, and the same image carrying a 512-byte copier
header as `.smc`. Both build the byte-exact disk.

`--force` bypasses the fingerprint loudly, for anyone deliberately feeding a ROM hack.

## What it derives

Everything below is driven by `a2vera_blobs.h`, the one generated file in this repository.
It holds the port's own 6502 code and the offset tables that say where to read your ROMs.
It is built in the main a2vera tree, which has the 6502 sources and the assembler; nothing
in this repository needs either.

### The 768-tile L0 background tileset

All 768 exact. SMAS keeps no ready-made sheet anywhere in the ROM; it DMAs different banks
into different VRAM windows as it runs, so the port's `tiles_vera.bin` is a snapshot of
assembled VRAM. `TILERULES[]` in the header replays that, one rule per tile, in four kinds.
All four read your ROMs:

| kind | source | used for |
|---|---|---|
| 0 | SMAS offset, straight 4bpp→4bpp | 757 tiles |
| 1 | SMAS offset + a 16-entry index remap | 4, the water-level coral re-index |
| 2 | NES CHR tile + a 4-entry remap | 5, the tree-ledge stem and the overworld fence |
| 3 | SMAS offset masked by a NES tile's silhouette | 2, the castle battlement crenellation |

Those rules were solved rather than written by hand: given a target tile and a candidate
source, the index map is fully determined, so the tool that emitted the header searched for a
consistent one and hard-errored if any tile had no derivation at all.

### The 32 KB game image

Built from your PRG in three ordered stages. Every stage is positional, so the header ships
offsets and the bytes come from the ROM:

1. the port's own source-level edits to `prg.asm` (5 bytes, the §9 vine-snap table planted in
   SMB1's unused space), applied at the original base so that stage 2 rebases them;
2. the three mechanical rules: 1465 offsets take `−$78` (the relocation to `$0800`), 36 take
   `$20 → $FE` and 51 take `$40 → $FF` (the PPU/APU register shadows). Measured by diffing an
   original-base assembly against the port's, leaving no unexplained bytes;
3. the port's own hooks (109 bytes of `JMP`s into its routines plus `$EA` pad), spliced last.
   19 of them deliberately overwrite a byte stage 2 rewrote.

Order matters in both directions, and getting it wrong is nearly invisible. Running stage 1
after stage 2 leaves one operand pointing 30 KB away; running stage 3 before stage 2 would
subtract `$78` from a jump target.

### The disk layout

`disk.c`. A `.dsk` is not nibblized, so the whole job is placing pages at (track, *physical*
sector) through the DOS 3.3 skew. The track map is a contract with `resident.asm`. The
LC-audio and APU-LUT tracks are computed from the VRAM stream's real length and cross-checked
against the numbers the shipped payload was patched with, so growing the art fails the build
instead of streaming audio off the wrong track.

### The APU divide LUT

`floor(300240/(p+1))` for p = 0..2047. It verifies its own pulse/triangle identity rather
than trusting it.

### The VERA stream

All 7 chunks, built from scratch. (`--vram <file>` still exists: it loads a reference stream
and substitutes the derived chunks into it, which is how each one was checked as it landed.
Either way, a derived chunk off by one byte moves the disk's md5.)

| chunk | size | source |
|---|---|---|
| L0 tilemap `$00000` | 6 KB | constant, every cell the blank/sky tile `$024` at palette offset 0 |
| L1 tilemap `$01000` | 2 KB | constant, the same tile, transparent |
| L0 tileset `$02000` | 24 KB | `TILERULES` (above) |
| b0A sprites `$14000` | 8 KB | SMAS ROM `$50000`, straight bank copy |
| b07 sprites `$16000` | 8 KB | SMAS ROM `$38000` + the port's injections (below) |
| HUD font `$1A000` | 8 KB | NES CHR pattern table 1, promoted to 4bpp |
| palette `$1FA00` | 512 B | SMAS BG palette pool + Mario's table + the port's overrides (below) |

The three SMAS graphics banks are plain ROM slices, verified byte-for-byte (`b0A $50000`,
`b07 $38000`, `b06 $30000`), so nothing there needs a search and the offsets serve as the
metadata. b0A is 256/256 verbatim; b07 is 209/256 verbatim plus 47 injected slots, which
`sprites.c` assembles the way the asset pipeline does. NES-CHR tiles come through a
palette-index remap (the lift, the castle raise-flag, the flame, the bubble, the brick chunk,
the cloud puff, the explosions), b06 BG art is borrowed for objects SMAS has no OBJ tile for
(the brick, the vine), and six slots are 16×16 composites (Bowser ×5, the hammer, the flag,
the floatey "000", the coin crops).

A VERA 16×16 sprite is ONE 128-byte image rather than four 8×8 tiles, which is why a
byte-level search for the composites in the ROM finds nothing and they have to be assembled.

Injection order matters: the brick injection overwrites three of the four quads of SMAS's own
16×16 hammer, so the hammer is built from the untouched ROM into a different slot. Reordering
corrupts it silently.

### The palette

The chunk that looked least derivable, since a palette carries no structure to search for.
14 of the 16 blocks turn out to be exact selectors into SMAS's own BG palette pool (`DATA_04AEC3`, indexed through
`DATA_04AE3F`, both verified to sit at their LoROM offsets), and one is Mario's `DATA_0499FD`
verbatim. On top sit 14 single-entry overrides and one fully authored block (offset 8, the
cloud/hill/fence palette). Those are the port's own: the sky backdrop, the ?-block/coin gold
ramp seeded to crtab step 0, §28's coral copies, and the forced-black off4 idx8 that
`setup_black_tile` depends on.

The overrides ship as literals on purpose. 58 of the 64 entries in the odd blocks do occur
somewhere in the pool, so each could be "cited" as a ROM offset, but a colour that happens to
appear elsewhere is coincidence rather than provenance. Dressing the port's own palette
choices up as ROM references would be laundering them. They are this project's data and they
ship as this project's data.

The L0 map chunk is `$1800`, not `$1000`: VRAM `$0000-$17FF` spans the L0 map *and* the L1
map, so the L1 half rides along in that chunk and is then sent a second time as its own.
Filling only the first `$1000` leaves the disk wrong by exactly 2048 bytes.

## Gotchas

**NES background tiles are pattern table 1** (CHR index `$1xx`). Reading table 0 for a
background tile is a bug this project shipped once, which put a sprite tile on screen as the
tree-ledge stem.

**High nibble is the LEFT pixel** in VERA 4bpp. At least one script in the main repo has this
backwards and renders every tile mirrored.

**The embedded 6502 blobs in `a2vera_blobs.h` are post-patch.** `mkboot` resolves labels and
rewrites operands into the resident/payload/LC images at build time, so raw assembler output
does not match the disk. The header carries the patched images, taken from mkboot's
`A2VERA_DUMP_BLOBS` hook.

## Credits

**Adrian Black** contributed the NES/SNES controller concept and the controller code this
port uses. The Apple II has no digital pad; his approach drives LATCH and CLOCK from
annunciators AN0/AN1 and reads the shift register back on PB2, which is what gives the port a
real d-pad. `read_nes` is his routine, kept byte-identical so that its per-step timing, the
part that decides whether a real controller latches, is his rather than a re-derivation of
it. He also tested the port on hardware.
[github.com/misterblack1](https://github.com/misterblack1/) ·
[Adrian's Digital Basement](https://www.youtube.com/@adriansdigitalbasement)

**Joe Burks** developed the A2VERA card the port renders on, and tested the port on hardware.
[github.com/jburks](https://github.com/jburks) ·
[Wavicle on Tindie](https://www.tindie.com/stores/wavicle/)

**Michael Morrison** contributed code and tested the port on hardware.
[github.com/code-bythepound](https://github.com/code-bythepound)

Several defects in this port only ever appeared on metal: the RWTS seek-phase and
nibble-decode bugs, and the VERA-card address corruption that traced back to the Apple II
slot having no VDA/VPA pins. Emulation showed none of them.

## License

[PolyForm Noncommercial 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0),
full text in [LICENSE](LICENSE). Use, modification and redistribution are permitted for
noncommercial purposes only: research, education, preservation, private study and hobby use.
Commercial use, and commercial use of anything derived from this, are not permitted.

PolyForm Noncommercial was chosen over the more familiar CC BY-NC because Creative Commons
recommends against applying its licenses to software; they say nothing about source versus
object form, or about patents. PolyForm is drafted for software and defines "noncommercial"
in operative terms.

This is not an open-source license by the OSI definition, which admits no restriction on
field of endeavour. The restriction is deliberate, and it means this repository cannot be
vendored into an OSI-licensed project and that some distributions will not package it.

What the license does not cover:

- **The game.** *Super Mario Bros.* is copyright © Nintendo, first published 1985. *Super
  Mario All-Stars* is copyright © Nintendo, first published 1993. The titles, characters,
  artwork, music and code are Nintendo's property, and both names are Nintendo trademarks.
  This project is not affiliated with, authorised by, endorsed by or connected to Nintendo
  in any way. The names appear here only to identify which ROMs the tool reads.
- **The ROMs.** Nothing here licenses them, and this repository deliberately ships none of
  their data. You supply your own dumps, and whether you may possess them is between you and
  your local law.
- **The output.** A built `.dsk` contains Nintendo's code and artwork. It is not this
  project's to license and no grant here extends to it. Do not redistribute it.
- **Third-party code.** The controller routine is Adrian Black's work, licensed MIT. MIT
  permits commercial use, so that portion stays MIT and the noncommercial restriction above
  does not attach to it. The MIT text is reproduced at the end of [LICENSE](LICENSE); keep
  its copyright notice with any copy you make.
