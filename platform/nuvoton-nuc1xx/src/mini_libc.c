/*
 * mini_libc.c: memcpy/memset for -nostdlib builds. GCC may emit calls to
 * these for struct copies or zeroing. Those calls appear only after LTO
 * has run, so the functions are marked "used" to keep LTO from discarding
 * them early; --gc-sections still drops them from images that never call
 * them.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>

__attribute__((used)) void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

__attribute__((used)) void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}
