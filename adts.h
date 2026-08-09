#ifndef ADTS_H
#define ADTS_H

#include <stddef.h>

// ADTS framing helpers.
//
// A live AAC stream is joined at an arbitrary byte offset, so the first bytes
// are usually the tail of a partial frame. NeAACDecInit does not hunt for a
// sync word: handed unaligned data it quietly reports 44100 Hz stereo defaults
// and then fails on every subsequent frame. The stream must therefore be
// advanced to a real frame boundary first.

// An ADTS header starts with a 12-bit sync (0xFFF), then ID (1), a 2-bit layer
// field that is always 00, and the protection_absent bit. MPEG audio uses an
// 11-bit sync with a non-zero layer field, which is what separates the two.
static inline bool adts_is_sync(const unsigned char* p) {
    return p[0] == 0xFF && (p[1] & 0xF6) == 0xF0;
}

// aac_frame_length is 13 bits spanning bytes 3..5 and covers the header too.
static inline size_t adts_frame_length(const unsigned char* p) {
    return ((size_t)(p[3] & 0x03) << 11) | ((size_t)p[4] << 3) | ((size_t)p[5] >> 5);
}

// Finds the first frame boundary, confirming a candidate by checking that the
// frame it declares is followed by another sync word. A bare 0xFF 0xFx pair
// occurs often enough in compressed audio that the confirmation matters.
//
// A candidate whose frame runs past the end of the buffer cannot be confirmed
// either way. Such a candidate is only returned if the whole buffer yields no
// confirmed one, so a false positive early in the buffer can never mask a real
// boundary later in it.
//
// Returns the byte offset, or -1 if there is no candidate at all.
static inline long adts_find_sync(const unsigned char* p, size_t n) {
    if (n < 9) return -1;

    long unconfirmed = -1;

    for (size_t i = 0; i + 8 < n; i++) {
        if (!adts_is_sync(p + i)) continue;

        size_t frame_len = adts_frame_length(p + i);
        if (frame_len < 7) continue;

        size_t next = i + frame_len;
        if (next + 1 < n) {
            if (adts_is_sync(p + next)) return (long)i;
            continue;
        }

        if (unconfirmed < 0) unconfirmed = (long)i;
    }

    return unconfirmed;
}

#endif // ADTS_H
