/*
 * disk.c -- lay out the bootable 140K Apple II 5.25" image.
 *
 * A .dsk is not nibblized: it is 35 tracks x 16 logical sectors x 256 bytes,
 * and the only transform is the DOS 3.3 physical-to-logical skew.  So the
 * whole job is "put this page at (track, physical sector)".  The port's own
 * RWTS reads by physical position with a +2 interleave, which is why every
 * blob is laid down in physical order and the skew applied on the way out.
 *
 * The layout is a contract with resident.asm.  RESERVE_TRK/GAME_TRK/PAY_TRK/
 * VRAM_TRK must equal its constants; LC_TRK and QP_TRK are computed from the
 * VRAM stream's real length, so they move whenever the art does, and the
 * payload was patched with the values in a2vera_blobs.h.  disk_build asserts
 * the computed ones match -- that turns a silent mislayout into a build error.
 */
#include <stdio.h>
#include <string.h>
#include "smb1transpiler.h"
#include "a2vera_blobs.h"

#define SEC_BYTES       256
#define RESERVE_TRK     1
#define GAME_TRK        2
#define PAY_TRK         10
#define VRAM_TRK        12

/* boot1's page count is byte 0 of T0S0, and the IIGS firmware gives up after
 * about 6 sectors, so this has to stay small */
#define LDR_PAGES       4

static const unsigned char p2l[16] = {
  0, 7, 0xE, 6, 0xD, 5, 0xC, 4, 0xB, 3, 0xA, 2, 9, 1, 8, 0xF
};

static void
put_phys (struct disk *d, int track, int phys,
          const unsigned char *page, long n)
{
  long o = (long) track * 16 * SEC_BYTES + (long) p2l[phys] * SEC_BYTES;

  memset (d->img + o, 0, SEC_BYTES);
  if (n > SEC_BYTES)
    n = SEC_BYTES;
  memcpy (d->img + o, page, (size_t) n);
}

/* place len bytes as page-sized sectors from (trk, physical 0), wrapping */
static long
put_run (struct disk *d, int trk, const unsigned char *p, long len)
{
  long i, np = (len + SEC_BYTES - 1) / SEC_BYTES;

  if (np < 1)
    np = 1;
  for (i = 0; i < np; i++)
    put_phys (d, trk + (int) (i / 16), (int) (i % 16),
              p + i * SEC_BYTES, len - i * SEC_BYTES);
  return np;
}

/*
 * The APU divide LUT: QP[p] = floor(300240/(p+1)) & $FFFF, lo half then hi.
 * One table serves the pulse (300240) and the triangle (150120), because
 * 300240 == 2*150120 and floor(floor(2N/d)/2) == floor(N/d); the LC code just
 * shifts right.  The two divisors where that breaks (d in {3,4}, the pulse
 * quotient overflows 16 bits) are excluded by qdivt in the 6502, not here, so
 * assert the property rather than trust it.
 */
static int
build_qdiv (unsigned char *out)
{
  unsigned q[2048];
  int p;

  for (p = 0; p < 2048; p++)
    q[p] = (300240u / (unsigned) (p + 1)) & 0xFFFF;
  for (p = 0; p < 2048; p++)
    {
      out[p] = (unsigned char) (q[p] & 0xFF);
      out[2048 + p] = (unsigned char) (q[p] >> 8);
    }
  if ((q[2] >> 1) == 150120u / 3 || (q[3] >> 1) == 150120u / 4)
    return -1;
  for (p = 0; p < 2048; p++)
    {
      if (p + 1 == 3 || p + 1 == 4)
        continue;
      if ((q[p] >> 1) != ((150120u / (unsigned) (p + 1)) & 0xFFFF))
        return -1;
    }
  return 0;
}

/*
 * vram.dat is u16 nchunks, then per chunk u24 addr, u16 len, data[len].  On
 * the disk that becomes a header sector (u8 nchunks, then per chunk u24 addr,
 * u16 len) followed by each chunk's data, sector-aligned so load_vram can
 * stream a chunk with one ADDR0 setup.  Track 12 sector 0 IS that header:
 * corrupt it and the whole 57K upload is misdirected, which is why DSKERR
 * names the sector.
 */
