/*
 * po.c -- lay out the bootable 800K Apple II ProDOS-protocol image.
 *
 * This is not a second build of the game.  Everything the .po ships is the
 * same bytes the .dsk ships, so the image is laid out FROM a 140K image
 * rather than rebuilt from the blobs -- if the two were assembled
 * independently they would drift, and the failure is invisible until a real
 * drive reads the wrong track.
 *
 * Two things are genuinely different.  ProDOS reads 512-byte blocks, so the
 * port carries a second resident built on rwts_po, and a boot block of its
 * own; and the payload reaches the rwts entry by address, which the two
 * residents put in different places -- hence the caller patches the payload
 * before building the disk this reads.
 *
 * The map is linear and deliberately dumb: logical sector L = track*16 +
 * physical sector goes to block L/2, low half first.  rwts_po turns the
 * resident's existing (track, sector) requests into that arithmetic, so no
 * call site in bootmain had to change.  Tracks 0 and 1 are the 5.25" boot and
 * resident and are never requested here, which is what leaves the first 16
 * blocks free for the boot block and the volume structures.
 */
#include <stdio.h>
#include <string.h>
#include "smb1transpiler.h"
#include "a2vera_blobs.h"

#define BLK             512
#define SEC             256
#define PO_BLOCKS       1600
#define RESID_BLK       512     /* rwts_po expects the resident to start here */
#define FIRST_DATA_TRK  2

static const unsigned char p2l[16] = {
  0, 7, 0xE, 6, 0xD, 5, 0xC, 4, 0xB, 3, 0xA, 2, 9, 1, 8, 0xF
};

static void
put_block (struct po *p, int n, const unsigned char *data, long len)
{
  if (len > BLK)
    len = BLK;
  memset (p->img + (long) n * BLK, 0, BLK);
  memcpy (p->img + (long) n * BLK, data, (size_t) len);
}

static void
mark (unsigned char *bm, int n)
{
  bm[n >> 3] |= (unsigned char) (0x80 >> (n & 7));
}

/*
 * A volume header and a bitmap, purely so the image is recognisable as a
 * ProDOS volume in a file browser.  Nothing in the boot path reads either --
 * the boot block jumps straight at the resident and the resident addresses
 * blocks arithmetically.
 */
static void
put_volume (struct po *p, long resid_len)
{
  unsigned char vd[BLK], bm[BLK];
  int n, nblk = (int) ((resid_len + BLK - 1) / BLK);

  memset (vd, 0, sizeof vd);
  vd[0x04] = 0xF0 | 8;          /* volume dir header, 8-char name */
  memcpy (vd + 0x05, "A2VERA  ", 8);
  vd[0x23] = 0x27;
  vd[0x24] = 0x0D;
  vd[0x27] = 6;                 /* bitmap lives in block 6 */
  vd[0x29] = (unsigned char) (PO_BLOCKS & 0xFF);
  vd[0x2A] = (unsigned char) (PO_BLOCKS >> 8);
  put_block (p, 2, vd, sizeof vd);

  memset (bm, 0, sizeof bm);
  mark (bm, 0);
  mark (bm, 2);
  mark (bm, 6);
  for (n = 0; n < nblk; n++)
    mark (bm, RESID_BLK + n);
  for (n = 0; n < 259; n++)     /* the mapped tracks start here */
    mark (bm, n);
  put_block (p, 6, bm, sizeof bm);
}

int
po_build (struct po *p, const struct disk *d,
          const unsigned char *boot, long boot_len,
          const unsigned char *resid, long resid_len, char *err, size_t errsz)
{
  int trk, sec, last = RESID_BLK + (int) ((resid_len - 1) / BLK);
  long i;

  if (boot_len > BLK)
    {
      snprintf (err, errsz, "the .po boot block is %ld bytes, one block is %d",
                boot_len, BLK);
      return -1;
    }
  if (last >= PO_BLOCKS)
    {
      snprintf (err, errsz, "the .po resident ends at block %d of %d",
                last, PO_BLOCKS);
      return -1;
    }

  memset (p->img, 0, sizeof p->img);
  put_block (p, 0, boot, boot_len);
  for (i = 0; i < resid_len; i += BLK)
    put_block (p, RESID_BLK + (int) (i / BLK), resid + i, resid_len - i);

  for (trk = FIRST_DATA_TRK; trk < 35; trk++)
    for (sec = 0; sec < 16; sec++)
      {
        long l = (long) trk * 16 + sec;
        long src = (long) trk * 16 * SEC + (long) p2l[sec] * SEC;

        memcpy (p->img + (l >> 1) * BLK + (l & 1) * SEC, d->img + src, SEC);
      }

  put_volume (p, resid_len);
  return 0;
}
