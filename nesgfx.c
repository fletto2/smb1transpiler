/*
 * nesgfx.c -- EXPERIMENTAL: draw the a2vera SMB1 port with the original
 * 1985 NES graphics instead of the Super Mario All-Stars artwork.
 *
 * Nothing here needs the SNES ROM.  Every pixel comes from the NES CHR and
 * every colour from the game's own palette writes, so the look follows the
 * NES rather than a fixed palette picked per area.
 *
 * How the stock port draws, and what this changes:
 *
 *   L0, the level.  The port does not emulate a nametable for the playfield.
 *   It hooks RenderAreaGraphics ($88AE -> $A800) and the block replacer
 *   ($BEF6 -> $A8B2) and draws each 16x16 metatile straight from a table of
 *   VERA map entries in the payload: LB15B[256] maps an NES metatile id to an
 *   entry, and the entries at $A95B are 8 bytes each, TL/BL/TR/BR (the NES's
 *   own column-major order), each a VERA (tile, palette) pair pointing into
 *   the SMAS tileset.
 *      -> LB15B becomes the identity.  Entry m is rebuilt from the game's
 *         own MetatileGraphics tables, read out of the caller's PRG: the four
 *         NES tile numbers, unchanged, with VERA palette offset 4 + (m >> 6).
 *         SMB1 takes a metatile's attribute palette from the top two bits of
 *         its id, so this is the NES attribute behaviour at its native 16x16
 *         grain.  The L0 tileset becomes NES pattern table 1, tile N at N.
 *      -> Three SMAS-only helpers are stubbed with RTS: $B8FF (swaps entries
 *         $56/$63 for neighbour-aware SMAS joins), $B3B8 (uploads a SMAS
 *         palette per area type, then $BADE/$BB0C rewrite entries 7, 8 and
 *         $56 of the table above at runtime).
 *
 *   L1, the HUD and text screens.  Already an NES nametable + attribute
 *   emulator in the LC ($F576, hooked over UpdateScreen) drawing NES pattern
 *   table 1 with palette offset 4 + attribute.  Unchanged; L0 now uses the
 *   same four offsets, so both layers share one set of NES BG palettes.
 *
 *   Sprites.  One VERA 8x8 sprite per NES OAM entry, through a 256 x 3 table
 *   at $FA0A (slot, bank, palette) and fixed per TILE, plus special cases that
 *   swap groups of NES tiles for SMAS 16x16 composites.
 *      -> The table becomes tile t -> slot t of a sheet holding NES pattern
 *         table 0, so no tile reaches the special cases.  The palette byte is
 *         taken from OAM attribute bits 0-1 instead (offset 8 + p), patched
 *         at $9A32 in both residents.  Flip and priority already came from
 *         the OAM attribute byte.
 *
 *   Palette.  The parser drops every $3Fxx write.  Its second pass is
 *   retargeted through a trampoline in its own NOP pad to nesgfx/palhook.s,
 *   which lands at $B400 over the SMAS palette tables freed above, and turns
 *   each $3Fxx write into a VERA palette write via a 64-entry NES colour
 *   table.  Area palettes, the ?-block colour cycle, star flashing, fiery
 *   Mario, Luigi and the black intermission backdrop are then the game's
 *   own.  Three port routines that wrote SMAS colours over those offsets are
 *   stubbed: $F757 (?-block gold ramp), $F7CF (per-area sprite colours),
 *   $F92B (forced black backdrop).
 *
 * Every patch site is checked for the bytes it expects before it is written,
 * so a header from a different port build fails loudly instead of producing
 * a disk that boots into garbage.
 */
#include <stdio.h>
#include <string.h>
#include "smb1transpiler.h"
#include "a2vera_blobs.h"

#define RES_BASE  0x9000L
#define PAY_BASE  0xA800L
#define LC_BASE   0xE000L