static long
vram_sectors (const unsigned char *v, long vlen)
{
  long ln, off = 2, sec = 1;    /* +1 for the header sector */
  int i, nch;

  if (vlen < 2)
    return -1;
  nch = v[0] | (v[1] << 8);
  for (i = 0; i < nch; i++)
    {
      if (off + 5 > vlen)
        return -1;
      ln = v[off + 3] | (v[off + 4] << 8);
      off += 5 + ln;
      if (off > vlen)
        return -1;
      sec += (ln + 255) / 256;
    }
  return sec;
}

static long
put_vram (struct disk *d, const unsigned char *v)
{
  unsigned char hdr[SEC_BYTES];
  const unsigned char *data;
  long ln, p, off, sec, h;
  int i, nch;

  nch = v[0] | (v[1] << 8);
  memset (hdr, 0, sizeof (hdr));
  hdr[0] = (unsigned char) nch;
  for (i = 0, h = 1, off = 2; i < nch; i++)
    {
      memcpy (hdr + h, v + off, 5);     /* u24 addr + u16 len, LE */
      h += 5;
      off += 5 + (v[off + 3] | (v[off + 4] << 8));
    }
  put_phys (d, VRAM_TRK, 0, hdr, SEC_BYTES);

  for (i = 0, sec = 1, off = 2; i < nch; i++)
    {
      ln = v[off + 3] | (v[off + 4] << 8);
      data = v + off + 5;
      for (p = 0; p * 256 < ln; p++, sec++)
        put_phys (d, VRAM_TRK + (int) (sec / 16),
                  (int) (sec % 16), data + p * 256, ln - p * 256);
      off += 5 + ln;
    }
  return sec;
}

#define FAIL(...) \
  do { snprintf (err, errsz, __VA_ARGS__); return -1; } while (0)

/*
 * Every blob but the game image is already post-patch: mkboot resolves labels
 * and rewrites operands at build time, so the raw .bin the assembler emits
 * does not match the disk.  These come from its A2VERA_DUMP_BLOBS hook.
 */
