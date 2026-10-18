/* PShare - JPEG XR pictures (the PS5's HDR screenshots) as 8-bit sRGB.
 * Copyright (C) 2026 PShare contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PSHARE_JXR_H
#define PSHARE_JXR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when the bytes start like a JPEG XR file ("II" 0xBC). */
int pshare_jxr_is(const unsigned char *bytes, size_t size);

/* Decodes a JPEG XR picture to RGBA, 8 bits a channel, sRGB. The PS5's HDR
 * screenshots (10-bit PQ in BT.2020) are tone-mapped to SDR; 8-bit RGB ones
 * are taken as they are. Returns a malloc'd width * height * 4 buffer (free()
 * it), or NULL when the picture cannot be read or is in another format. */
unsigned char *pshare_jxr_decode(const unsigned char *bytes, size_t size, int *width,
                                 int *height);

#ifdef __cplusplus
}
#endif

#endif
