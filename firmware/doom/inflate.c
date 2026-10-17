// SPDX-License-Identifier: GPL-2.0-or-later
//
// Raw deflate (RFC 1951) decoder, table driven: a code of up to ROOT bits
// is one lookup; a longer one goes through a second-level table hanging
// off its first ROOT bits. Table entries are 16 bits:
//   symbol   bit 15 = 0, bits 14..11 code length (0: no such code), 10..0 symbol
//   link     bit 15 = 1, bits 14..11 bits of the subtable, 10..0 its index
// The tables live in static RAM (6 KiB); nothing is allocated.

#include <stdint.h>

#include "doomtype.h"
#include "inflate.h"

#define LROOT 10
#define DROOT 8
#define LSIZE 2048
#define DSIZE 1024

static uint16_t ltab[LSIZE] DOOM_LOWRAM, dtab[DSIZE] DOOM_LOWRAM;

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

typedef struct
{
    const uint8_t *in, *end;
    uint32_t bits;
    int nbits;
    int overrun;            // zero bytes fed past the end
} bitstream_t;

#define INLINE static inline __attribute__((always_inline))

INLINE void refill(bitstream_t *s)
{
    while (s->nbits <= 24)
    {
        uint32_t b = 0;

        if (s->in < s->end)
            b = *s->in++;
        else
            s->overrun++;
        s->bits |= b << s->nbits;
        s->nbits += 8;
    }
}

INLINE uint32_t getbits(bitstream_t *s, int n)
{
    uint32_t v;

    if (n == 0)
        return 0;
    refill(s);
    v = s->bits & ((1u << n) - 1u);
    s->bits >>= n;
    s->nbits -= n;
    return v;
}

static uint32_t reverse(uint32_t code, int len)
{
    uint32_t r = 0;

    while (len--)
    {
        r = (r << 1) | (code & 1u);
        code >>= 1;
    }
    return r;
}

// Build a decoding table for n symbols with the given code lengths.
// Returns 0, or -1 for an over-subscribed set or one that does not fit.
// (An incomplete set is allowed: its unused entries decode as errors.)
static int build(uint16_t *tab, int size, int root, const uint8_t *lens, int n)
{
    uint16_t count[16], next[16], nx[16];
    uint8_t sub[1 << LROOT];
    int sym, len, i, used, left;
    uint32_t code;

    for (len = 0; len < 16; len++)
        count[len] = 0;
    for (sym = 0; sym < n; sym++)
        count[lens[sym]]++;
    count[0] = 0;
    left = 1;
    for (len = 1; len < 16; len++)
    {
        left <<= 1;
        left -= count[len];
        if (left < 0)
            return -1;
    }
    // canonical codes: the first code of each length
    code = 0;
    for (len = 1; len < 16; len++)
    {
        code = (code + count[len - 1]) << 1;
        next[len] = nx[len] = (uint16_t) code;
    }
    for (i = 0; i < (1 << root); i++)
    {
        tab[i] = 0;
        sub[i] = 0;
    }
    // the longest code under each root prefix sizes that prefix's subtable
    for (sym = 0; sym < n; sym++)
    {
        len = lens[sym];
        if (len > root)
        {
            int p = (int) (reverse(nx[len], len) & ((1u << root) - 1u));

            if (len - root > sub[p])
                sub[p] = (uint8_t) (len - root);
        }
        if (len)
            nx[len]++;
    }
    used = 1 << root;
    for (i = 0; i < (1 << root); i++)
    {
        if (sub[i])
        {
            if (used + (1 << sub[i]) > size)
                return -1;
            tab[i] = (uint16_t) (0x8000u | ((unsigned) sub[i] << 11) | (unsigned) used);
            for (left = 0; left < (1 << sub[i]); left++)
                tab[used + left] = 0;
            used += 1 << sub[i];
        }
    }
    for (sym = 0; sym < n; sym++)
    {
        uint32_t r;

        len = lens[sym];
        if (!len)
            continue;
        r = reverse(next[len]++, len);
        if (len <= root)
        {
            for (i = (int) r; i < (1 << root); i += 1 << len)
                tab[i] = (uint16_t) ((len << 11) | sym);
        }
        else
        {
            uint16_t link = tab[r & ((1u << root) - 1u)];
            int sbits = (link >> 11) & 15;
            uint16_t *st = tab + (link & 0x7FFu);
            int slen = len - root;

            for (i = (int) (r >> root); i < (1 << sbits); i += 1 << slen)
                st[i] = (uint16_t) ((slen << 11) | sym);
        }
    }
    return 0;
}

