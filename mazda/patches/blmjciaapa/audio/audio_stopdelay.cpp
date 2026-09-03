// SPDX-License-Identifier: AGPL-3.0-or-later
//
// audio_stopdelay.cpp — hold the amplifier's AA mix until the audio tail has drained, then
// release it the instant the tail is gone (fixes the end of a guidance sentence being cut).
//
// After a guidance/media stop, the AudioManager arms a short one-shot timer; when it expires it
// abandons audio focus and the amplifier un-mixes AA back to the previous source (e.g. FM). But
// the audio tail can take anywhere from ~0.2 s (warm) to ~2.6 s (a cold first prompt) to drain,
// so a fixed short hold un-mixes before the tail finishes and chops the clip's end at the
// amplifier, not in the pipeline.
//
// A fixed hold can't win: too short cuts the cold case, too long taxes every warm prompt with
// dead-air before the music returns. So we use the drain-done moment as an EVENT:
//   * aap_service (sem_clockfix.cpp) posts a "tail drained" token the instant its EOS-drain
//     wait completes (common/audio/drain_event.h, a SysV semaphore).
//   * Here, on the stop-timer arm, we set that OEM timer to a long fail-safe cap (so it can't
//     un-mix early on its own), hand it to a persistent helper thread, and the helper — as soon
//     as the token arrives — re-arms the same OEM timer to fire immediately. The OEM's own
//     handler then un-mixes right after the tail drained:
//       warm -> un-mix ~at audio-end (no dead-air); cold -> un-mix at ~2.6 s (no cut);
//       nothing posts -> the fail-safe cap fires (= the old worst case, never worse).
//
// Cancellation is free: a new stream cancels the OEM timer and clears its own armed flag.
// Because the helper re-arms the raw timer (never through the OEM arm path, so it never touches
// that flag), a re-arm that races a cancel just makes the OEM handler a no-op. The helper also
// drops any job a newer arm superseded, so it never fires a stale timer. No lock against the OEM
// cancel path — the OEM state machine already serializes it.
//
// Fallback: if the event machinery can't come up (semget or pthread_create fails), degrade to a
// fixed 1.5 s hold, which covers the common warm drain — never worse than the version this
// replaces.
//
// Gated by libpatch.conf `aa_audio_low_latency` (same switch as the aap_service side; the two
// are complementary halves of the end-cutoff fix). Pure libc interpose: no memory writes, no
// detour, so none of jciAAPA's restart risk. Needs -lrt (timer_settime) and -lpthread.

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE          // RTLD_NEXT via common/preload.h;
#endif                         // semtimedop via common/audio/drain_event.h

#define LOG_TAG "STOPDELAY"
#include "../log.h"             // LIBPATCH_NAME=blmjciaapa + common/log.h (LOG*)
#include "common/preload.h"     // PRELOAD_EXPORT, resolve_real_symbol()
#include "common/thread_util.h" // preload_thread_create (bounded-stack helper thread)
#include "common/audio/drain_event.h" // drain_event::{open_sem,wait} — the drain-done event
                                // (the producer owns reset(), at drain-start; see the note below)

#include <atomic>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// Identifies the guidance/media stop-timer arm by the low 12 bits of its return address (the
// library is page-aligned, so this offset is stable across loads). This is the only
// timer_settime call we act on; every other timer passes through untouched.
static const uintptr_t kStopTimerRetPageOff = 0xf28;
static const uintptr_t kPageOffMask         = 0xFFF;
// The stop delay we act on (200 ms, one-shot). The voice-recognition 500 ms delay and every
// other value are left alone.
static const long kGuidanceStopNs = 200000000L;

// Event-driven amp release:
//   kCapSec   — fail-safe: the longest the amp is ever held if no drain-done token arrives
//               (matches the aap_service EOS-wait ceiling). = the old worst case, never worse.
//   kFireNs   — how soon the helper makes the OEM timer expire after the token (must be > 0;
//               a zero it_value would DISARM the timer instead of firing it).
//   kWaitCapSec — helper's own wait timeout, just past kCapSec so it always unblocks and loops.
static const long kCapSec     = 3;
static const long kFireNs     = 1000000L;   // 1 ms
static const long kWaitCapSec = kCapSec + 1;