#define MT_ENTRIES 0xA95BL      /* 256 x 8 VERA map entries          */
#define MT_INDEX   0xB15BL      /* NES metatile id -> entry          */
#define SPR_TABLE  0xFA0AL      /* 256 x 3: slot, bank, palette - 4  */
#define PALHOOK    0xB400L

/* nesgfx/palhook.s assembled at $B400.  Tables follow the code. */
static const unsigned char palhook_code[] = {
  0xAD, 0x29, 0xF5, 0xC9, 0x3F, 0xD0, 0x4E, 0xAD, 0x2C, 0xF5, 0xF0, 0x49,
  0x8D, 0x59, 0xB4, 0xAD, 0x2A, 0xF5, 0x29, 0x1F, 0x8D, 0x58, 0xB4, 0xA0,
  0x03, 0xAE, 0x58, 0xB4, 0xBD, 0x7A, 0xB4, 0xF0, 0x1F, 0x8D, 0x01, 0xC4,
  0xBD, 0x5A, 0xB4, 0x8D, 0x00, 0xC4, 0xA9, 0x11, 0x8D, 0x02, 0xC4, 0xB1,
  0x00, 0x29, 0x3F, 0xAA, 0xBD, 0x9A, 0xB4, 0x8D, 0x03, 0xC4, 0xBD, 0xDA,
  0xB4, 0x8D, 0x03, 0xC4, 0x2C, 0x2B, 0xF5, 0x70, 0x01, 0xC8, 0xAE, 0x58,
  0xB4, 0xE8, 0x8A, 0x29, 0x1F, 0x8D, 0x58, 0xB4, 0xCE, 0x59, 0xB4, 0xD0,
  0xC4, 0x4C, 0xE7, 0xF6, 0x00, 0x00
};
#define PH_ADRLO  0xB45AL
#define PH_ADRHI  0xB47AL
#define PH_COLGB  0xB49AL
#define PH_COLR   0xB4DAL
#define PH_END    0xB51AL     /* < $B600: the freed SMAS tables */

/*
 * The NES master palette as RGB.  This is this project's own choice of a
 * common 2C02 approximation, not data from either ROM; it is quantised to
 * VERA's 4 bits per channel below.  $0D-$0F, $1D-$1F, $2E-$2F, $3E-$3F are
 * black.
 */
static const unsigned long nes_rgb[64] = {
  0x747474, 0x24188C, 0x0000A8, 0x44009C, 0x8C0074, 0xA80010, 0xA40000,
  0x7C0800, 0x402C00, 0x004400, 0x005000, 0x003C14, 0x183C5C, 0x000000,
  0x000000, 0x000000,
  0xBCBCBC, 0x0070EC, 0x2038EC, 0x8000F0, 0xBC00BC, 0xE40058, 0xD82800,
  0xC84C0C, 0x887000, 0x009400, 0x00A800, 0x009038, 0x008088, 0x000000,
  0x000000, 0x000000,
  0xFCFCFC, 0x3CBCFC, 0x5C94FC, 0xCC88FC, 0xF478FC, 0xFC74B4, 0xFC7460,
  0xFC9838, 0xF0BC3C, 0x80D010, 0x4CDC48, 0x58F898, 0x00E8D8, 0x787878,
  0x000000, 0x000000,
  0xFCFCFC, 0xA8E4FC, 0xC4D4FC, 0xD4C8FC, 0xFCC4FC, 0xFCC4D8, 0xFCBCB0,
  0xFCD8A8, 0xFCE4A0, 0xE0FCA0, 0xA8F0BC, 0xB0FCCC, 0x9CFCF0, 0xC4C4C4,
  0x000000, 0x000000
};

static void
vera_rgb (int nes, unsigned char *gb, unsigned char *r)
{
  unsigned long c = nes_rgb[nes & 0x3F];
  int R = (int) ((((c >> 16) & 0xFF) * 15 + 127) / 255);
  int G = (int) ((((c >> 8) & 0xFF) * 15 + 127) / 255);
  int B = (int) (((c & 0xFF) * 15 + 127) / 255);

  *gb = (unsigned char) ((G << 4) | B);
  *r = (unsigned char) R;
}

