/*
 * sprites.c -- the VERA sprite sheets, palette, HUD font and tilemaps,
 * derived from the SMAS and NES ROMs.
 *
 * The three SMAS graphics banks are plain ROM slices, verified byte for byte,
 * so nothing here needs a search: the offsets are the metadata.
 *
 *	b0A  $50000  32K  only the first 8K is uploaded
 *	b07  $38000  16K  512 tiles; bank 0 is uploaded, the composites below
 *			  source their quads from tiles 256-511
 *	b06  $30000   8K  BG tiles; the brick and vine injections come from here
 */
#include <string.h>
#include "smb1transpiler.h"
#include "a2vera_blobs.h"

#define SMAS_B0A	0x50000L
#define SMAS_B07	0x38000L
#define SMAS_B06	0x30000L

/* DATA_04AEC3 / DATA_04AE3F / DATA_0499FD, at their LoROM offsets */
#define SMAS_POOL	0x22EC3L
#define SMAS_AE3F	0x22E3FL
#define SMAS_MPAL	0x219FDL

/* SNES 4bpp: two bitplane pairs 16 bytes apart */
static int spx(const unsigned char *bank, int t, int x, int y)
{
	const unsigned char *o = bank + (long)t * SNES_TILE_BYTES;
	int b = 7 - x;

	return ((o[y * 2] >> b) & 1)
	     | (((o[y * 2 + 1] >> b) & 1) << 1)
	     | (((o[16 + y * 2] >> b) & 1) << 2)
	     | (((o[16 + y * 2 + 1] >> b) & 1) << 3);
}

/* SMAS lays tiles out 16 per row, so a 16x16 is base, +1, +16, +17 */
static int quad_px(const unsigned char *bank, int base, int px, int py)
{
	return spx(bank, base + (px >= 8) + 16 * (py >= 8), px & 7, py & 7);
}

static int npx(const unsigned char *chr, int t, int x, int y)
{
	const unsigned char *o = chr + (long)t * NES_TILE_BYTES;
	int b = 7 - x;

	return ((o[y] >> b) & 1) | (((o[y + 8] >> b) & 1) << 1);
}

static void put8(unsigned char *sheet, int slot, int x, int y, int lo, int hi)
{
	sheet[slot * 32 + y * 4 + x / 2] = (unsigned char)((lo << 4) | hi);
}

/* an NES CHR tile through a 4-entry palette-index remap */
static void inj_nes(unsigned char *b07, const unsigned char *chr, int ntile,
		    int slot, const unsigned char *remap)
{
	int x, y;

	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x += 2)
			put8(b07, slot, x, y, remap[npx(chr, ntile, x, y)],
			     remap[npx(chr, ntile, x + 1, y)]);
}

static void inj_smas8(unsigned char *b07, const unsigned char *bank, int src,
		      int slot)
{
	int x, y;

	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x += 2)
			put8(b07, slot, x, y, spx(bank, src, x, y),
			     spx(bank, src, x + 1, y));
}

/*
 * A VERA 16x16 sprite is ONE 128-byte image spanning slot..slot+3, not four
 * 8x8 tiles.  That is why a byte-level search for these composites in the ROM
 * finds nothing: the pixels are there, the byte layout is not.
 */
static void inj_quad16(unsigned char *b07, const unsigned char *bank, int base,
		       int slot, const unsigned char *remap16)
{
	int a, b, x, y;

	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x += 2) {
			a = quad_px(bank, base, x, y);
			b = quad_px(bank, base, x + 1, y);
			if (remap16) {
				a = remap16[a];
				b = remap16[b];
			}
			b07[slot * 32 + y * 8 + x / 2] =
					(unsigned char)((a << 4) | b);
		}
	}
}

/* SNES BGR555 -> VERA: drop each component's low bit, pack [G|B],[-|R] */
static void vera_pal(const unsigned char *rom, long off, unsigned char *out)
{
	unsigned x;
	int i, r, g, b;

	for (i = 0; i < 16; i++) {
		x = (unsigned)rom[off + i * 2]
		  | ((unsigned)rom[off + i * 2 + 1] << 8);
		r = (x & 31) >> 1;
		g = ((x >> 5) & 31) >> 1;
		b = ((x >> 10) & 31) >> 1;
		out[i * 2] = (unsigned char)((g << 4) | b);
		out[i * 2 + 1] = (unsigned char)r;
	}
}