// Fallback fixed hold, used ONLY if the event machinery can't come up: 1.5 s covers the warm
// drain (0.2-1.7 s), the common case. Same behaviour as before this file went event-driven.
static const long kHeldStopSec = 1;
static const long kHeldStopNs  = 500000000L;

typedef int (*timer_settime_fn)(timer_t, int,
                                const struct itimerspec *, struct itimerspec *);

// The real librt timer_settime, resolved once in the interpose and shared with the helper so
// the helper's re-arm chains straight through (never back into our shim).
static timer_settime_fn g_real_timer_settime = nullptr;

// Hand-off from the interpose (OEM worker thread) to the single persistent helper thread.
static pthread_mutex_t g_lock    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv      = PTHREAD_COND_INITIALIZER;
static bool            g_pending = false;   // a fresh re-arm job is waiting
static unsigned        g_gen     = 0;       // bumped per arm; supersedes an in-flight helper job
static timer_t         g_job_timerid;       // the OEM stop timer of the current job
static int             g_semid   = -1;      // the drain-done SysV semaphore
// Written by audio_stopdelay_init() on the session thread, read by the interpose on OEM worker
// threads that already exist — atomic to avoid a data race. Off => pass-through; a stale read is
// always safe (stock pass-through, or the fixed hold).
static std::atomic<bool> g_event_ready{false};
static std::atomic<bool> g_enabled{false};

// Helper thread: for each arm, wait for the drain-done token then make the OEM stop timer fire
// ~immediately, so the OEM's own handler un-mixes the amp right when the tail drained.
static void *drain_helper(void *)
{
    for (;;) {
        // Take the next job.
        pthread_mutex_lock(&g_lock);
        while (!g_pending)
            pthread_cond_wait(&g_cv, &g_lock);
        timer_t  t       = g_job_timerid;
        unsigned my_gen  = g_gen;
        g_pending        = false;
        pthread_mutex_unlock(&g_lock);

        // Block until aap_service reports the tail drained (or the fail-safe elapses).
        const int got = drain_event::wait(g_semid, kWaitCapSec);

        // If a newer arm superseded this job while we waited, drop it: don't fire a stale
        // timerid. The newest arm's own helper iteration + the OEM 3 s cap cover it. (We
        // deliberately do NOT re-post the consumed token — falling back to the cap is safer
        // than risking an early un-mix from an out-of-order token.)
        pthread_mutex_lock(&g_lock);
        const bool superseded = (g_gen != my_gen);
        pthread_mutex_unlock(&g_lock);
        if (superseded)
            continue;

        if (got == 0 && g_real_timer_settime) {
            // Drain done -> make the OEM stop timer fire now, so the OEM's own handler un-mixes
            // the amp. If a cancel already disarmed it, this just makes that handler a no-op
            // (see the file header). flags=0 => relative.
            struct itimerspec now;
            memset(&now, 0, sizeof(now));
            now.it_value.tv_nsec = kFireNs;
            g_real_timer_settime(t, 0, &now, nullptr);

            LOGD("drain-done event -> released amp hold (re-armed OEM stop timer to %ldms; "
                 "event-driven, no dead-air)", kFireNs / 1000000L);
        } else if (got != 0) {
            LOGW("no drain-done event within %lds -> OEM stop timer fired at the fail-safe "
                 "cap (= old worst case)", kCapSec);
        }
    }
}

