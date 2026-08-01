/*
 * smb1transpiler -- rebuild the a2vera Apple II + VERA port of Super Mario
 * Bros 1 from your own ROMs, with nothing but a C compiler.
 *
 * The port draws SMB1 using Super Mario All-Stars artwork.  None of that may be
 * redistributed, so this repository ships no Nintendo data: what it ships is
 * metadata (where in each ROM the needed blocks live, and how to convert them)
 * plus the port's own 6502 code.  Put both dumps in a folder, run this there,
 * get a bootable .dsk that is byte-identical to the shipped one.
 *
 *	cc -O2 -o smb1transpiler *.c
 *	./smb1transpiler
 *
 * Dumps are found by extension, not by filename: .nes, and .sfc/.smc/.bin.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "smb1transpiler.h"
#include "a2vera_blobs.h"

/* the VERA stream: u16 nchunks, then per chunk u24 addr, u16 len, data[].
 * Order and geometry are a contract with the 6502 load_vram. */
static const struct {
	unsigned long	addr;
	long		len;
} vram_chunks[] = {
	{ 0x00000UL, 0x1800 },	/* L0 tilemap; see fill_vram         */
	{ 0x01000UL, 0x0800 },	/* L1 tilemap				    */
	{ 0x02000UL, 24576 },	/* L0 tileset				    */
	{ 0x14000UL, 8192 },	/* b0A sprites, Mario			    */
	{ 0x16000UL, 8192 },	/* b07 sprites, enemies and objects	    */
	{ 0x1A000UL, 8192 },	/* HUD font				    */
	{ 0x1FA00UL, 512 },	/* palette				    */
};
#define NCHUNKS (sizeof(vram_chunks) / sizeof(vram_chunks[0]))

static int ends_with(const char *s, const char *suf)
{
	size_t i, ls = strlen(s), lu = strlen(suf);
	char a, b;

	if (lu > ls)
		return 0;
	for (i = 0; i < lu; i++) {
		a = s[ls - lu + i];
		b = suf[i];
		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (a != b)
			return 0;
	}
	return 1;
}

static int slurp(const char *path, struct rom *r)
{
	FILE *f = fopen(path, "rb");

	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	r->size = ftell(f);
	fseek(f, 0, SEEK_SET);
	r->data = malloc((size_t)r->size);
	if (!r->data || fread(r->data, 1, (size_t)r->size, f)
			!= (size_t)r->size) {
		free(r->data);
		fclose(f);
		return 0;
	}
	fclose(f);
	snprintf(r->path, sizeof(r->path), "%s", path);
	r->base = 0;
	return 1;
}

static int write_file(const char *dir, const char *name,
		      const unsigned char *p, long n)
{
	char path[1200];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (!f) {
		perror(path);
		return -1;
	}
	fwrite(p, 1, (size_t)n, f);
	fclose(f);
	printf("wrote %s (%ld bytes)\n", path, n);
	return 0;
}

/* iNES: "NES\x1a", byte 4 = 16K PRG banks, byte 5 = 8K CHR banks */
static int is_smb1(const struct rom *r, long *chr_off)
{
	if (r->size < 16 + 32768 + 8192)
		return 0;
	if (memcmp(r->data, "NES\x1a", 4))
		return 0;
	if (r->data[4] != 2 || r->data[5] != 1)
		return 0;
	*chr_off = 16 + 32768L;
	return 1;
}

/* 2 MB LoROM.  A .smc may carry a 512-byte copier header; detect that by the
 * size remainder rather than by trusting the extension.  The header check is
 * structural so that no title string has to be embedded here. */
static int is_smas(struct rom *r)
{
	const unsigned char *h;
	unsigned comp, csum;
	long sz = r->size;

	if (sz % 1024 == 512) {
		r->base = 512;
		sz -= 512;
	}
	if (sz != 2 * 1024 * 1024)
		return 0;
	h = r->data + r->base + 0x7FDC;		/* LoROM $00:FFDC */
	comp = (unsigned)h[0] | ((unsigned)h[1] << 8);
	csum = (unsigned)h[2] | ((unsigned)h[3] << 8);
	return ((comp ^ csum) & 0xFFFF) == 0xFFFF;
}

static unsigned long crc32b(const unsigned char *p, long n)
{
	unsigned long c = 0xFFFFFFFFUL;
	long i;
	int k;

	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320UL
					& (unsigned long)(-(long)(c & 1)));
	}
	return c ^ 0xFFFFFFFFUL;
}