/* NES 2bpp tile -> VERA 4bpp, indices 0-3, high nibble is the LEFT pixel */
static void
chr_to_vera (const unsigned char *chr, int t, unsigned char *out)
{
  const unsigned char *o = chr + (long) t * NES_TILE_BYTES;
  int x, y, a, b;

  for (y = 0; y < 8; y++)
    for (x = 0; x < 8; x += 2)
      {
        a = ((o[y] >> (7 - x)) & 1) | (((o[y + 8] >> (7 - x)) & 1) << 1);
        b = ((o[y] >> (6 - x)) & 1) | (((o[y + 8] >> (6 - x)) & 1) << 1);
        out[y * 4 + x / 2] = (unsigned char) ((a << 4) | b);
      }
}

/* write n bytes at a CPU address after checking the bytes already there */
static int
patch (unsigned char *img, long base, long len, long addr,
       const unsigned char *expect, int nexp,
       const unsigned char *val, int n, const char *what,
       char *err, size_t errsz)
{
  long o = addr - base;

  if (o < 0 || o + (n > nexp ? n : nexp) > len)
    {
      snprintf (err, errsz, "%s: $%04lX lies outside its image", what, addr);
      return -1;
    }
  if (expect && memcmp (img + o, expect, (size_t) nexp))
    {
      snprintf (err, errsz, "%s: $%04lX does not hold the expected bytes "
                "-- a2vera_blobs.h is not the port build nesgfx was "
                "written against", what, addr);
      return -1;
    }
  memcpy (img + o, val, (size_t) n);
  return 0;
}

#define PATCH(img, base, addr, exp, val, what)                          \
  patch ((img), (base), (long) sizeof (img), (addr), (exp),             \
         (int) sizeof (exp), (val), (int) sizeof (val), (what), err, errsz)

/*
 * Find SMB1's MetatileGraphics_Low/High.  RenderAreaGraphics loads both
 * halves with the same index register into $06/$07:
 *
 *      LDA lo,Y / STA $06 / LDA lo+4,Y / STA $07      (B9 .. .. 85 06 ...)
 *
 * The four pointers it yields must ascend inside the PRG and the first
 * metatile, the blank, must be $24 x 4.  Every hit has to agree, and there
 * has to be at least one; anything else is an error rather than a guess.
 * The PRG is CRC-gated, so on a good dump this is deterministic.
 */
static int
find_metatiles (const unsigned char *prg, unsigned ptr[4],
                char *err, size_t errsz)
{
  unsigned lo, hi, p[4];
  int i, k, hits = 0, ok;

  for (i = 0; i + 10 <= 32768; i++)
    {
      if ((prg[i] != 0xB9 && prg[i] != 0xBD) || prg[i + 5] != prg[i]
          || prg[i + 3] != 0x85 || prg[i + 4] != 0x06
          || prg[i + 8] != 0x85 || prg[i + 9] != 0x07)
        continue;
      lo = prg[i + 1] | (prg[i + 2] << 8);
      hi = prg[i + 6] | (prg[i + 7] << 8);
      if (hi != lo + 4 || lo < 0x8000 || hi + 4 > 0x10000)
        continue;
      for (k = 0, ok = 1; k < 4; k++)
        {
          p[k] = prg[lo - 0x8000 + k] | (prg[hi - 0x8000 + k] << 8);
          if (p[k] < 0x8000 || p[k] + 4 > 0x10000
              || (k && p[k] <= p[k - 1]))
            ok = 0;
        }
      if (!ok || memcmp (prg + (p[0] - 0x8000), "\x24\x24\x24\x24", 4))
        continue;
      if (hits && memcmp (p, ptr, sizeof (p)))
        {
          snprintf (err, errsz, "two different metatile tables found "
                    "($%04X and $%04X); refusing to guess", ptr[0], p[0]);
          return -1;
        }
      memcpy (ptr, p, sizeof (p));
      hits++;
    }
  if (!hits)
    {
      snprintf (err, errsz, "MetatileGraphics not found in the PRG");
      return -1;
    }
  return 0;
}

