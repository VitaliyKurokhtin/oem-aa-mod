// SPDX-License-Identifier: AGPL-3.0-or-later
//
// audio_keepalive.cpp — keep the shared output device warm so the FIRST AA audio over a tuner
// isn't lost to a cold open (the "beginning cut off").
//
// The AA guidance sink ends at the shared dmix device -> hw:0,0. With a tuner (FM/DAB, analog,
// never a dmix client) in the background that device is closed, so the first AA prompt pays a
// cold open + power-up of several seconds, during which the phone's audio is dropped. A dmix
// client that is simply HELD OPEN keeps the shared slave running (it free-runs from first open
// to last close), so every later AA open is a fast secondary attach.
//
// Design points:
//   * Open once, never close, never reopen. A reopen can collide with the first instance's
//     not-yet-released shared IPC, so we never do it.
//   * No writes. The slave free-runs once opened; an idle open fd holds it warm at ~0% CPU, so
//     there is no write loop and no recovery logic.
//   * We run inside jciAAPA (a restart-critical service), so the surface is minimized: one
//     detached thread does the open once (any error just fails open — no warming, no crash)
//     and exits — the fd is owned by the process, not the thread, so nothing has to hold it.
//     After the open there is no ongoing ALSA activity, so the steady-state fault surface is ~nil.
//
// Gated by libpatch.conf `aa_audio_low_latency` (default false) — the beginning edge of the
// same AA audio-cutoff fix as the start/head (goactive) and end/tail (sem_clockfix +
// audio_stopdelay) halves, all under the one switch. ALSA is reached via dlopen (no build-time
// libasound dependency). The open blocks (a cold open can take seconds), so it runs on its own
// detached thread, spawned by audio_keepalive_init() from lifecycle.cpp's session bracket
// (alongside touch/hud/play-pause) rather than from a library constructor.

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#define LOG_TAG "KEEPALIVE"
#include "../log.h"             // LIBPATCH_NAME=blmjciaapa + common/log.h (LOG*)
#include "audio_keepalive.h"    // audio_keepalive_init() (this TU defines it)
#include "common/thread_util.h" // preload_thread_create (bounded stack)

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

namespace {

struct snd_pcm_t;   // opaque
enum { kStreamPlayback = 0, kFormatS16LE = 2, kAccessRwInterleaved = 3 };
typedef int (*pcm_open_fn)(snd_pcm_t **, const char *, int, int);
typedef int (*pcm_set_params_fn)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
typedef int (*pcm_close_fn)(snd_pcm_t *);

// Open one dmix client and return its (intentionally leaked) handle, or nullptr on any
// failure (fail open — the unit just behaves as stock, no warming). Opened ONCE.
snd_pcm_t *open_hold_dmix()
{
    void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) lib = dlopen("/usr/lib/libasound.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        LOGE("keepalive: dlopen libasound.so.2 failed: %s", dlerror() ? dlerror() : "?");
        return nullptr;
    }
    pcm_open_fn       pcm_open       = reinterpret_cast<pcm_open_fn>(dlsym(lib, "snd_pcm_open"));
    pcm_set_params_fn pcm_set_params = reinterpret_cast<pcm_set_params_fn>(dlsym(lib, "snd_pcm_set_params"));
    // Only needed on the set_params failure path below; not a hard requirement
    // (the success path never closes), so its absence doesn't block warming.
    pcm_close_fn      pcm_close      = reinterpret_cast<pcm_close_fn>(dlsym(lib, "snd_pcm_close"));
    if (!pcm_open || !pcm_set_params) {
        LOGE("keepalive: dlsym snd_pcm_{open,set_params} incomplete");
        return nullptr;
    }

    // `plug:` adapts our S16/48k/2ch to the dmix slave; the dmix instance is shared by IPC key
    // and uid, so (running as root) we join the same instance the guidance sink attaches to.
    snd_pcm_t *pcm = nullptr;
    int err = pcm_open(&pcm, "plug:dmix_48000", kStreamPlayback, 0);
    if (err < 0 || !pcm) {
        LOGE("keepalive: snd_pcm_open(plug:dmix_48000) failed: %d", err);
        return nullptr;
    }
    // set_params prepares the stream, which starts the shared hw:0,0 slave (what stays warm).
    // We never write and never start our own client; it stays prepared while the slave
    // free-runs, holding the device open at ~0% CPU.
    err = pcm_set_params(pcm, kFormatS16LE, kAccessRwInterleaved,
                         /*channels*/ 2, /*rate*/ 48000, /*soft_resample*/ 1,
                         /*latency_us*/ 500000);
    if (err < 0) {
        LOGE("keepalive: snd_pcm_set_params failed: %d", err);
        // Close the just-opened client so a failed attach neither leaks the
        // handle nor holds a slot on the shared dmix IPC. (The success path
        // below deliberately never closes — holding it open IS the mechanism.)
        if (pcm_close) pcm_close(pcm);
        return nullptr;
    }
    LOGD("keepalive: holding plug:dmix_48000 open -> hw:0,0 warm (0%% CPU, no streaming); pcm=%p", pcm);
    return pcm;
}

void *keepalive_thread(void *)
{
    // Open the dmix client once (blocking — a cold open can take seconds, hence its own thread)
    // and return. The pcm fd is owned by the process, not this thread, so it stays open with no
    // thread parked on it. On failure we just fail open.
    open_hold_dmix();
    return nullptr;
}

} // namespace  (open_hold_dmix + keepalive_thread stay TU-private)

// Start the dmix warmer, once per process. Called from lifecycle.cpp's
// aap_create_session bracket (gated on aa_audio_low_latency) — the same place
// touch/hud/play-pause are wired, so the launcher-process detection and the
// config gate live there instead of a duplicated constructor guard here. The
// open can block for seconds (a cold open), so it runs on a bounded, detached
// thread (common/thread_util.h): the 256 KiB stack bound avoids glibc's default
// multi-MB per-thread reservation on the memory-tight CMU, and the detach keeps
// the never-joined fire-and-forget semantics. Idempotent — repeat session edges
// are no-ops.
void audio_keepalive_init(void)
{
    static bool started = false;
    if (started) return;
    started = true;

    pthread_t t;
    int rc = preload_thread_create(&t, keepalive_thread, nullptr);
    if (rc != 0) {
        LOGE("keepalive: thread create failed (%d) — no route keepalive", rc);
        return;
    }
    pthread_detach(t);
}