#define CRC_SMB1_PRG	0x5CF548D3UL
#define CRC_SMB1_CHR	0x867B51ADUL
#define CRC_SMAS_B06	0x55A2FDADUL
#define CRC_SMAS_B07	0x317371F2UL
#define CRC_SMAS_B0A	0x1B0F831CUL

/* the disk this tool is expected to produce.  Checking our own output closes
 * the loop: every derivation above is measured against the shipped build, so a
 * mismatch here means one of them drifted, and the message says which half to
 * look at rather than leaving the user with a plausible-looking .dsk. */
#define CRC_DISK	0x8875B7F8UL

/*
 * Structural validation alone is not enough, and the gap is large.  Running
 * every SMB1 dump in the NES TOSEC set (108 of them) through the tool before
 * this check existed: 5 produced the byte-exact disk, 14 failed loudly, 5 were
 * rejected at the door -- and 84 were accepted and silently produced a
 * different, wrong disk.  Hacks, translations and bad dumps all pass "iNES
 * magic + 2 PRG + 1 CHR", and the downstream failure is not an error, it is a
 * disk that boots and misbehaves.
 *
 * So check a CRC of the data rather than of the container.  The good dumps
 * differ only in header padding and trailing junk and share one (PRG, CHR)
 * pair; none of the other 103 shares it.  A CRC is a fingerprint, not content.
 */
static int check_dumps(const struct rom *nes, const struct rom *smas,
		       long chr_off)
{
	unsigned long pc, cc, b6, b7, ba;
	int bad;

	pc = crc32b(nes->data + 16, 32768);
	cc = crc32b(nes->data + chr_off, 8192);
	b6 = crc32b(smas->data + smas->base + 0x30000L, 8192);
	b7 = crc32b(smas->data + smas->base + 0x38000L, 16384);
	ba = crc32b(smas->data + smas->base + 0x50000L, 32768);

	bad = (pc != CRC_SMB1_PRG) + (cc != CRC_SMB1_CHR)
	    + (b6 != CRC_SMAS_B06) + (b7 != CRC_SMAS_B07)
	    + (ba != CRC_SMAS_B0A);
	if (!bad)
		return 0;

	fprintf(stderr,
		"\n*** DUMP FINGERPRINT MISMATCH (%d of 5 blocks) ***\n"
		"  SMB1 PRG %08lX %s | CHR %08lX %s\n"
		"  SMAS b06 %08lX %s | b07 %08lX %s | b0A %08lX %s\n"
		"This is a ROM hack, a translation or a bad dump.  The\n"
		"structural checks pass for those, but the port's relocation\n"
		"tables and tile offsets are measured against the original\n"
		"data, so the build would SUCCEED and hand you a disk that\n"
		"boots and misbehaves.  Measured: 84 of 108 SMB1 dumps in the\n"
		"TOSEC set do exactly that.  Use --force if you meant it.\n",
		bad,
		pc, pc == CRC_SMB1_PRG ? "ok" : "BAD",
		cc, cc == CRC_SMB1_CHR ? "ok" : "BAD",
		b6, b6 == CRC_SMAS_B06 ? "ok" : "BAD",
		b7, b7 == CRC_SMAS_B07 ? "ok" : "BAD",
		ba, ba == CRC_SMAS_B0A ? "ok" : "BAD");
	return -1;
}

static int find_roms(struct rom *nes, struct rom *smas, long *chr_off)
{
	struct dirent *e;
	struct rom r;
	int nes_ext, snes_ext;
	DIR *d = opendir(".");

	if (!d) {
		perror("opendir");
		return -1;
	}
	while ((e = readdir(d))) {
		nes_ext = ends_with(e->d_name, ".nes");
		/* .bin is what the SNES TOSEC set uses; safe to scan now that
		 * the CRC gates acceptance -- a wrong .bin is rejected */
		snes_ext = ends_with(e->d_name, ".sfc")
			|| ends_with(e->d_name, ".smc")
			|| ends_with(e->d_name, ".bin");
		if (!nes_ext && !snes_ext)
			continue;
		if (!slurp(e->d_name, &r))
			continue;
		if (nes_ext && !nes->data && is_smb1(&r, chr_off))
			*nes = r;
		else if (snes_ext && !smas->data && is_smas(&r))
			*smas = r;
		else
			free(r.data);
	}
	closedir(d);

	if (!nes->data)
		fprintf(stderr, "ERROR: no Super Mario Bros .nes here "
				"(need iNES, 32K PRG + 8K CHR)\n");
	if (!smas->data)
		fprintf(stderr, "ERROR: no Super Mario All-Stars .sfc/.smc/"
				".bin here (need 2 MB LoROM)\n");
	return nes->data && smas->data ? 0 : -1;
}

