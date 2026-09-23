/*
 * d64.c -- lay out the bootable 1541 image for the C64 + VERA port.
 *
 * The C64 port is the same game: its payload and APU LUT are byte-identical
 * to the Apple II ones, its VERA upload is the same chunks with a different
 * frame, and the three images that do differ carry a delta table rather than
 * a second copy.  So this file is only a disk format, not a second build.
 *
 * ⚠ This does NOT reproduce the shipped beta4 image byte for byte, and it is
 * not meant to.  That image was written by a dozen rounds of c1541 delete and
 * rewrite, so its sectors record the order those edits happened in -- PAYLD
 * starts at 10/19 and runs backwards into track 9.  There is no layout rule
 * that reproduces it.  What is checked instead is stronger than a checksum of
 * one historical arrangement: every file read back off this image has to match
 * the bytes the port expects, which is what a drive actually sees.
 *
 * Geometry: 35 tracks, 683 sectors of 256 bytes, four speed zones.  Track 18
 * holds the BAM and the directory and carries no file data.  A file is a
 * chain -- the first two bytes of each sector are the next track and sector,
 * or zero and a byte count when it is the last one.
 */
#include <stdio.h>
#include <string.h>
#include "smb1transpiler.h"

#define SEC_BYTES       256
#define DIR_TRK         18
#define INTERLEAVE      10
#define DIR_INTERLEAVE  3
#define PAYLOAD         254     /* a sector carries 254 bytes plus the link */

static int
sectors_per_track (int t)
{
  if (t < 18)
    return 21;
  if (t < 25)
    return 19;
  if (t < 31)
    return 18;
  return 17;
}

static long
sector_off (int t, int s)
{
  long n = 0;
  int i;

  for (i = 1; i < t; i++)
    n += sectors_per_track (i);
  return (n + s) * SEC_BYTES;
}

/*
 * Walk tracks outward from 1 and skip 18, handing out sectors at an
 * interleave so the drive is not waiting a full revolution between links.
 * Any valid chain boots; the interleave is only there for load speed.
 */
static int
next_free (struct d64 *d, int *t, int *s)
{
  int spt, tries;

  while (*t <= 35)
    {
      if (*t == DIR_TRK)
        {
          (*t)++;
          *s = 0;
          continue;
        }
      spt = sectors_per_track (*t);
      for (tries = 0; tries < spt; tries++)
        {
          if (!d->used[*t][*s])
            {
              d->used[*t][*s] = 1;
              return 0;
            }
          *s = (*s + INTERLEAVE) % spt;
          if (*s == 0)
            *s = 1;             /* the interleave closed the cycle */
        }
      (*t)++;
      *s = 0;
    }
  return -1;
}

static void
put_name (unsigned char *dst, const char *name)
{
  int i, end = 0;

  /* A name shorter than the field is padded with $A0.  Stop AT the
     terminator: walking past it reads whatever the linker put next, which is
     another literal, and it lands in the directory.  That is invisible here,
     because the pad byte the terminator becomes still ends the name, but a
     drive lists the rest and two compilers do not produce the same disk. */
  for (i = 0; i < 16; i++)
    {
      if (!end && !name[i])
        end = 1;
      dst[i] = end ? 0xA0 : (unsigned char) name[i];
    }
}

/* the BAM's per-track bitmap: a set bit is a FREE sector */
static void
build_bam (struct d64 *d, const char *title)
{
  unsigned char *b = d->img + sector_off (DIR_TRK, 0);
  int t, s, spt, free;

  memset (b, 0, SEC_BYTES);
  b[0] = DIR_TRK;
  b[1] = 1;
  b[2] = 0x41;                  /* DOS version A */
  for (t = 1; t <= 35; t++)
    {
      spt = sectors_per_track (t);
      free = 0;
      for (s = 0; s < spt; s++)
        if (!d->used[t][s])
          {
            b[4 + (t - 1) * 4 + 1 + (s >> 3)] |=
              (unsigned char) (1 << (s & 7));
            free++;
          }
      b[4 + (t - 1) * 4] = (unsigned char) free;
    }
  put_name (b + 0x90, title);
  b[0xA0] = b[0xA1] = 0xA0;
  b[0xA2] = '6';
  b[0xA3] = '4';
  b[0xA4] = 0xA0;
  b[0xA5] = '2';
  b[0xA6] = 'A';
  b[0xA7] = b[0xA8] = b[0xA9] = b[0xAA] = 0xA0;
}

int
d64_build (struct d64 *d, const struct d64_file *files, int nfiles,
           const char *title, char *err, size_t errsz)
{
  int i, dt = DIR_TRK, ds = 1, dslot = 0, t = 1, s = 0;

  memset (d, 0, sizeof *d);
  memset (d->used, 0, sizeof d->used);
  d->used[DIR_TRK][0] = 1;      /* BAM */
  d->used[DIR_TRK][1] = 1;      /* first directory sector */

  for (i = 0; i < nfiles; i++)
    {
      const unsigned char *p = files[i].data;
      long left = files[i].len;
      int ft = 0, fs = 0, blocks = 0, first = 1;
      int pt = 0, ps = 0;

      do
        {
          long n = left > PAYLOAD ? PAYLOAD : left;
          unsigned char *sec;

          if (next_free (d, &t, &s))
            {
              snprintf (err, errsz, "the disk is full writing %s",
                        files[i].name);
              return -1;
            }
          sec = d->img + sector_off (t, s);
          if (first)
            {
              ft = t;
              fs = s;
              first = 0;
            }
          else
            {
              d->img[sector_off (pt, ps)] = (unsigned char) t;
              d->img[sector_off (pt, ps) + 1] = (unsigned char) s;
            }
          sec[0] = 0;
          sec[1] = (unsigned char) (n + 1);
          memcpy (sec + 2, p, (size_t) n);
          p += n;
          left -= n;
          blocks++;
          pt = t;
          ps = s;
        }
      while (left > 0);

      if (dslot == 8)
        {
          int nt = dt, ns =
            (ds + DIR_INTERLEAVE) % sectors_per_track (DIR_TRK);

          if (ns == 0)
            {
              snprintf (err, errsz, "the directory is full");
              return -1;
            }
          d->used[nt][ns] = 1;
          d->img[sector_off (dt, ds)] = (unsigned char) nt;
          d->img[sector_off (dt, ds) + 1] = (unsigned char) ns;
          dt = nt;
          ds = ns;
          dslot = 0;
        }
      {
        unsigned char *e = d->img + sector_off (dt, ds) + dslot * 32;

        if (dslot == 0)
          {
            e[0] = 0;
            e[1] = 0xFF;
          }
        e[2] = 0x82;            /* closed PRG */
        e[3] = (unsigned char) ft;
        e[4] = (unsigned char) fs;
        put_name (e + 5, files[i].name);
        e[30] = (unsigned char) (blocks & 0xFF);
        e[31] = (unsigned char) (blocks >> 8);
      }
      dslot++;
    }

  build_bam (d, title);
  return 0;
}
