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
//     then parks in pause() holding the fd. After the open there is no ongoing ALSA activity,
//     so the steady-state fault surface is ~nil.
//
// Gated by libpatch.conf `aa_audio_low_latency` (default false) — the beginning edge of the
// same AA audio-cutoff fix as the start/head (goactive) and end/tail (sem_clockfix +
// audio_stopdelay) halves, all under the one switch. ALSA is reached via dlopen (no build-time
// libasound dependency). The open blocks (a cold open can take seconds), so it MUST run off the
// loader/constructor thread.

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#define LOG_TAG "KEEPALIVE"
#include "../log.h"             // LIBPATCH_NAME=blmjciaapa + common/log.h (LOG*)
#include "common/preload_guard.h" // preload_read_cmdline / preload_is_launcher_process
#include "common/config.h"      // libpatch_config::{load,aa_audio_low_latency}

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

namespace {

struct snd_pcm_t;   // opaque
enum { kStreamPlayback = 0, kFormatS16LE = 2, kAccessRwInterleaved = 3 };
typedef int (*pcm_open_fn)(snd_pcm_t **, const char *, int, int);
typedef int (*pcm_set_params_fn)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);

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
        return nullptr;   // leak the handle; we're failing open anyway
    }
    LOGD("keepalive: holding plug:dmix_48000 open -> hw:0,0 warm (0%% CPU, no streaming); pcm=%p", pcm);
    return pcm;
}

void *keepalive_thread(void *)
{
    open_hold_dmix();          // open once; on failure, fail open (no warming)
    for (;;) pause();          // park forever, holding the fd (near-zero fault surface)
    return nullptr;
}

// Runs at dlopen of libpatch-blmjciaapa.so (jciAAPA start). Spawns the holder on a detached
// thread so the (potentially multi-second) cold open never blocks jciAAPA startup.
__attribute__((constructor))
void audio_keepalive_init()
{
    // This constructor is inherited by EVERY process that gets our LD_PRELOAD, including the
    // short-lived helper children the system spawns roughly once a second. Only run in the real
    // service host (jciAAPA); otherwise we'd parse config, spawn a thread, dlopen libasound and
    // race a dmix open in every throwaway child. Same guard main.cpp uses.
    char cmdline[256];
    preload_read_cmdline(cmdline, sizeof cmdline);
    if (!preload_is_launcher_process(cmdline))
        return;

    libpatch_config::load(reinterpret_cast<const void *>(&audio_keepalive_init));
    if (!libpatch_config::aa_audio_low_latency())
        return;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, keepalive_thread, nullptr) != 0)
        LOGE("keepalive: pthread_create failed — no route keepalive");
    pthread_attr_destroy(&attr);
}

} // namespace