/*
 * 14 of the 16 blocks are exact selectors into SMAS's own BG palette pool and
 * one is Mario's table verbatim.  The rest is ours: the sky backdrop, the
 * ?-block gold ramp seeded to crtab step 0, the coral copies, and the forced
 * black at off4 idx8 that setup_black_tile depends on -- making that one
 * non-black once turned the whole lives card white.
 */
void build_palette(const struct rom *smas, unsigned char *out)
{
	const unsigned char *r = smas->data + smas->base;
	const struct palrule *p;
	unsigned char *dst;
	long sel;
	int i, k;

	for (k = 0; k < 16; k++) {
		p = &PALRULES[k];
		dst = out + k * 32;
		if (p->kind == 1) {
			vera_pal(r, SMAS_MPAL, dst);
		} else if (p->kind == 0) {
			sel = (long)r[SMAS_AE3F + p->sel * 2]
			    | ((long)r[SMAS_AE3F + p->sel * 2 + 1] << 8);
			vera_pal(r, SMAS_POOL + (sel / 2) * 2, dst);
		} else {
			memset(dst, 0, 32);	/* every entry is an override */
		}
		for (i = 0; i < p->nov; i++) {
			dst[p->ov[i * 3] * 2] = p->ov[i * 3 + 1];
			dst[p->ov[i * 3] * 2 + 1] = p->ov[i * 3 + 2];
		}
	}
}

/*
 * The HUD font is the NES BACKGROUND CHR promoted to 4bpp.  Background tiles
 * are PATTERN TABLE 1, i.e. CHR index $1xx -- reading table 0 here would put
 * sprite art in the status bar, a bug this project has shipped once.
 */
void build_font(const unsigned char *chr, unsigned char *out)
{
	int t, x, y;

	for (t = 0; t < 256; t++)
		for (y = 0; y < 8; y++)
			for (x = 0; x < 8; x += 2)
				out[t * 32 + y * 4 + x / 2] = (unsigned char)
					((npx(chr, 256 + t, x, y) << 4)
					 | npx(chr, 256 + t, x + 1, y));
}

/* L0 boots as the blank/sky tile at palette offset 0, L1 as the same tile
 * transparent */
void build_maps(unsigned char *l0, unsigned char *l1)
{
	int i;

	for (i = 0; i < 64 * 32; i++) {
		l0[i * 2] = 0x24;
		l0[i * 2 + 1] = 0x00;
	}
	for (i = 0; i < 32 * 32; i++) {
		l1[i * 2] = 0x24;
		l1[i * 2 + 1] = 0x80;
	}
}

/*
 * NES sprite palette 2 is $0F,$16,$30,$27 = transparent/red/white/orange, and
 * VERA offset 11 carries idx3 red, idx1 white, idx4 orange, so {1:3, 2:1, 3:4}
 * is the index-faithful map for every pal2 object.  The lift and the castle
 * raise-flag shipped 3-way rotations of it for months: black bodies, and a red
 * flag that should have been white.
 */
static const unsigned char pal2_remap[4]   = { 0, 3, 1, 4 };
static const unsigned char bubble_remap[4] = { 0, 0, 1, 0 };  /* ring is idx2 */
static const unsigned char chunk_remap[4]  = { 0, 2, 0, 4 };
static const unsigned char puff_remap[4]   = { 0, 0, 1, 4 };

/* NES tile -> b07 slot, for the plain CHR injections */
static const struct {
	unsigned short		ntile;
	unsigned char		slot;
	const unsigned char	*remap;
} nes_inject[] = {
	{ 0x5B, 0x0C, pal2_remap },	/* small platform / lift	*/
	{ 0x54, 0x20, pal2_remap },	/* castle raise-flag,	*/
	{ 0x55, 0x21, pal2_remap },	/* four 8x8s in a 2x2	*/
	{ 0x56, 0x22, pal2_remap },
	{ 0x57, 0x23, pal2_remap },
	{ 0x51, 0x24, pal2_remap },	/* Bowser flame		*/
	{ 0x52, 0x25, pal2_remap },
	{ 0x53, 0x26, pal2_remap },
	{ 0x74, 0x27, bubble_remap },	/* air bubble		*/
	{ 0x84, 0x1F, chunk_remap },	/* brick chunk		*/
	{ 0x75, 0x37, puff_remap },	/* cloud lift, and the	*/
					/* end-of-level ball	*/
	{ 0x66, 0x48, pal2_remap },	/* explosion, fireworks	*/
	{ 0x67, 0x5D, pal2_remap },	/* these come from a	*/
	{ 0x68, 0x5F, pal2_remap },	/* TABLE, so an LDA #$xx*/
					/* audit missed them	*/
};