/*
 * The port reaches VERA through absolute $C400-$C41F operands: slot 4,
 * hard-wired, in every published build.  Move them to slot n.  A site is an
 * absolute-mode opcode followed by $00-$1F, $C4.  Checked against a
 * disassembly of each image: every match is code, including the ones da65
 * mis-syncs on ($9ABA, the DC_VIDEO write that turns VGA on).  The counts
 * are pinned so that a different port build fails instead of half-moving.
 */
static int
slot_patch (unsigned char *img, long len, int slot, int expect,
            const char *what, char *err, size_t errsz)
{
  static const unsigned char abs_ops[] = {
    0x0D, 0x0E, 0x19, 0x1D, 0x1E, 0x20, 0x2C, 0x2D, 0x2E, 0x39, 0x3D, 0x3E,
    0x4C, 0x4D, 0x4E, 0x59, 0x5D, 0x5E, 0x6D, 0x6E, 0x79, 0x7D, 0x7E, 0x8C,
    0x8D, 0x8E, 0x99, 0x9D, 0xAC, 0xAD, 0xAE, 0xB9, 0xBC, 0xBD, 0xBE, 0xCC,
    0xCD, 0xCE, 0xD9, 0xDD, 0xDE, 0xEC, 0xED, 0xEE, 0xF9, 0xFD, 0xFE
  };
  long i;
  int n = 0;

  for (i = 0; i + 2 < len; i++)
    if (img[i + 2] == 0xC4 && img[i + 1] < 0x20
        && memchr (abs_ops, img[i], sizeof (abs_ops)))
      n++;
  if (n != expect)
    {
      snprintf (err, errsz, "%s: %d VERA register operands, expected %d",
                what, n, expect);
      return -1;
    }
  for (i = 0; i + 2 < len; i++)
    if (img[i + 2] == 0xC4 && img[i + 1] < 0x20
        && memchr (abs_ops, img[i], sizeof (abs_ops)))
      img[i + 2] = (unsigned char) (0xC0 + slot);
  return 0;
}

int
nesgfx_slot (struct nesgfx *g, int slot, char *err, size_t errsz)
{
  if (slot < 1 || slot > 7)
    {
      snprintf (err, errsz, "slot %d: must be 1-7", slot);
      return -1;
    }
  if (slot == 4)
    return 0;
  /* the payload's 32 plus palhook's five */
  if (slot_patch (g->resident, sizeof (g->resident), slot, 56, "resident",
                  err, errsz)
      || slot_patch (g->resident_po, sizeof (g->resident_po), slot, 56,
                     "ProDOS resident", err, errsz)
      || slot_patch (g->payload, sizeof (g->payload), slot, 37, "payload",
                     err, errsz)
      || slot_patch (g->lcaud, sizeof (g->lcaud), slot, 58, "LC audio",
                     err, errsz))
    return -1;
  return 0;
}

int
nesgfx_build (const unsigned char *prg, const unsigned char *chr,
              struct nesgfx *g, char *err, size_t errsz)
{
  static const unsigned char rts[] = { 0x60 };
  unsigned char mt[256 * 8], idx[256], spr[256 * 3];
  unsigned char adrlo[32], adrhi[32], colgb[64], colr[64];
  unsigned ptr[4];
  long a;
  int m, k, t, i;

  memcpy (g->resident, blob_resident, sizeof (g->resident));
  memcpy (g->resident_po, blob_resident_po, sizeof (g->resident_po));
  memcpy (g->payload, blob_payload, sizeof (g->payload));
  memcpy (g->lcaud, blob_lc_audio, sizeof (g->lcaud));

