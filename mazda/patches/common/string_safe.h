// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Shared string-safety helpers for the HUD/transport patches.
// These functions deliberately avoid handing malformed UTF-8 or
// unterminated C strings downstream to the OEM libraries.

#ifndef LIBPATCH_COMMON_STRING_SAFE_H
#define LIBPATCH_COMMON_STRING_SAFE_H

#include <cstddef>
#include <cstring>

namespace libpatch {

inline void copy_utf8_truncated(char *dst, size_t cap,
                                const char *src,
                                size_t src_len = static_cast<size_t>(-1))
{
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    const size_t limit = (cap > 0) ? cap - 1 : 0;
    const size_t avail = (src_len == static_cast<size_t>(-1))
                            ? std::strlen(src)
                            : src_len;
    size_t used = 0;

    while (used < limit && used < avail) {
        const unsigned char ch = static_cast<unsigned char>(src[used]);
        if (ch == '\0') break;

        size_t width = 1;
        if ((ch & 0x80u) != 0x00u) {
            if ((ch & 0xE0u) == 0xC0u) {
                width = 2;
            } else if ((ch & 0xF0u) == 0xE0u) {
                width = 3;
            } else if ((ch & 0xF8u) == 0xF0u) {
                width = 4;
            } else {
                break;  // invalid leading byte: stop before malformed UTF-8
            }
        }

        if (used + width > limit || used + width > avail) {
            break;
        }

        bool valid = true;
        for (size_t i = 1; i < width; ++i) {
            const unsigned char b = static_cast<unsigned char>(src[used + i]);
            if ((b & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
        }
        if (!valid) break;

        used += width;
    }

    std::memcpy(dst, src, used);
    dst[used] = '\0';
}

}  // namespace libpatch

#endif  // LIBPATCH_COMMON_STRING_SAFE_H
