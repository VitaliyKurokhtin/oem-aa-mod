// SPDX-License-Identifier: AGPL-3.0-or-later
//
// sem_clockfix.cpp — fix the end ("tail") of an Android Auto guidance prompt being cut off.
//
// When a guidance clip stops, the audio player pushes end-of-stream and waits on a semaphore
// for the pipeline to drain its buffered tail. That wait has a fixed timeout that is too short:
// the drain can take ~1.2 s but the wait gives up much sooner, so the buffered tail is flushed
// and the last part of the sentence is lost.
//
// We interpose libc sem_timedwait and, only for that one EOS-drain wait, extend the timeout.
// The semaphore is posted the moment the drain actually finishes, so the wait returns then —
// the extension is only a ceiling, never a fixed delay. On success we also post a cross-process
// token (see common/audio/drain_event.h) so blmjciaapa releases the amplifier mix exactly when the
// tail is gone.
//
// The player's stop path has several timed waits; we must extend ONLY the EOS-drain one and
// leave the pipeline-stop waits alone. They are told apart by the low bits of the return address
// (the library is page-aligned, so this offset is stable across loads).
//
// Pure interpose — writes no memory, installs no detour, so none of aap_service's restart risk.
// Pairs with blmjciaapa's audio_stopdelay (holds the amp mix past the drain); the start-side
// head-clip is handled by goactive and the cold-open (beginning) by audio_keepalive — all under
// this same switch. Gated by libpatch.conf `aa_audio_low_latency` (default false). Needs -lrt
// (clock_gettime).

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE          // semtimedop via common/audio/drain_event.h; RTLD_NEXT via common/preload.h
#endif

#define LOG_TAG "SEMFIX"
#include "../log.h"             // LIBPATCH_NAME=aap_service + common/log.h (LOG*)
#include "common/preload.h"     // PRELOAD_EXPORT, resolve_real_symbol()
#include "common/audio/drain_event.h" // drain_event::{open_sem,post} — tell blmjciaapa the tail drained

#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

// Identifies the EOS-drain wait by the low 12 bits of its return address (the library is
// page-aligned, so this offset is stable across loads). This is the only timed wait we extend;
// the pipeline-stop waits in the same function return to different offsets and are left alone.
static const uintptr_t kEosDrainRetPageOff = 0x198;
static const uintptr_t kPageOffMask        = 0xFFF;
// Fail-open ceiling. The wait is event-driven — the semaphore is posted the instant the tail
// finishes draining (~1.2 s), so the wait returns then; this is only a backstop for a
// pathological never-fires case. We only ever extend a too-short wait, never shorten one, so a
// longer timeout set by a future firmware is left untouched.
static const long kEosWaitSec = 3;

// Set once by sem_clockfix_init() (after config load); default off => pass-through when disabled or
// before init. Plain (not atomic): the aap_service constructor runs before any OEM audio thread
// exists, so the write happens-before every reader via thread creation.
static bool g_enabled = false;

// Producer (drain-done) semaphore, opened once in sem_clockfix_init() so the matched EOS-drain path
// does no semget. -1 until opened / if the open fails (drain_event::reset/post tolerate -1).
static int g_psemid = -1;

extern "C" PRELOAD_EXPORT
int sem_timedwait(sem_t *sem, const struct timespec *abstime)
{
    typedef int (*real_fn)(sem_t *, const struct timespec *);
    static void   *handle = nullptr;
    static real_fn real   = reinterpret_cast<real_fn>(resolve_real_symbol(
        "sem_timedwait", "libpthread.so.0", "/lib/libpthread.so.0",
        reinterpret_cast<void *>(&sem_timedwait), &handle));
    if (!real)
        return -1;   // unresolved (shouldn't happen) — behave like a failed wait

    // Gate is resolved in sem_clockfix_init() (constructor time), never here — no config I/O on
    // the libc-wait path. Off => pass-through.
    if (!abstime || !g_enabled)
        return real(sem, abstime);

    const uintptr_t ret     = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uintptr_t ret_off = ret & kPageOffMask;

    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    // Remaining time until the caller's deadline, in realtime terms (what sem_timedwait uses).
    const long rem_ms = (long)(abstime->tv_sec  - rt.tv_sec) * 1000L
                      + (long)(abstime->tv_nsec - rt.tv_nsec) / 1000000L;

    // Verbose per-call trace: confirm the interpose is bound and show each wait's remaining time
    // and call-site offset, so the EOS-drain site stays identifiable in a trace. Compiled out entirely
    // unless the shim is built at verbose level (LOG_LEVEL_VERBOSE) — the level gate is the switch,
    // no manual call counter.
    LOGV("sem_timedwait HIT: rem=%ldms retoff=0x%03lx abstime={%ld,%09ld} caller=%p",
         rem_ms, (unsigned long)ret_off,
         (long)abstime->tv_sec, (long)abstime->tv_nsec, (void *)ret);

    // The EOS-drain wait. Extend its deadline so the pipeline finishes draining and the tail
    // plays instead of being flushed (only ever extend, never shorten); then, the instant it
    // returns success, post a cross-process "tail drained" token so blmjciaapa's audio_stopdelay
    // releases the amp mix exactly then rather than guessing a fixed hold. See common/audio/drain_event.h.
    if (ret_off == kEosDrainRetPageOff) {
        // Clear any stale token here, at drain-START — not at the consumer's arm. The stop
        // reaches this drain before it reaches blmjciaapa's arm, and a short prompt can finish
        // draining (and post its token) inside that gap; resetting at drain-start clears
        // leftovers without ever wiping the token this drain is about to post.
        drain_event::reset(g_psemid);

        const struct timespec *use = abstime;
        struct timespec fixed;
        if (rem_ms < kEosWaitSec * 1000L) {
            fixed.tv_sec  = rt.tv_sec + kEosWaitSec;
            fixed.tv_nsec = rt.tv_nsec;
            use = &fixed;
            LOGD("sem_timedwait: extended EOS-drain wait %ldms -> %lds (audio cutoff fix); caller=%p",
                 rem_ms, kEosWaitSec, (void *)ret);
        }

        const int r = real(sem, use);

        // r == 0 => the semaphore was posted => the tail has drained THIS INSTANT. Signal the
        // amp-hold consumer to un-mix now. r != 0 (ETIMEDOUT at the ceiling / EINTR) => the
        // drain did not complete, so we post nothing and the consumer falls back to its cap.
        if (r == 0) {
            drain_event::post(g_psemid);
            LOGD("sem_timedwait: EOS drain complete -> posted drain-done event "
                 "(amp release, event-driven); caller=%p", (void *)ret);
        }
        return r;
    }
    return real(sem, abstime);
}

namespace aap_service_audio {

// Called once from the aap_service constructor, after config load, to enable the EOS-drain
// extension. Resolving the gate here (not on the first sem_timedwait) keeps config I/O off the
// libc-wait path, where a fault would take the whole process down.
void sem_clockfix_init()
{
    g_enabled = true;
    g_psemid  = drain_event::open_sem();   // producer sem up front, off the wait path
#if LOG_LEVEL <= LOG_LEVEL_VERBOSE
    // Verbose builds only: line-buffer the player's stdout/stderr so its log timestamps aren't
    // batched into clumps. Release stdio is untouched.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
#endif
}

} // namespace aap_service_audio