int
disk_build (struct disk *d,
            const unsigned char *boot1, long boot1_len,
            const unsigned char *resid, long resid_len,
            const unsigned char *game, long game_len,
            const unsigned char *payld, long payld_len,
            const unsigned char *vram, long vram_len,
            const unsigned char *lcaud, long lcaud_len,
            char *err, size_t errsz)
{
  long rpages, gpages, ppages, lpages, t0r, vsec, got, i;
  unsigned char qp[4096];
  int lc_trk, qp_trk;

  memset (d->img, 0, sizeof (d->img));

  if (boot1_len > LDR_PAGES * 256)
    FAIL ("boot1 is %ld B but the loader is %d pages; the PROM page "
          "count in byte 0 of T0S0 must stay <= 6, because the IIGS "
          "firmware gives up after ~6 sectors", boot1_len, LDR_PAGES);
  if (boot1_len < 1 || boot1[0] < 1 || boot1[0] > 6)
    FAIL ("boot1 byte 0 (the PROM page count) is %d, must be 1..6",
          boot1_len ? boot1[0] : -1);
  if (resid_len > 0x1800)
    FAIL ("resident is %ld B, %+ld over 6144.  $9000+6144 = "
          "$A800 is the payload base, so a bigger resident "
          "overwrites it", resid_len, resid_len - 0x1800);
  if (game_len != 32768)
    FAIL ("game image is %ld B, expected 32768", game_len);

  rpages = (resid_len + 255) / 256;
  gpages = (game_len + 255) / 256;
  ppages = (payld_len + 255) / 256;
  lpages = (lcaud_len + 255) / 256;

  if (LDR_PAGES + rpages > 32)
    FAIL ("loader(%d)+resident(%ld) pages exceed the two tracks "
          "boot1 reads", LDR_PAGES, rpages);
  if (rpages < 16 - LDR_PAGES)
    FAIL ("resident must span into track 1 for this layout");
  if (GAME_TRK + (gpages - 1) / 16 >= PAY_TRK)
    FAIL ("game collides with the payload");
  if (PAY_TRK + (ppages - 1) / 16 >= VRAM_TRK)
    FAIL ("payload collides with the VRAM stream");

  vsec = vram_sectors (vram, vram_len);
  if (vsec < 0)
    FAIL ("vram.dat is malformed (truncated chunk table)");
  lc_trk = VRAM_TRK + (int) ((vsec - 1) / 16) + 1;
  qp_trk = lc_trk + (int) ((lpages - 1) / 16) + 1;
  if (qp_trk >= 35)
    FAIL ("the APU divide LUT lands on track %d, past the 35-track "
          "disk", qp_trk);

  /* these two are also patched into the payload as lcl_trk / lql_trk, so
   * a payload built for the old numbers would stream the audio image off
   * the wrong track and boot into garbage */
  if (lc_trk != BLOB_LC_TRK)
    FAIL ("computed LC audio track %d != %d, the value the shipped "
          "payload carries.  The VRAM stream changed length; "
          "rebuild the blobs", lc_trk, BLOB_LC_TRK);
  if (qp_trk != BLOB_QP_TRK)
    FAIL ("computed APU divide LUT track %d != %d.  Same cause",
          qp_trk, BLOB_QP_TRK);

  /* track 0 is boot1 then as much resident as fits; the rest spills onto
   * track 1 */
  for (i = 0; i < LDR_PAGES; i++)
    put_phys (d, 0, (int) i, boot1 + i * 256, boot1_len - i * 256);
  t0r = 16 - LDR_PAGES;
  if (t0r > rpages)
    t0r = rpages;
  for (i = 0; i < t0r; i++)
    put_phys (d, 0, (int) (LDR_PAGES + i), resid + i * 256,
              resid_len - i * 256);
  if (rpages - t0r > 16)
    FAIL ("resident overflow > 1 track; move GAME_TRK up");
  for (i = t0r; i < rpages; i++)
    put_phys (d, RESERVE_TRK + (int) ((i - t0r) / 16),
              (int) ((i - t0r) % 16), resid + i * 256, resid_len - i * 256);

  put_run (d, GAME_TRK, game, game_len);
  put_run (d, PAY_TRK, payld, payld_len);

  got = put_vram (d, vram);
  if (got != vsec)
    FAIL ("VRAM sector count drifted (%ld written, %ld computed)", got, vsec);
  if (VRAM_TRK + (got - 1) / 16 >= lc_trk)
    FAIL ("VRAM collides with the LC audio track");

  put_run (d, lc_trk, lcaud, lcaud_len);

  if (build_qdiv (qp))
    FAIL ("the APU divide LUT failed its own identity check; see "
          "scratchpad/qdivgate.py");
  put_run (d, qp_trk, qp, sizeof (qp));

  printf ("  boot1 %ldB(%dp) resident %ldB(%ldp -> t0:%ld + t1:%ld) "
          "game %ldB(%ldp) payload %ldB(%ldp)\n",
          boot1_len, LDR_PAGES, resid_len, rpages, t0r, rpages - t0r,
          game_len, gpages, payld_len, ppages);
  printf ("  layout: boot 0 | resid-ovfl %d | game %d-%ld | payload "
          "%d-%ld | VRAM %d-%ld (%ld sectors) | LC audio %d | APU LUT "
          "%d\n", RESERVE_TRK, GAME_TRK, GAME_TRK + (gpages - 1) / 16,
          PAY_TRK, PAY_TRK + (ppages - 1) / 16, VRAM_TRK,
          VRAM_TRK + (got - 1) / 16, got, lc_trk, qp_trk);
  return 0;
}

#undef FAIL