  /* ---- graphics chunks ---- */
  memset (g->tiles, 0, sizeof (g->tiles));
  for (t = 0; t < 256; t++)
    chr_to_vera (chr, 256 + t, g->tiles + t * VERA_TILE_BYTES);
  /* tile $300 is the port's solid fill (setup_black_tile writes $88 into
     it at runtime); it lives past the 768-tile chunk, so nothing to do */
  for (t = 0; t < 256; t++)
    {
      chr_to_vera (chr, t, g->spr_b0a + t * 32);
      chr_to_vera (chr, t, g->spr_b07 + t * 32);
    }
  build_font (chr, g->font);

  /* the starting palette: black, with grey ramps in the eight NES
     palettes so that anything drawn before the game's first palette write
     is still legible.  Offset 4 index 8 stays black for the solid fill. */
  memset (g->pal, 0, sizeof (g->pal));
  for (k = 4; k < 12; k++)
    {
      static const int ramp[4] = { 0x0F, 0x00, 0x10, 0x30 };

      for (i = 1; i < 4; i++)
        vera_rgb (ramp[i], &g->pal[k * 32 + i * 2],
                  &g->pal[k * 32 + i * 2 + 1]);
    }

  /* ---- L0 metatiles from the game's own tables ---- */
  if (find_metatiles (prg, ptr, err, errsz))
    return -1;
  g->mt_ptr[0] = ptr[0];
  g->mt_ptr[1] = ptr[1];
  g->mt_ptr[2] = ptr[2];
  g->mt_ptr[3] = ptr[3];
  for (m = 0; m < 256; m++)
    {
      /* exactly what the NES reads: group pointer + 4 * low six bits, even
         for ids the game never uses */
      a = (long) ptr[m >> 6] + 4L * (m & 0x3F) - 0x8000L;
      for (k = 0; k < 4; k++)
        {
          mt[m * 8 + k * 2] = (unsigned char) (a + k < 32768 ? prg[a + k]
                                               : 0x24);
          mt[m * 8 + k * 2 + 1] = (unsigned char) ((4 + (m >> 6)) << 4);
        }
      idx[m] = (unsigned char) m;
    }
  {
    static const unsigned char e_mt[] = { 0x24, 0x00, 0x24, 0x00 };
    static const unsigned char e_idx[] = { 0x00, 0x01, 0x04, 0x05 };

    if (patch (g->payload, PAY_BASE, sizeof (g->payload), MT_ENTRIES,
               e_mt, 4, mt, (int) sizeof (mt), "metatile entries",
               err, errsz)
        || patch (g->payload, PAY_BASE, sizeof (g->payload), MT_INDEX,
                  e_idx, 4, idx, 256, "metatile index", err, errsz))
      return -1;
  }

  /* ---- SMAS-only helpers off ---- */
  {
    static const unsigned char e_b8ff[] = { 0xE0, 0x00, 0xD0, 0x05 };
    static const unsigned char e_b3b8[] = { 0xA9, 0x00, 0x8D, 0x00, 0xC4 };
    static const unsigned char e_f757[] = { 0xAD, 0x47, 0x99, 0x0A };
    static const unsigned char e_f7cf[] = { 0x20, 0xDF, 0xB9 };
    static const unsigned char e_f92b[] = { 0xA9, 0x00, 0x8D, 0x00, 0xC4 };

    if (PATCH (g->payload, PAY_BASE, 0xB8FFL, e_b8ff, rts, "$B8FF")
        || PATCH (g->payload, PAY_BASE, 0xB3B8L, e_b3b8, rts, "$B3B8")
        || PATCH (g->lcaud, LC_BASE, 0xF757L, e_f757, rts, "$F757")
        || PATCH (g->lcaud, LC_BASE, 0xF7CFL, e_f7cf, rts, "$F7CF")
        || PATCH (g->lcaud, LC_BASE, 0xF92BL, e_f92b, rts, "$F92B"))
      return -1;
  }