/*
 * The L0 tileset.  SMAS keeps no ready-made 768-tile sheet anywhere in the ROM:
 * it DMAs different banks into different VRAM windows as it runs, so the port's
 * tiles_vera.bin is a snapshot of assembled VRAM and reproducing it means
 * replaying that mapping.  TILERULES has one rule per tile, in four kinds, all
 * of which read the user's ROMs:
 *
 *	0  SMAS offset, straight 4bpp -> 4bpp		757 tiles
 *	1  SMAS offset + a 16-entry index remap		  4  water-level coral
 *	2  NES CHR tile + a 4-entry remap		  5  tree-ledge stem,
 *							     overworld fence
 *	3  SMAS offset masked by a NES tile		  2  battlement
 *
 * The rules are solved, not hand-written: given a target tile and a candidate
 * source the index map is fully determined, so gen_blobs.py searches for a
 * consistent one and hard-errors if a tile has no derivation at all.
 */
static int build_tileset(const struct rom *smas, const struct rom *nes,
			 long chr_off, unsigned char *tiles)
{
	int per_kind[4] = { 0, 0, 0, 0 };
	const unsigned char *in, *mk;
	const struct tilerule *r;
	const unsigned char *rm;
	unsigned char px[8][8];
	unsigned char p0, p1, p2, p3;
	unsigned char *dst;
	int col, row, t, op;
	long o, m;

	for (t = 0; t < NTILES; t++) {
		r = &TILERULES[t];
		rm = TILE_REMAP[r->rmap];
		dst = tiles + (size_t)t * VERA_TILE_BYTES;

		if (r->kind == 2) {
			o = chr_off + (long)r->src * NES_TILE_BYTES;
			if (o + NES_TILE_BYTES > nes->size)
				return -1;
			in = nes->data + o;
			for (row = 0; row < 8; row++)
				for (col = 0; col < 8; col++)
					px[row][col] = rm[
						((in[row] >> (7 - col)) & 1)
						| (((in[row + 8] >> (7 - col))
						    & 1) << 1)];
			goto pack;
		}

		o = smas->base + (long)r->src;
		if (o + SNES_TILE_BYTES > smas->size)
			return -1;
		in = smas->data + o;
		for (row = 0; row < 8; row++) {
			p0 = in[row * 2];
			p1 = in[row * 2 + 1];
			p2 = in[16 + row * 2];
			p3 = in[16 + row * 2 + 1];
			for (col = 0; col < 8; col++)
				px[row][col] = (unsigned char)
					(((p0 >> (7 - col)) & 1)
					 | (((p1 >> (7 - col)) & 1) << 1)
					 | (((p2 >> (7 - col)) & 1) << 2)
					 | (((p3 >> (7 - col)) & 1) << 3));
		}
		if (r->kind == 1) {
			for (row = 0; row < 8; row++)
				for (col = 0; col < 8; col++)
					px[row][col] = rm[px[row][col]];
		}
		if (r->kind == 3) {
			m = chr_off + (long)r->aux * NES_TILE_BYTES;
			if (m + NES_TILE_BYTES > nes->size)
				return -1;
			mk = nes->data + m;
			for (row = 0; row < 8; row++) {
				for (col = 0; col < 8; col++) {
					op = ((mk[row] >> (7 - col)) & 1)
					   | (((mk[row + 8] >> (7 - col)) & 1)
					      << 1);
					if (!op)
						px[row][col] = 0;
				}
			}
		}
pack:
		/* high nibble is the LEFT pixel */
		for (row = 0; row < 8; row++)
			for (col = 0; col < 8; col += 2)
				dst[row * 4 + col / 2] = (unsigned char)
					((px[row][col] << 4)
					 | px[row][col + 1]);
		per_kind[r->kind]++;
	}
	printf("  %d direct, %d re-indexed, %d from NES CHR, %d masked\n",
	       per_kind[0], per_kind[1], per_kind[2], per_kind[3]);
	return 0;
}