// Next symbol, or -1 for a code the table does not have
INLINE int decode(bitstream_t *s, const uint16_t *tab, int root)
{
    uint16_t e;
    int len;

    refill(s);
    e = tab[s->bits & ((1u << root) - 1u)];
    if (e & 0x8000u)
    {
        int sbits = (e >> 11) & 15;

        s->bits >>= root;
        s->nbits -= root;
        e = tab[(e & 0x7FFu) + (s->bits & ((1u << sbits) - 1u))];
    }
    len = (e >> 11) & 15;
    if (!len)
        return -1;
    s->bits >>= len;
    s->nbits -= len;
    return e & 0x7FF;
}

static int dynamic_tables(bitstream_t *s)
{
    uint8_t lens[288 + 32], clens[19];
    int hlit, hdist, hclen, i, n;

    hlit = (int) getbits(s, 5) + 257;
    hdist = (int) getbits(s, 5) + 1;
    hclen = (int) getbits(s, 4) + 4;
    if (hlit > 286 || hdist > 30)
        return -1;
    for (i = 0; i < 19; i++)
        clens[i] = 0;
    for (i = 0; i < hclen; i++)
        clens[clen_order[i]] = (uint8_t) getbits(s, 3);
    if (build(dtab, DSIZE, 7, clens, 19))
        return -1;
    n = 0;
    while (n < hlit + hdist)
    {
        int sym = decode(s, dtab, 7);
        int rep, val;

        if (sym < 0)
            return -1;
        if (sym < 16)
        {
            lens[n++] = (uint8_t) sym;
            continue;
        }
        if (sym == 16)
        {
            if (n == 0)
                return -1;
            val = lens[n - 1];
            rep = 3 + (int) getbits(s, 2);
        }
        else if (sym == 17)
        {
            val = 0;
            rep = 3 + (int) getbits(s, 3);
        }
        else
        {
            val = 0;
            rep = 11 + (int) getbits(s, 7);
        }
        if (n + rep > hlit + hdist)
            return -1;
        while (rep--)
            lens[n++] = (uint8_t) val;
    }
    if (lens[256] == 0)
        return -1;
    if (build(ltab, LSIZE, LROOT, lens, hlit))
        return -1;
    if (build(dtab, DSIZE, DROOT, lens + hlit, hdist))
        return -1;
    return 0;
}

static void fixed_tables(void)
{
    uint8_t lens[288];
    int i;

    for (i = 0; i < 144; i++)
        lens[i] = 8;
    for (; i < 256; i++)
        lens[i] = 9;
    for (; i < 280; i++)
        lens[i] = 7;
    for (; i < 288; i++)
        lens[i] = 8;
    build(ltab, LSIZE, LROOT, lens, 288);
    for (i = 0; i < 30; i++)
        lens[i] = 5;
    build(dtab, DSIZE, DROOT, lens, 30);
}

long inflate_raw(void *dst, unsigned long dstlen, const void *src, unsigned long srclen)
{
    bitstream_t s;
    uint8_t *out = dst;
    unsigned long o = 0;
    int last;

    s.in = src;
    s.end = s.in + srclen;
    s.bits = 0;
    s.nbits = 0;
    s.overrun = 0;
    do
    {
        uint32_t type;

        last = (int) getbits(&s, 1);
        type = getbits(&s, 2);
        if (type == 0)
        {
            uint32_t len, nlen;

            // stored: back up to the byte boundary, the bit buffer may
            // hold whole bytes read ahead
            s.bits >>= s.nbits & 7;
            s.nbits -= s.nbits & 7;
            len = getbits(&s, 16);
            nlen = getbits(&s, 16);
            if ((len ^ 0xFFFFu) != nlen || o + len > dstlen)
                return -1;
            while (len && s.nbits)
            {
                out[o++] = (uint8_t) getbits(&s, 8);
                len--;
            }
            if ((unsigned long) (s.end - s.in) < len)
                return -1;
            while (len--)
                out[o++] = *s.in++;
            continue;
        }
        if (type == 1)
            fixed_tables();
        else if (type != 2 || dynamic_tables(&s))
            return -1;
        for (;;)
        {
            int sym = decode(&s, ltab, LROOT);
            uint32_t len, dist;

            if (sym < 256)
            {
                if (sym < 0 || o >= dstlen)
                    return -1;
                out[o++] = (uint8_t) sym;
                continue;
            }
            if (sym == 256)
                break;
            sym -= 257;
            if (sym >= 29)
                return -1;
            len = len_base[sym] + getbits(&s, len_extra[sym]);
            sym = decode(&s, dtab, DROOT);
            if (sym < 0 || sym >= 30)
                return -1;
            dist = dist_base[sym] + getbits(&s, dist_extra[sym]);
            if (dist > o || o + len > dstlen)
                return -1;
            {
                uint8_t *d = out + o;
                const uint8_t *f = d - dist;

                o += len;
                while (len--)
                    *d++ = *f++;
            }
        }
        if (s.overrun > 4)
            return -1;
    } while (!last);
    return (long) o;
}