  /* ---- sprites: tile t is slot t, bank b0A, palette from OAM ---- */
  for (t = 0; t < 256; t++)
    {
      spr[t * 3] = (unsigned char) t;
      spr[t * 3 + 1] = 0;
      spr[t * 3 + 2] = 4;       /* unread now; the stock "offset 8" */
    }
  {
    static const unsigned char e_spr[] = { 0x00, 0x00, 0x05 };
    /* LDA $8A00,Y / STA $42 / CPY #$76 ... -> LDA $53 / AND #3 / ORA #8 /
       STA $42 / JMP $9A49, NOP to the end of the coral special case */
    static const unsigned char e_pal[] = {
      0xB9, 0x00, 0x8A, 0x85, 0x42, 0xC0, 0x76, 0x90, 0x0E, 0xC0, 0x7A,
      0xB0, 0x0A, 0xA5, 0x53, 0x29, 0x01, 0xF0, 0x04, 0xA9, 0x0D, 0x85,
      0x42, 0xA4, 0x53
    };
    static const unsigned char n_pal[] = {
      0xA5, 0x53, 0x29, 0x03, 0x09, 0x08, 0x85, 0x42, 0x4C, 0x49, 0x9A,
      0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA,
      0xEA, 0xA4, 0x53
    };

    if (patch (g->lcaud, LC_BASE, sizeof (g->lcaud), SPR_TABLE, e_spr, 3,
               spr, (int) sizeof (spr), "sprite table", err, errsz)
        || PATCH (g->resident, RES_BASE, 0x9A32L, e_pal, n_pal,
                  "sprite palette")
        || PATCH (g->resident_po, RES_BASE, 0x9A32L, e_pal, n_pal,
                  "sprite palette (ProDOS resident)"))
      return -1;
  }

  /* ---- palette emulation ---- */
  for (i = 0; i < 32; i++)
    {
      int e = i & 3, p = (i >> 2) & 3;
      unsigned v;

      if (i == 0x00 || i == 0x10)
        v = 0xFA00;             /* backdrop: $3F10 mirrors $3F00 */
      else if (!e)
        v = 0;                  /* $3F04/08/0C and mirrors: never shown */
      else
        v = 0xFA00 + (unsigned) ((i < 0x10 ? 4 + p : 8 + p) * 32 + e * 2);
      adrlo[i] = (unsigned char) (v & 0xFF);
      adrhi[i] = (unsigned char) (v >> 8);
    }
  for (i = 0; i < 64; i++)
    vera_rgb (i, &colgb[i], &colr[i]);
  {
    static const unsigned char e_bcs[] = { 0xC9, 0x28, 0xB0, 0x13 };
    static const unsigned char n_bcs[] = { 0xC9, 0x28, 0xB0, 0x16 };
    static const unsigned char e_nop[] = { 0xEA, 0xEA, 0xEA };
    static const unsigned char n_jmp[] = { 0x4C, PALHOOK & 0xFF,
      PALHOOK >> 8
    };

    /* $B400 was SMAS palette data only $B3B8 read; nothing to check
       beyond it being inside the payload */
    if (patch (g->payload, PAY_BASE, sizeof (g->payload), PALHOOK, NULL, 0,
               palhook_code, (int) sizeof (palhook_code), "palhook",
               err, errsz)
        || patch (g->payload, PAY_BASE, sizeof (g->payload), PH_ADRLO, NULL,
                  0, adrlo, 32, "palhook", err, errsz)
        || patch (g->payload, PAY_BASE, sizeof (g->payload), PH_ADRHI, NULL,
                  0, adrhi, 32, "palhook", err, errsz)
        || patch (g->payload, PAY_BASE, sizeof (g->payload), PH_COLGB, NULL,
                  0, colgb, 64, "palhook", err, errsz)
        || patch (g->payload, PAY_BASE, sizeof (g->payload), PH_COLR, NULL,
                  0, colr, 64, "palhook", err, errsz)
        || PATCH (g->lcaud, LC_BASE, 0xF618L, e_bcs, n_bcs, "$F618")
        || PATCH (g->lcaud, LC_BASE, 0xF632L, e_nop, n_jmp, "$F632"))
      return -1;
  }
  return 0;
}