/* varint-delta-encoded ascending offsets; kind selects the rewrite rule */
static int varint_apply(const unsigned char *d, int count, unsigned char *img,
			int kind, long imglen)
{
	long off = 0, pos = 0, v;
	unsigned char b;
	int i, sh;

	for (i = 0; i < count; i++) {
		for (v = 0, sh = 0;; sh += 7) {
			b = d[pos++];
			v |= (long)(b & 0x7F) << sh;
			if (!(b & 0x80))
				break;
		}
		off += v;
		if (off >= imglen)
			return -1;
		if (kind == 0) {
			img[off] = (unsigned char)((img[off] - 0x78) & 0xFF);
		} else if (kind == 1) {
			if (img[off] != 0x20)
				return -1;
			img[off] = 0xFE;
		} else {
			if (img[off] != 0x40)
				return -1;
			img[off] = 0xFF;
		}
	}
	return 0;
}

static int apply_pairs(const unsigned char *offs, const unsigned char *vals,
		       int count, unsigned char *img, long imglen)
{
	long off = 0, pos = 0, v;
	unsigned char b;
	int i, sh;

	for (i = 0; i < count; i++) {
		for (v = 0, sh = 0;; sh += 7) {
			b = offs[pos++];
			v |= (long)(b & 0x7F) << sh;
			if (!(b & 0x80))
				break;
		}
		off += v;
		if (off >= imglen)
			return -1;
		img[off] = vals[i];
	}
	return 0;
}

/*
 * The game image: the user's own PRG in three ordered stages.  The port
 * relocates SMB1 by -$7800 to $0800 and moves the PPU/APU register windows into
 * Language Card shadows, so every affected byte is an operand high byte and the
 * transform is positional -- a2vera_blobs.h ships offsets, the bytes come from
 * the ROM.  gen_blobs.py measured them by diffing an original-base assembly
 * against the port's and hard-errors on any byte that fits no rule, so
 * "1465 + 36 + 51, zero unexplained" is a proof rather than a hope.
 *
 * ⚠ The order is load-bearing in both directions, and getting it wrong is
 * nearly invisible.  The source edits are at the ORIGINAL base, so stage 2 must
 * rebase them; doing them afterwards left the vine-snap table pointing 30K off.
 * The hooks are jump targets into our own routines, so they must not be run
 * through the -$78 subtraction; 19 of them deliberately overwrite a byte stage
 * 2 rewrote.
 */
static int build_game(const struct rom *nes, unsigned char *img)
{
	memcpy(img, nes->data + 16, 32768);

	if (apply_pairs(srcedit_off, srcedit_val, SRCEDIT_COUNT, img, 32768))
		return -1;
	if (varint_apply(reloc_deltas, RELOC_COUNT, img, 0, 32768))
		return -2;
	if (varint_apply(ppu_deltas, PPU_COUNT, img, 1, 32768))
		return -3;
	if (varint_apply(apu_deltas, APU_COUNT, img, 2, 32768))
		return -4;
	if (apply_pairs(gamepatch_off, gamepatch_val, GAMEPATCH_COUNT, img,
			32768))
		return -5;
	return 0;
}

/* the empty stream: header plus zeroed chunks, ready to be filled */
static unsigned char *alloc_vram(long *out_len)
{
	unsigned char *v;
	long o, tot = 2;
	unsigned i;

	for (i = 0; i < NCHUNKS; i++)
		tot += 5 + vram_chunks[i].len;
	v = calloc(1, (size_t)tot);
	if (!v)
		return NULL;
	v[0] = (unsigned char)NCHUNKS;
	for (i = 0, o = 2; i < NCHUNKS; i++) {
		v[o] = (unsigned char)(vram_chunks[i].addr & 0xFF);
		v[o + 1] = (unsigned char)((vram_chunks[i].addr >> 8) & 0xFF);
		v[o + 2] = (unsigned char)((vram_chunks[i].addr >> 16) & 0xFF);
		v[o + 3] = (unsigned char)(vram_chunks[i].len & 0xFF);
		v[o + 4] = (unsigned char)((vram_chunks[i].len >> 8) & 0xFF);
		o += 5 + vram_chunks[i].len;
	}
	*out_len = tot;
	return v;
}

/*
 * Fill (or, with --vram, overwrite) every chunk from the ROMs.  Overwriting a
 * reference stream is how each chunk was checked as it landed: a derived chunk
 * that is off by one byte moves the finished disk's md5.
 */
