/* smb1transpiler -- shared declarations */
#ifndef SMB1TRANSPILER_H
#define SMB1TRANSPILER_H

#include <stddef.h>

#define VERA_TILE_BYTES	32	/* 8x8 4bpp packed, 2 pixels per byte	*/
#define SNES_TILE_BYTES	32	/* 8x8 4bpp planar, bitplanes paired	*/
#define NES_TILE_BYTES	16	/* 8x8 2bpp planar			*/
#define NTILES		768	/* the L0 tileset the port loads	*/
#define DSK_BYTES	(35 * 16 * 256)

/* A loaded dump.  base is nonzero when a .smc carried a 512-byte copier
 * header, so every ROM offset is data + base + off. */
struct rom {
	unsigned char	*data;
	long		size;
	long		base;
	char		path[1024];
};

struct disk {
	unsigned char img[DSK_BYTES];
};

/* sprites.c */
int build_sprite_sheets(const struct rom *smas, const unsigned char *chr,
			unsigned char *b0a, unsigned char *b07);
void build_palette(const struct rom *smas, unsigned char *out);
void build_font(const unsigned char *chr, unsigned char *out);
void build_maps(unsigned char *l0, unsigned char *l1);

/* disk.c */
int disk_build(struct disk *d,
	       const unsigned char *boot1, long boot1_len,
	       const unsigned char *resid, long resid_len,
	       const unsigned char *game, long game_len,
	       const unsigned char *payld, long payld_len,
	       const unsigned char *vram, long vram_len,
	       const unsigned char *lcaud, long lcaud_len,
	       char *err, size_t errsz);

#endif /* SMB1TRANSPILER_H */