/* jumping coin: the CENTRED crop (columns 4-11) of SMAS's 16x16 coin.  The
 * top-left quadrant cut the right five columns off -- IoU 0.00 against the NES
 * tile.  The game's own V-flipped second sprite supplies the bottom half. */
static const unsigned short coin_base[4] = { 0x28, 0x2A, 0x2C, 0x2E };
static const unsigned char  coin_slot[4] = { 0x4C, 0x4D, 0x4E, 0x4F };

/* four body quads plus the mouth-open head, index-remapped to VERA offset 13 */
static const unsigned short bowser_base[5] = {
	0x1C2, 0x1EE, 0x1E2, 0x1C0, 0x1E0
};
static const unsigned char bowser_slot[5] = { 0x00, 0x04, 0x08, 0x10, 0x14 };

/*
 * ⚠ Injection order is load-bearing.  The brick injection overwrites b07
 * $45/$47/$57, three of the four quads of SMAS's own 16x16 hammer at $44 --
 * which is exactly why the hammer is built from the untouched ROM into a
 * different slot rather than referenced in place.  Reorder and it corrupts
 * silently.
 */
int build_sprite_sheets(const struct rom *smas, const unsigned char *chr,
			unsigned char *b0a, unsigned char *b07)
{
	const unsigned char *sb0a, *sb07, *sb06;
	unsigned i, x, y;

	if (smas->base + SMAS_B0A + 8192 > smas->size)
		return -1;
	sb0a = smas->data + smas->base + SMAS_B0A;
	sb07 = smas->data + smas->base + SMAS_B07;
	sb06 = smas->data + smas->base + SMAS_B06;

	for (i = 0; i < 256; i++) {
		for (y = 0; y < 8; y++) {
			for (x = 0; x < 8; x += 2) {
				put8(b0a, i, x, y, spx(sb0a, i, x, y),
				     spx(sb0a, i, x + 1, y));
				put8(b07, i, x, y, spx(sb07, i, x, y),
				     spx(sb07, i, x + 1, y));
			}
		}
	}

	inj_smas8(b07, sb06, 0x45, 0x45);	/* BG brick, block-bump	*/
	inj_smas8(b07, sb06, 0x47, 0x47);
	inj_smas8(b07, sb06, 0x57, 0x57);

	inj_quad16(b07, sb07, 0x120, 0x58, NULL);	/* flag		*/
	inj_quad16(b07, sb07, 0x044, 0x1B, NULL);	/* hammer	*/

	inj_smas8(b07, sb06, 0x41, 0x32);	/* vine crown; no OBJ vine */
	inj_smas8(b07, sb06, 0x2F, 0x33);	/* vine segment		   */

	/* floatey "000": SMAS digits are ~5px wide, so the thousands popup
	 * cannot fit in one 8x8.  It is a 16x8 from bank-1 $10E/$10F, and the
	 * bank bit lives in the OAM attribute -- which the original map
	 * ignored, and so drew HUD junk. */
	for (y = 0; y < 8; y++) {
		for (x = 0; x < 16; x += 2) {
			b07[0x30 * 32 + y * 8 + x / 2] = (unsigned char)
				((spx(sb07, 0x10E + (x >= 8), x & 7, y) << 4)
				 | spx(sb07, 0x10E + (x + 1 >= 8),
				       (x + 1) & 7, y));
		}
	}

	for (i = 0; i < sizeof(nes_inject) / sizeof(nes_inject[0]); i++)
		inj_nes(b07, chr, nes_inject[i].ntile, nes_inject[i].slot,
			nes_inject[i].remap);

	for (i = 0; i < 4; i++)
		for (y = 0; y < 8; y++)
			for (x = 0; x < 8; x += 2)
				put8(b07, coin_slot[i], x, y,
				     quad_px(sb07, coin_base[i], x + 4, y),
				     quad_px(sb07, coin_base[i], x + 5, y));

	/* podoboo: the left halves of SMAS's 16x16 lava bubble.  MirrorEnemyGfx
	 * gives the right column as an OAM h-flip and the quads are pixel
	 * symmetric, so two 8x8s reproduce the whole thing.  No remap: the port
	 * already loads pal 4. */
	inj_smas8(b07, sb07, 0x162, 0xFC);
	inj_smas8(b07, sb07, 0x172, 0x49);

	for (i = 0; i < 5; i++)
		inj_quad16(b07, sb07, bowser_base[i], bowser_slot[i],
			   BOWSER_REMAP);
	return 0;
}
