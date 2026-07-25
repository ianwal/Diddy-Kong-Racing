/**
 * DKR implementation of the N64San "N64 Wrapper" API.
 *
 * n64san (tools/n64san) ships wrapper implementations for OoT and MM only, so this
 * file provides the DKR flavour of the handful of functions the sanitizer runtimes
 * need: formatted output, memcpy/bzero and a termination hook.
 *
 * This file is only ever compiled when SANITIZE is non-empty (see the Makefile) and
 * is deliberately excluded from instrumentation, otherwise reporting a UB event would
 * recurse into the reporting path itself.
 */

#include "libc/string.h"
#include "macros.h"
#include "printf.h"
#include "stdarg.h"
#include "types.h"

#include <PR/R4300.h>
#include <PR/os_libc.h>
#include <PR/rcp.h>
#include <PR/ultratypes.h>

/* N64WRAPPER_DONT_INCLUDE_BUILTIN_ULTRA64 is set on the command line: n64san's own
 * ultra64/libc headers would collide with DKR's, included above. */
#include "n64_wrapper/n64_wrapper.h"

#if !defined(N64SAN_SINK_ISVIEWER) && !defined(N64SAN_SINK_SCREEN)
#define N64SAN_SINK_ISVIEWER 1
#endif

/* Reports are formatted here before being handed to a sink. */
static char sN64SanPrintBuffer[512];

/**
 * Reentrancy guard. A sanitizer check that fires while a report is being emitted
 * (e.g. inside vsprintf, which is itself part of the game) would otherwise recurse
 * until the stack is gone.
 */
static s32 sN64SanReporting = FALSE;

#if N64SAN_SINK_ISVIEWER

/**
 * IS-Viewer 64 debug port. The device lives in cart address space, so the buffer is
 * accessed a word at a time through the PI. Emulators that implement it (gopher64,
 * ares, cen64) will show anything written here in their debug output.
 *
 * The ring buffer logic below is libultra's. libdragon instead treats ISV_PUT as a
 * plain byte count, and emulators resolve the ambiguity by not storing writes to that
 * register: ISV_PUT therefore always reads back as 0, every message starts at the
 * beginning of the buffer, and both readings produce the same output.
 */
#define ISV_PHYS_BASE 0x13FF0000
#define ISV_MAGIC (ISV_PHYS_BASE + 0x00)
#define ISV_GET (ISV_PHYS_BASE + 0x04)
#define ISV_PUT (ISV_PHYS_BASE + 0x14)
#define ISV_DATA (ISV_PHYS_BASE + 0x20)
#define ISV_DATA_SIZE 0xFFE0

#define ISV_MAGIC_IS64 0x49533634 /* 'IS64' */

static void isv_wait(void) {
    while (IO_READ(PI_STATUS_REG) & (PI_STATUS_IO_BUSY | PI_STATUS_DMA_BUSY)) {
        ;
    }
}

static u32 isv_read(u32 physAddr) {
    isv_wait();
    return IO_READ(physAddr);
}

static void isv_write(u32 physAddr, u32 data) {
    isv_wait();
    IO_WRITE(physAddr, data);
}

/**
 * The IS-Viewer buffer survives a reset, so the ring buffer pointers have to be zeroed
 * before the first write or the viewer will read stale garbage.
 */
static void isv_init(void) {
    isv_write(ISV_PUT, 0);
    isv_write(ISV_GET, 0);
    isv_write(ISV_MAGIC, ISV_MAGIC_IS64);
}

static void isv_puts(const char *str, s32 count) {
    s32 get;
    s32 put;
    s32 end;

    if (isv_read(ISV_MAGIC) != ISV_MAGIC_IS64) {
        /* No IS-Viewer present (or not writable): drop the output. */
        return;
    }

    get = isv_read(ISV_GET);
    put = isv_read(ISV_PUT);
    end = put + count;

    /* Bail out rather than overwrite text the viewer hasn't consumed yet. */
    if (end >= ISV_DATA_SIZE) {
        end -= ISV_DATA_SIZE;
        if (get < end || put < get) {
            return;
        }
    } else {
        if (put < get && get < end) {
            return;
        }
    }

    while (count > 0) {
        u32 addr = ISV_DATA + (put & ~3);
        s32 shift = (3 - (put & 3)) * 8;
        u32 data;

        if (*str != '\0') {
            data = isv_read(addr);
            isv_write(addr, ((u32) (u8) *str << shift) | (data & ~(0xFF << shift)));

            put++;
            if (put >= ISV_DATA_SIZE) {
                put -= ISV_DATA_SIZE;
            }
        }
        count--;
        str++;
    }

    isv_write(ISV_PUT, put);
}

#endif /* N64SAN_SINK_ISVIEWER */

static void n64san_init(void) {
    static s32 isInitialized = FALSE;

    if (!isInitialized) {
        isInitialized = TRUE;
#if N64SAN_SINK_ISVIEWER
        isv_init();
#endif
    }
}

static void n64san_puts(const char *str, s32 count) {
    n64san_init();

#if N64SAN_SINK_ISVIEWER
    isv_puts(str, count);
#endif
#if N64SAN_SINK_SCREEN
    /**
     * The on-screen debug text buffer is flushed once a frame by debug_text_print and
     * only holds 0x800 bytes, so this sink only really works for the first few reports.
     */
    render_printf("%s", str);
#endif
}

int N64Wrapper_Vprintf(const char *fmt, va_list args) {
    s32 written;

    if (sN64SanReporting) {
        return 0;
    }
    sN64SanReporting = TRUE;

    written = vsprintf(sN64SanPrintBuffer, fmt, args);
    if (written > 0) {
        n64san_puts(sN64SanPrintBuffer, written);
    }

    sN64SanReporting = FALSE;

    return written;
}

int N64Wrapper_Printf(const char *fmt, ...) {
    int ret;
    va_list args;

    va_start(args, fmt);
    ret = N64Wrapper_Vprintf(fmt, args);
    va_end(args);

    return ret;
}

s32 N64Wrapper_Vsprintf(char *dst, const char *fmt, va_list args) {
    return vsprintf(dst, fmt, args);
}

s32 N64Wrapper_Sprintf(char *dst, const char *fmt, ...) {
    s32 ret;
    va_list args;

    va_start(args, fmt);
    ret = N64Wrapper_Vsprintf(dst, fmt, args);
    va_end(args);

    return ret;
}

void *N64Wrapper_Memcpy(void *dst, const void *src, size_t n) {
    return memcpy(dst, src, n);
}

void N64Wrapper_Bzero(void *dst, size_t length) {
    bzero(dst, length);
}

/**
 * Only reached from the runtime's own Die()/CheckFailed() paths, never from a plain UB
 * report (those are recoverable and let the game carry on).
 *
 * DKR's __assert is compiled out in _FINALROM builds, so print the failure ourselves
 * and then hang: a frozen frame with the message in the debug log is far easier to
 * pin down than a silent return into a runtime that just failed its own invariants.
 */
void NORETURN N64Wrapper_Assert(const char *exp, const char *file, s32 line) {
    /* Bypass the reentrancy guard: this is terminal, we want the message out. */
    sN64SanReporting = FALSE;
    N64Wrapper_Printf("N64San: assertion failed: %s, file %s, line %d\n", exp, file, line);

    for (;;) {
        ;
    }
}
