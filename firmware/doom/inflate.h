// SPDX-License-Identifier: GPL-2.0-or-later
// Raw deflate (RFC 1951) decoder for the packed lumps of a DPK image.

#ifndef __INFLATE__
#define __INFLATE__

// Unpack the raw deflate stream src[0..srclen) into dst[0..dstlen).
// Returns the number of bytes written, or -1 when the stream is broken
// or does not fit.
long inflate_raw(void *dst, unsigned long dstlen, const void *src, unsigned long srclen);

#endif