static int fill_vram(unsigned char *v, const struct rom *smas,
		     const struct rom *nes, long chr_off,
		     const unsigned char *tiles)
{
	static unsigned char b0a[8192], b07[8192], mapl1[0x800];
	unsigned char *dat;
	long a, ln, off;
	int i, nch, sub = 0;

	if (build_sprite_sheets(smas, nes->data + chr_off, b0a, b07)) {
		fprintf(stderr, "sprite sheets: a bank offset lies outside "
				"the ROM\n");
		return -1;
	}
	nch = v[0] | (v[1] << 8);
	for (i = 0, off = 2; i < nch; i++) {
		a = v[off] | (v[off + 1] << 8) | ((long)v[off + 2] << 16);
		ln = v[off + 3] | (v[off + 4] << 8);
		dat = v + off + 5;
		off += 5 + ln;

		if (a == 0x02000L && ln == NTILES * VERA_TILE_BYTES)
			memcpy(dat, tiles, (size_t)ln);
		else if (a == 0x14000L && ln == 8192)
			memcpy(dat, b0a, 8192);
		else if (a == 0x16000L && ln == 8192)
			memcpy(dat, b07, 8192);
		else if (a == 0x1A000L && ln == 8192)
			build_font(nes->data + chr_off, dat);
		else if (a == 0x1FA00L && ln == 512)
			build_palette(smas, dat);
		else if (a == 0x00000L && ln == 0x1800) {
			/* ⚠ 0x1800, not 0x1000: VRAM $0000-$17FF spans the L0
			 * map AND the L1 map, so the L1 half rides along here
			 * and is then sent a second time as its own chunk.
			 * Filling only the first 0x1000 leaves the disk wrong
			 * by exactly 2048 bytes. */
			build_maps(dat, mapl1);
			memcpy(dat + 0x1000, mapl1, 0x800);
		} else if (a == 0x01000L && ln == 0x0800)
			memcpy(dat, mapl1, 0x800);
		else
			continue;
		sub++;
	}
	printf("  derived %d of %d VERA chunks from the ROMs\n", sub, nch);
	return sub == nch ? 0 : -1;
}

static int build_disk(const struct rom *nes, const struct rom *smas,
		      long chr_off, const unsigned char *tiles,
		      const char *vram_path, const char *outdir)
{
	static unsigned char game[32768];
	static struct disk dsk;
	unsigned char *vram;
	unsigned long crc;
	long vram_len;
	char err[512];
	struct rom vd;
	int ret;

	ret = build_game(nes, game);
	if (ret) {
		fprintf(stderr, "game image: stage %d failed -- the PRG is not "
				"the expected dump\n", -ret);
		return -1;
	}

	if (vram_path) {
		if (!slurp(vram_path, &vd)) {
			fprintf(stderr, "cannot read %s\n", vram_path);
			return -1;
		}
		vram = vd.data;
		vram_len = vd.size;
	} else {
		vram = alloc_vram(&vram_len);
		if (!vram) {
			fprintf(stderr, "out of memory\n");
			return -1;
		}
	}

	ret = fill_vram(vram, smas, nes, chr_off, tiles);
	if (ret)
		goto err_free_vram;

	printf("\nbuilding the disk image:\n");
	ret = disk_build(&dsk, blob_boot1, sizeof(blob_boot1),
			 blob_resident, sizeof(blob_resident),
			 game, sizeof(game),
			 blob_payload, sizeof(blob_payload),
			 vram, vram_len,
			 blob_lc_audio, sizeof(blob_lc_audio),
			 err, sizeof(err));
	if (ret) {
		fprintf(stderr, "disk layout FAILED: %s\n", err);
		goto err_free_vram;
	}
	ret = write_file(outdir, "smb1_vera.dsk", dsk.img, DSK_BYTES);

	crc = crc32b(dsk.img, DSK_BYTES);
	if (crc == CRC_DISK) {
		printf("  CRC32 %08lX -- matches the shipped disk\n", crc);
	} else {
		printf("  CRC32 %08lX -- does NOT match the shipped disk "
		       "(%08lX)\n", crc, CRC_DISK);
		if (vram_path)
			printf("  (a reference VERA stream was supplied; try "
			       "without --vram)\n");
		ret = 1;
	}

err_free_vram:
	free(vram);
	return ret;
}