extern "C" PRELOAD_EXPORT
int timer_settime(timer_t timerid, int flags,
                  const struct itimerspec *new_value, struct itimerspec *old_value)
{
    static timer_settime_fn real = nullptr;
    if (!real) {
        static void *handle = nullptr;
        real = reinterpret_cast<timer_settime_fn>(resolve_real_symbol(
            "timer_settime", "librt.so.1", "/lib/librt.so.1",
            reinterpret_cast<void *>(&timer_settime), &handle));
        g_real_timer_settime = real;
    }
    if (!real)
        return -1;   // unresolved (shouldn't happen) — behave like a failed arm

    // Gate is resolved in audio_stopdelay_init(), never here — no config I/O on the libc timer
    // path. Off => pass-through.
    const bool enabled = g_enabled.load(std::memory_order_acquire);
    if (!enabled || !new_value)
        return real(timerid, flags, new_value, old_value);

    const uintptr_t ret_off =
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)) & kPageOffMask;

    // Only the guidance/media stop-timer arm: unique call site + the 200 ms one-shot.
    if (ret_off == kStopTimerRetPageOff &&
        new_value->it_value.tv_sec  == 0 &&
        new_value->it_value.tv_nsec == kGuidanceStopNs)
    {
        // Machinery is brought up once in audio_stopdelay_init(); here we only read the flag.
        const bool ready = g_event_ready.load(std::memory_order_acquire);
        if (ready) {
            // Do NOT reset the semaphore here. The producer (sem_clockfix) clears stale tokens
            // at drain-start, which always precedes this arm; a short prompt can finish draining
            // (and post its token) before we get here, and resetting at the arm would wipe the
            // token we're about to wait for. Just hand the job to the helper and wait.
            pthread_mutex_lock(&g_lock);
            g_job_timerid = timerid;
            ++g_gen;
            g_pending = true;
            pthread_cond_signal(&g_cv);
            pthread_mutex_unlock(&g_lock);
        }

        struct itimerspec ext = *new_value;
        if (ready) {
            // Arm to the fail-safe cap; the helper re-arms to ~1 ms on the drain-done event.
            ext.it_value.tv_sec  = kCapSec;
            ext.it_value.tv_nsec = 0;
        } else {
            // Event machinery unavailable — degrade to the fixed hold (warm-drain coverage).
            ext.it_value.tv_sec  = kHeldStopSec;
            ext.it_value.tv_nsec = kHeldStopNs;
        }

        if (ready)
            LOGD("timer_settime: AA stop-delay armed to %lds fail-safe cap; amp un-mixes on "
                 "the drain-done event (end-cutoff fix, event-driven)", kCapSec);
        else
            LOGW("timer_settime: AA stop-delay stretched 200ms -> %ld.%03lds fixed hold "
                 "(event machinery down; end-cutoff fix, fallback)",
                 kHeldStopSec, kHeldStopNs / 1000000L);
        return real(timerid, flags, &ext, old_value);
    }
    return real(timerid, flags, new_value, old_value);
}

// Bring up the drain-done semaphore + persistent helper thread, once. Called from lifecycle.cpp's
// session bracket after config load, so the gate is resolved up front (not on the first
// timer_settime) and the matched arm is a bare flag check. On bring-up failure g_event_ready stays
// false and the interpose falls back to the fixed hold.
void audio_stopdelay_init(void)
{
    static bool started = false;
    if (started)
        return;
    started = true;

    g_enabled.store(true, std::memory_order_release);

    // g_real_timer_settime is resolved (and published) by the interpose on its first call; the
    // helper picks it up through the g_lock handoff, so there is nothing to resolve here.
    g_semid = drain_event::open_sem();
    if (g_semid < 0) {
        LOGE("audio_stopdelay: drain-done semaphore unavailable -> fixed-hold fallback");
        return;                       // g_event_ready stays false
    }

    pthread_t th;
    if (preload_thread_create(&th, &drain_helper, nullptr) != 0) {   // bounded stack
        LOGE("audio_stopdelay: preload_thread_create(drain_helper) failed: errno=%d -> "
             "fixed-hold fallback", errno);
        return;
    }
    pthread_detach(th);
    g_event_ready.store(true, std::memory_order_release);
    LOGD("audio_stopdelay: event-driven amp release armed (drain-done -> un-mix; %lds fail-safe)",
         kCapSec);
}
