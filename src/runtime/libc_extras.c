/*
 * PShare - The few C library functions FFmpeg, libiconv and libsmb2 ask for
 * that the native title's runtime does not export.
 * Copyright (C) 2026 PShare contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * None of them is on a path PShare uses for real (FFmpeg only calls
 * localtime_r in muxers, libiconv's locale lookup is never asked for), so each
 * is the smallest answer that is still correct:
 *   localtime_r    the broken-down UTC time (a title has no time zone data);
 *   getprotobyname "tcp" and "udp", what libsmb2 looks up;
 *   nl_langinfo    every string the C locale has is ASCII/UTF-8;
 *   ___mb_cur_max  FreeBSD's MB_CUR_MAX in the C locale;
 *   arc4random_buf FFmpeg's random seed. It does resolve, but only through
 *                  libScePosixForWebKit, which a native title never loads:
 *                  here it comes from libSceRandom instead.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

struct protoent
{
    char *p_name;
    char **p_aliases;
    int p_proto;
};

struct tm *localtime_r(const time_t *clock, struct tm *out)
{
    if (clock == NULL || out == NULL)
        return NULL;
    long long seconds = (long long)*clock;
    long long days = seconds / 86400;
    long long rest = seconds % 86400;
    if (rest < 0)
    {
        rest += 86400;
        --days;
    }
    memset(out, 0, sizeof(*out));
    out->tm_hour = (int)(rest / 3600);
    out->tm_min = (int)(rest % 3600 / 60);
    out->tm_sec = (int)(rest % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */

    /* Howard Hinnant's civil_from_days. */
    const long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long day = doy - (153 * mp + 2) / 5 + 1;
    const long long month = mp < 10 ? mp + 3 : mp - 9;
    const long long year = yoe + era * 400 + (month <= 2);
    out->tm_year = (int)(year - 1900);
    out->tm_mon = (int)(month - 1);
    out->tm_mday = (int)day;
    static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    out->tm_yday = before[out->tm_mon] + out->tm_mday - 1 + (leap && out->tm_mon > 1);
    return out;
}

struct protoent *getprotobyname(const char *name)
{
    static char tcp_name[] = "tcp";
    static char udp_name[] = "udp";
    static char *no_aliases[] = {NULL};
    static struct protoent tcp = {tcp_name, no_aliases, 6};
    static struct protoent udp = {udp_name, no_aliases, 17};
    if (name == NULL)
        return NULL;
    if (strcmp(name, "tcp") == 0)
        return &tcp;
    if (strcmp(name, "udp") == 0)
        return &udp;
    return NULL;
}

char *nl_langinfo(int item)
{
    static char codeset[] = "UTF-8";
    static char empty[] = "";
    return item == 0 ? codeset : empty; /* CODESET is 0 */
}

size_t ___mb_cur_max(void)
{
    return 1;
}

int sceRandomGetRandomNumber(void *buffer, size_t size);

void arc4random_buf(void *buffer, size_t size)
{
    unsigned char *out = (unsigned char *)buffer;
    while (size > 0)
    {
        const size_t part = size < 64 ? size : 64; /* the call's limit */
        if (sceRandomGetRandomNumber(out, part) != 0)
        {
            /* Never expected; still never hand back the same bytes twice. */
            static uint64_t state = 0x9e3779b97f4a7c15ull;
            for (size_t i = 0; i < part; ++i)
            {
                state ^= (uint64_t)time(NULL) + (uint64_t)(uintptr_t)out + i;
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                out[i] = (unsigned char)state;
            }
        }
        out += part;
        size -= part;
    }
}

/* ---- Thread stacks --------------------------------------------------------------------------
 * Linked with --wrap=pthread_create (Makefile). A thread created without
 * attributes gets the console's small default stack, which FFmpeg's decoder
 * threads (and curl's resolver) can run past; those get 1 MiB, like the app's
 * own threads (app/thread.hpp). Threads with attributes are left as asked. */

#include <pthread.h>

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*entry)(void *), void *argument);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*entry)(void *), void *argument)
{
    if (attributes != NULL)
        return __real_pthread_create(thread, attributes, entry, argument);
    pthread_attr_t roomy;
    if (pthread_attr_init(&roomy) != 0)
        return __real_pthread_create(thread, NULL, entry, argument);
    (void)pthread_attr_setstacksize(&roomy, 1u << 20);
    const int result = __real_pthread_create(thread, &roomy, entry, argument);
    (void)pthread_attr_destroy(&roomy);
    return result;
}
