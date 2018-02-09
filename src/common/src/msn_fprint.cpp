/* (c) Microsoft Corporation.  All rights reserved. */

#include <stdlib.h>
#include "msn_fprint.h"

namespace RSLibImpl
{

#if defined(pdp11) || defined(vax) || defined(__alpha) || defined(i386) || defined(__i386) || defined(__i386__) || defined(_M_IX86) || defined(MIPSEL) || defined(_MSC_VER)
#define MSN_ENDIAN_LITTLE 1
#endif
#if defined(__sparc__) || defined(MIPSEB) || defined(__ppc__)
#define MSN_ENDIAN_LITTLE 0
#endif

#if !defined(MSN_ENDIAN_LITTLE)
static short _endian_little = 1;
#define MSN_ENDIAN_LITTLE (*(char *)&_endian_little)
#endif

#define MSN_ENDIAN_BIG (!MSN_ENDIAN_LITTLE)

#define BYTESWAP_FP(_x) \
    ( \
        ((_x) << 56) | \
        ((_x) >> 56) | \
        (((_x) & 0x0000ff00UL) << 40) | \
        (((_x) >> 40) & 0x0000ff00UL) | \
        (((_x) & 0x00ff0000UL) << 24) | \
        (((_x) >> 24) & 0x00ff0000UL) | \
        (((_x) & 0xff000000UL) << 8) | \
        (((_x) >> 8) & 0xff000000UL) \
    )

static const msn_fprint_t the_poly = (((msn_fprint_t)0xa795d0f2UL) << 32)
     | (msn_fprint_t)0x9b4dcdf8UL;

struct msn_fprint_data_s {
    msn_fprint_t poly[2];          /* poly[0] = 0; poly[1] = polynomial */
    msn_fprint_t empty;           /* fingerprint of the empty string */
    msn_fprint_t bybyte[8][256];  /* bybyte[b][i] is i*X^(64+8*b) mod poly[1] */
    msn_fprint_t bybyte_r[8][256];  /* bybyte[b][i] is i*X^(64+8*b) mod poly[1], byte-swapped */
};

static void initbybyte (msn_fprint_data_t fp,
            msn_fprint_t bybyte[][256],
            msn_fprint_t f) {
    int b;
     for (b = 0; b != 8; b++) {
        int i;
        bybyte[b][0] = 0;
        for (i = 0x80; i != 0; i >>= 1) {
            bybyte[b][i] = f;
            f = fp->poly[f & 1] ^ (f >> 1);
        }
        for (i = 1; i != 256; i <<= 1) {
            msn_fprint_t xf = bybyte[b][i];
            int k;
            for (k = 1; k != i; k++) {
                bybyte[b][i+k] = xf ^ bybyte[b][k];
            }
        }
    }
}

static void msn_fprint_init (msn_fprint_data_t fp, msn_fprint_t poly) {
    int i, j;
    fp->poly[0] = 0;
    fp->poly[1] = poly;    /*This must be initialized early on */
    fp->empty = poly;
    initbybyte (fp, fp->bybyte, poly);
    for (i = 0; i < 8; i++)
      for (j = 0; j < 256; j++)
        fp->bybyte_r[i][j] = BYTESWAP_FP(fp->bybyte[i][j]);
}

msn_fprint_data_t msn_fprint_new (msn_fprint_t poly) {
  msn_fprint_data_t fp = (msn_fprint_data_t) malloc (sizeof (*fp));
  msn_fprint_init(fp, poly);
  return fp;
}

msn_fprint_data_t msn_fprint_new () {
  return msn_fprint_new(the_poly);
}

msn_fprint_t
msn_fprint_of (msn_fprint_data_t fp,
               void *data,
               size_t len ) {

    return msn_fprint_of(fp, fp->empty, data, len);
}

msn_fprint_t
msn_fprint_of (msn_fprint_data_t fp,
               msn_fprint_t init,
               void *data,
               size_t len ) {
    unsigned char *p = (unsigned char *)data;
    unsigned char *e = p+len;
    while (p != e && (((ptrdiff_t) p) & 7L) != 0) {
        init = (init >> 8) ^ fp->bybyte[0][(init & 0xff) ^ *p++];
    }
    if (MSN_ENDIAN_LITTLE) {
        while (p+8 <= e) {
            init ^= *(msn_fprint_t *)p;
            init =  fp->bybyte[7][init & 0xff] ^
                fp->bybyte[6][(init >> 8) & 0xff] ^
                fp->bybyte[5][(init >> 16) & 0xff] ^
                fp->bybyte[4][(init >> 24) & 0xff] ^
                fp->bybyte[3][(init >> 32) & 0xff] ^
                fp->bybyte[2][(init >> 40) & 0xff] ^
                fp->bybyte[1][(init >> 48) & 0xff] ^
                fp->bybyte[0][init >> 56];
            p += 8;
        }
    } else if (p+8 <= e) {
        init = BYTESWAP_FP (init);
        while (p+16 <= e) {
            init ^= *(msn_fprint_t *)p;
            init =  fp->bybyte_r[0][init & 0xff] ^
                fp->bybyte_r[1][(init >> 8) & 0xff] ^
                fp->bybyte_r[2][(init >> 16) & 0xff] ^
                fp->bybyte_r[3][(init >> 24) & 0xff] ^
                fp->bybyte_r[4][(init >> 32) & 0xff] ^
                fp->bybyte_r[5][(init >> 40) & 0xff] ^
                fp->bybyte_r[6][(init >> 48) & 0xff] ^
                fp->bybyte_r[7][init >> 56];
            p += 8;
        }
        init ^= *(msn_fprint_t *)p;
        init =  fp->bybyte[0][init & 0xff] ^
          fp->bybyte[1][(init >> 8) & 0xff] ^
          fp->bybyte[2][(init >> 16) & 0xff] ^
          fp->bybyte[3][(init >> 24) & 0xff] ^
          fp->bybyte[4][(init >> 32) & 0xff] ^
          fp->bybyte[5][(init >> 40) & 0xff] ^
          fp->bybyte[6][(init >> 48) & 0xff] ^
          fp->bybyte[7][init >> 56];
        p += 8;
    }

    while (p != e) {
        init = (init >> 8) ^ fp->bybyte[0][(init & 0xff) ^ *p++];
    }
    return (init);
}
void msn_fprint_destroy (msn_fprint_data_t fp) {
    free (fp);
}

} // namespace RSLibImpl