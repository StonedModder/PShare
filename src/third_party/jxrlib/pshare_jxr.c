/* PShare - JPEG XR pictures (the PS5's HDR screenshots) as 8-bit sRGB.
 * Copyright (C) 2026 PShare contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PS5 saves an HDR screenshot as 32bppRGB101010: 10-bit HDR10 values, the
 * PQ curve (SMPTE ST 2084) in BT.2020 colours. To show it on an SDR screen:
 * PQ to nits, with 203 nits (BT.2408's reference white) as SDR white; BT.2020
 * to BT.709 primaries; then a shoulder: values up to 0.75 stay as they are and
 * brighter ones approach 1 gradually, so highlights up to about 1600 nits keep
 * some detail instead of clipping (reference white lands near 0.85, an sRGB
 * byte of about 237); then the sRGB curve. The curves are tables, built once. */
#include "jxr_port.h"

#include "pshare_jxr.h"

#include <JXRGlue.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TONE_STEPS 4096 /* linear 0 .. TONE_RANGE in this many steps */
#define TONE_RANGE 8.0  /* 8 x reference white, about 1600 nits: past it, white */

static float g_pq[1024];                  /* code value -> linear, 1.0 = SDR white */
static unsigned char g_tone[TONE_STEPS]; /* linear -> shouldered sRGB byte */
static int g_tables = 0;

static void build_tables(void)
{
    if (g_tables)
        return;
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    for (int code = 0; code < 1024; ++code)
    {
        const double p = pow(code / 1023.0, 1.0 / m2);
        const double n = p - c1 > 0.0 ? p - c1 : 0.0;
        g_pq[code] = (float)(10000.0 * pow(n / (c2 - c3 * p), 1.0 / m1) / 203.0);
    }
    const double knee = 0.75;
    for (int i = 0; i < TONE_STEPS; ++i)
    {
        const double x = i * (TONE_RANGE / (TONE_STEPS - 1));
        const double over = x - knee;
        const double l = x <= knee ? x : knee + (1.0 - knee) * over / (over + 0.4);
        const double s = l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
        g_tone[i] = (unsigned char)(s * 255.0 + 0.5);
    }
    g_tables = 1;
}

/* Linear (1.0 = SDR white) to an sRGB byte, highlights rolled off. */
static unsigned char to_byte(float x)
{
    if (x <= 0.0f)
        return 0;
    const float step = x * (float)((TONE_STEPS - 1) / TONE_RANGE) + 0.5f;
    return step >= TONE_STEPS - 1 ? g_tone[TONE_STEPS - 1] : g_tone[(int)step];
}

int pshare_jxr_is(const unsigned char *bytes, size_t size)
{
    return size >= 4 && bytes[0] == 'I' && bytes[1] == 'I' && bytes[2] == 0xBC;
}

unsigned char *pshare_jxr_decode(const unsigned char *bytes, size_t size, int *width,
                                 int *height)
{
    if (!pshare_jxr_is(bytes, size))
        return NULL;
    struct WMPStream *stream = NULL;
    PKImageDecode *decoder = NULL;
    unsigned char *pixels = NULL;
    PKPixelFormatGUID format;
    I32 w = 0, h = 0;
    if (CreateWS_Memory(&stream, (void *)bytes, size) != WMP_errSuccess ||
        PKImageDecode_Create_WMP(&decoder) != WMP_errSuccess ||
        decoder->Initialize(decoder, stream) != WMP_errSuccess ||
        decoder->GetPixelFormat(decoder, &format) != WMP_errSuccess ||
        decoder->GetSize(decoder, &w, &h) != WMP_errSuccess || w <= 0 || h <= 0 ||
        w > 16384 || h > 16384)
        goto done;

    const int hdr = IsEqualGUID(&format, &GUID_PKPixelFormat32bppRGB101010);
    const int bgr = IsEqualGUID(&format, &GUID_PKPixelFormat24bppBGR) ||
                    IsEqualGUID(&format, &GUID_PKPixelFormat32bppBGR) ||
                    IsEqualGUID(&format, &GUID_PKPixelFormat32bppBGRA);
    int bytes_per_pixel = 0;
    if (hdr || IsEqualGUID(&format, &GUID_PKPixelFormat32bppBGR) ||
        IsEqualGUID(&format, &GUID_PKPixelFormat32bppBGRA) ||
        IsEqualGUID(&format, &GUID_PKPixelFormat32bppRGBA))
        bytes_per_pixel = 4;
    else if (IsEqualGUID(&format, &GUID_PKPixelFormat24bppRGB) ||
             IsEqualGUID(&format, &GUID_PKPixelFormat24bppBGR))
        bytes_per_pixel = 3;
    else
        goto done; /* float and other formats: not what the PS5 writes */

    /* Decoded straight into the RGBA buffer, then converted in place. */
    const size_t count = (size_t)w * (size_t)h;
    pixels = (unsigned char *)malloc(count * 4);
    PKRect all = {0, 0, w, h};
    if (pixels == NULL ||
        decoder->Copy(decoder, &all, pixels, (U32)(w * bytes_per_pixel)) != WMP_errSuccess)
    {
        free(pixels);
        pixels = NULL;
        goto done;
    }
    if (hdr)
    {
        build_tables();
        unsigned int *packed = (unsigned int *)pixels;
        for (size_t i = 0; i < count; ++i)
        {
            const unsigned int v = packed[i];
            const float r = g_pq[(v >> 20) & 1023], g = g_pq[(v >> 10) & 1023], b = g_pq[v & 1023];
            unsigned char *out = pixels + i * 4;
            out[0] = to_byte(1.6605f * r - 0.5876f * g - 0.0728f * b);
            out[1] = to_byte(-0.1246f * r + 1.1329f * g - 0.0083f * b);
            out[2] = to_byte(-0.0182f * r - 0.1006f * g + 1.1187f * b);
            out[3] = 255;
        }
    }
    else
    {
        /* 8-bit: spread 3 bytes to 4 from the end, so nothing is overwritten. */
        for (size_t i = count; i-- > 0;)
        {
            const unsigned char *in = pixels + i * (size_t)bytes_per_pixel;
            const unsigned char c0 = in[0], c1 = in[1], c2 = in[2];
            const unsigned char a = bytes_per_pixel == 4 && !IsEqualGUID(&format, &GUID_PKPixelFormat32bppBGR) ? in[3] : 255;
            unsigned char *out = pixels + i * 4;
            out[0] = bgr ? c2 : c0;
            out[1] = c1;
            out[2] = bgr ? c0 : c2;
            out[3] = a;
        }
    }
    *width = w;
    *height = h;
done:
    /* The decoder closes only a stream it opened; this one is ours. */
    if (decoder != NULL)
        decoder->Release(&decoder);
    if (stream != NULL)
        stream->Close(&stream);
    return pixels;
}
