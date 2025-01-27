/*
 * Copyright 2014-2019 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "util/aeron_platform.h"

#if defined(AERON_COMPILER_MSVC)
#include "aeron_windows.h"

#include <WinSock2.h>
#include <Windows.h>
#include <time.h>
#include <intrin.h>

#include "concurrent/aeron_thread.h"
#include "aeron_alloc.h"

#define __builtin_bswap32 _byteswap_ulong
#define __builtin_bswap64 _byteswap_uint64
#define __builtin_popcount __popcnt
#define __builtin_popcountll __popcnt64

static DWORD dwTlsIndex; // address of shared memory

// DllMain() is the entry-point function for this DLL.

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    LPVOID lpvData;

    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            // Allocate a TLS index.
            if ((dwTlsIndex = TlsAlloc()) == TLS_OUT_OF_INDEXES)
            {
                return FALSE;
            }

            // No break: Initialize the index for first thread.

        case DLL_THREAD_ATTACH:
            // Initialize the TLS index for this thread.
            lpvData = (LPVOID)LocalAlloc(LPTR, 256);
            if (lpvData != NULL)
            {
                TlsSetValue(dwTlsIndex, lpvData);
            }
            break;

        case DLL_THREAD_DETACH:
            // Release the allocated memory for this thread.
            lpvData = TlsGetValue(dwTlsIndex);
            if (lpvData != NULL)
            {
                LocalFree((HLOCAL)lpvData);
            }
            break;

        case DLL_PROCESS_DETACH:
            // Release the allocated memory for this thread.
            lpvData = TlsGetValue(dwTlsIndex);
            if (lpvData != NULL)
            {
                LocalFree((HLOCAL)lpvData);
            }

            // Release the TLS index.
            TlsFree(dwTlsIndex);
            break;

        default:
            break;
    }

    return TRUE;
}

void aeron_micro_sleep(size_t microseconds)
{
    aeron_nano_sleep(1000 * microseconds);
}

typedef struct { UINT64 q[2]; } aeron_uint128_t;

aeron_uint128_t make_aeron_uint128_t(UINT64 x)
{
    aeron_uint128_t result;
    result.q[0] = x;
    result.q[1] = 0;
    return result;
}

aeron_uint128_t aeron_uint128_bitwise_negate(aeron_uint128_t x)
{
    aeron_uint128_t r;
    r.q[0] = ~x.q[0];
    r.q[1] = ~x.q[1];
    return r;
}

BOOL aeron_uint128_equals(const aeron_uint128_t lhs, const aeron_uint128_t rhs)
{
    return lhs.q[0] == rhs.q[0] && lhs.q[1] == rhs.q[1];
}

aeron_uint128_t aeron_uint128_bitwise_shift_left(const aeron_uint128_t lhs, size_t n)
{
    aeron_uint128_t result = lhs;

    if (n >= 128)
    {
        result.q[1] = 0;
        result.q[0] = 0;
    }
    else
    {
        const unsigned int halfsize = 128 / 2;

        if (n >= halfsize)
        {
            n -= halfsize;
            result.q[1] = result.q[0];
            result.q[0] = 0;
        }

        if (n != 0)
        {
            // shift high half
            result.q[1] <<= n;

            const UINT64 mask = (~((UINT64)-1) >> n);

            // and add them to high half
            result.q[1] |= (result.q[0] & mask) >> (halfsize - n);

            // and finally shift also low half
            result.q[0] <<= n;
        }
    }

    return result;
}

aeron_uint128_t aeron_uint128_sub(const aeron_uint128_t lhs, const aeron_uint128_t rhs)
{
    aeron_uint128_t result = lhs;

    result.q[1] -= rhs.q[1];
    result.q[0] -= rhs.q[0];

    if (rhs.q[0] >= lhs.q[0])
    {
        result.q[1] -= 1;
    }

    return result;
}

aeron_uint128_t aeron_uint128_bitwise_and(const aeron_uint128_t lhs, const aeron_uint128_t rhs)
{
    aeron_uint128_t result;
    result.q[0] = lhs.q[0] & rhs.q[0];
    result.q[1] = lhs.q[1] & rhs.q[1];
    return result;
}

aeron_uint128_t aeron_ipv6_netmask_from_prefixlen(size_t prefixlen)
{
    aeron_uint128_t netmask;

    if (0 == prefixlen)
    {
        netmask.q[1] = 0;
        netmask.q[0] = 0;
    }
    else
    {
        netmask = aeron_uint128_bitwise_negate(aeron_uint128_sub(aeron_uint128_bitwise_shift_left(
            make_aeron_uint128_t(1), (128 - prefixlen)), make_aeron_uint128_t(1)));
    }

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    UINT64 lo = netmask.q[0];
    netmask.q[0] = __builtin_bswap64(netmask.q[1]);
    netmask.q[1] = __builtin_bswap64(lo);
#endif

    return netmask;
}

BOOL aeron_ipv6_does_prefix_match(struct in6_addr *in6_addr1, struct in6_addr *in6_addr2, size_t prefixlen)
{
    aeron_uint128_t addr1;
    aeron_uint128_t addr2;
    const aeron_uint128_t netmask = aeron_ipv6_netmask_from_prefixlen(prefixlen);

    memcpy(&addr1, in6_addr1, sizeof(addr1));
    memcpy(&addr2, in6_addr2, sizeof(addr2));

    return aeron_uint128_equals(aeron_uint128_bitwise_and(addr1, netmask), aeron_uint128_bitwise_and(addr2, netmask));
}

void aeron_srand48(UINT64 aeron_nano_clock)
{
    srand((unsigned int)aeron_nano_clock);
}

double aeron_drand48()
{
    return rand();
}

double aeron_erand48(unsigned short xsubi[3])
{
    return rand();
}

void localtime_r(const time_t *timep, struct tm *result)
{
    localtime_s(result, timep);
}

#define MS_PER_SEC      1000ULL     // MS = milliseconds
#define US_PER_MS       1000ULL     // US = microseconds
#define HNS_PER_US      10ULL       // HNS = hundred-nanoseconds (e.g., 1 hns = 100 ns)
#define NS_PER_US       1000ULL

#define HNS_PER_SEC     (MS_PER_SEC * US_PER_MS * HNS_PER_US)
#define NS_PER_HNS      (100ULL)    // NS = nanoseconds
#define NS_PER_SEC      (MS_PER_SEC * US_PER_MS * NS_PER_US)

int aeron_clock_gettime_monotonic(struct timespec *tv)
{
    static LARGE_INTEGER ticksPerSec;
    LARGE_INTEGER ticks;

    if (!ticksPerSec.QuadPart)
    {
        QueryPerformanceFrequency(&ticksPerSec);
        if (!ticksPerSec.QuadPart)
        {
            errno = ENOTSUP;
            return -1;
        }
    }

    QueryPerformanceCounter(&ticks);

    double seconds = (double)ticks.QuadPart / (double)ticksPerSec.QuadPart;
    tv->tv_sec = (time_t)seconds;
    tv->tv_nsec = (long)((ULONGLONG)(seconds * NS_PER_SEC) % NS_PER_SEC);

    return 0;
}

int aeron_clock_gettime_realtime(struct timespec *tv)
{
    FILETIME ft;
    ULARGE_INTEGER hnsTime;

    GetSystemTimeAsFileTime(&ft);

    hnsTime.LowPart = ft.dwLowDateTime;
    hnsTime.HighPart = ft.dwHighDateTime;

    hnsTime.QuadPart -= (11644473600ULL * HNS_PER_SEC);

    tv->tv_nsec = (long)((hnsTime.QuadPart % HNS_PER_SEC) * NS_PER_HNS);
    tv->tv_sec = (long)(hnsTime.QuadPart / HNS_PER_SEC);

    return 0;
}

char *aeron_strndup(const char *value, size_t length)
{
    size_t str_length = strlen(value);
    char *dup = NULL;

    str_length = (str_length > length) ? length : str_length;
    if (aeron_alloc((void **)&dup, str_length + 1) < 0)
    {
        errno = ENOMEM;
        return NULL;
    }

    strncpy(dup, value, str_length);
    dup[str_length] = '\0';
    return dup;
}

#else

typedef int aeron_make_into_non_empty_translation_unit_t;

#endif