/* optional: compare the derived tileset against a reference and report */
static int verify(const unsigned char *tiles, const char *ref_path)
{
	struct rom ref;
	int blk, i, ok, total = 0;

	if (!slurp(ref_path, &ref)) {
		fprintf(stderr, "verify: cannot read %s\n", ref_path);
		return 2;
	}
	if (ref.size != (long)NTILES * VERA_TILE_BYTES) {
		fprintf(stderr, "verify: %s is %ld bytes, expected %d\n",
			ref_path, ref.size, NTILES * VERA_TILE_BYTES);
		free(ref.data);
		return 2;
	}
	printf("\nper-64-tile-block fidelity vs %s:\n", ref_path);
	for (blk = 0; blk < NTILES; blk += 64) {
		for (i = 0, ok = 0; i < 64; i++)
			if (!memcmp(tiles + (size_t)(blk + i) * VERA_TILE_BYTES,
				    ref.data + (size_t)(blk + i)
					     * VERA_TILE_BYTES,
				    VERA_TILE_BYTES))
				ok++;
		total += ok;
		printf("  $%03X-$%03X  %2d/64%s\n", blk, blk + 63, ok,
		       ok == 64 ? "  exact" : "");
	}
	printf("  TOTAL     %d/%d tiles (%.1f%%)\n", total, NTILES,
	       100.0 * total / NTILES);
	free(ref.data);
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--out DIR] [--verify tiles_vera.bin]\n"
		"           [--vram vram.dat] [--force]\n"
		"  scans the CURRENT folder for *.nes and *.sfc/*.smc/*.bin\n"
		"  --verify  report per-block tileset fidelity vs a reference\n"
		"  --vram    overwrite a reference VERA stream instead of\n"
		"            building one (a stricter check on the chunks)\n"
		"  --force   build from a dump whose fingerprint does not "
		"match\n", argv0);
}

int main(int argc, char **argv)
{
	struct rom nes = { NULL, 0, 0, { 0 } };
	struct rom smas = { NULL, 0, 0, { 0 } };
	const char *outdir = "out";
	const char *vram_path = NULL;
	const char *ref = NULL;
	unsigned char *tiles;
	long chr_off = 0;
	int force = 0;
	int i, ret = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--verify") && i + 1 < argc)
			ref = argv[++i];
		else if (!strcmp(argv[i], "--vram") && i + 1 < argc)
			vram_path = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc)
			outdir = argv[++i];
		else if (!strcmp(argv[i], "--force"))
			force = 1;
		else {
			usage(argv[0]);
			return 2;
		}
	}

	if (find_roms(&nes, &smas, &chr_off)) {
		fprintf(stderr, "\nThis repository ships no Nintendo data by "
				"design -- supply your own dumps in this\n"
				"folder and the assets are derived from "
				"them.\n");
		ret = 1;
		goto err_free_roms;
	}
	if (check_dumps(&nes, &smas, chr_off) && !force) {
		ret = 1;
		goto err_free_roms;
	}

	printf("NES  SMB1 : %s  (%ld bytes, CHR at 0x%04lX)\n",
	       nes.path, nes.size, chr_off);
	printf("SNES SMAS : %s  (%ld bytes%s)\n\n", smas.path, smas.size,
	       smas.base ? ", 512-byte copier header skipped" : "");

	tiles = malloc((size_t)NTILES * VERA_TILE_BYTES);
	if (!tiles) {
		fprintf(stderr, "out of memory\n");
		ret = 1;
		goto err_free_roms;
	}
	printf("deriving the L0 tileset (%d tiles) from the ROMs:\n", NTILES);
	if (build_tileset(&smas, &nes, chr_off, tiles)) {
		fprintf(stderr, "a tile rule points outside the ROM\n");
		ret = 1;
		goto err_free_tiles;
	}

	mkdir(outdir, 0777);
	write_file(outdir, "tiles_vera.bin", tiles,
		   (long)NTILES * VERA_TILE_BYTES);
	/* later stages want the CHR verbatim; extract it rather than making
	 * every downstream tool re-parse the iNES header */
	write_file(outdir, "smb1.chr", nes.data + chr_off, 8192);

	if (ref)
		ret = verify(tiles, ref);
	if (build_disk(&nes, &smas, chr_off, tiles, vram_path, outdir))
		ret = 1;

err_free_tiles:
	free(tiles);
err_free_roms:
	free(nes.data);
	free(smas.data);
	return ret;
}
