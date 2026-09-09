// SPDX-License-Identifier: AGPL-3.0-or-later
//
// goactive.cpp — start ("head") of an Android Auto nav/guidance prompt underrunning the ALSA
// sink, which garbles the first fraction of the prompt.
//
// The guidance audio pipeline only starts playing once the head unit sends its "go active"
// command, which arrives after the phone has already begun streaming. Until then every incoming
// frame is buffered in the player's small ring. Waiting for that late go-active is itself lost
// head, so we self-activate as soon as a minimal cushion of frames has buffered — this ADVANCES
// the prompt onset versus stock (we start before the head unit tells us to), which is exactly what
// a nav prompt wants: the voice must not start late.
//
// The cushion here is deliberately the SMALLEST that lets the pipeline come up cleanly — its job
// is a fast onset, not riding delivery jitter. Making it deeper would only push the voice onset
// later, which is unacceptable for guidance. The jitter-underrun that garbles some prompts is an
// ALSA sink-buffer problem and is addressed on the sink side (audio.cpp), where cushion can be
// added without moving the onset.
//
// Pure interpose — no memory writes, no detour, so none of aap_service's restart risk. Gated by
// libpatch.conf `aa_audio_low_latency` (the AA low-latency audio switch; this is its start-side half,
// paired with the end-side drain fix). Fail-open: a prompt that never reaches the cushion count
// is still started by the head unit's own go-active, so the worst case is stock timing.

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE          // dladdr() via common/config.h; RTLD_NEXT via common/preload.h
#endif

#define LOG_TAG "GOACTIVE"
#include "../log.h"             // LIBPATCH_NAME=aap_service + common/log.h (LOG*)
#include "common/preload.h"     // PRELOAD_EXPORT, resolve_real_symbol()
#include "common/config.h"      // libpatch_config::{load,aa_audio_low_latency} (opt-in gate)

#include <stdint.h>
#include <time.h>              // clock_gettime — measurement-only per-frame arrival probe (VERBOSE)

namespace {

// Read the player's stream-type field the same way the player does: player -> context
// sub-object (+0x6c) -> stream-type word (+0x10). Only the guidance/nav stream (0xa02) suffers
// the onset underrun, so media and the third stream are intentionally left alone.
const uintptr_t kPlayerCtxOff   = 0x6c;   // player -> context sub-object
const uintptr_t kStreamTypeOff  = 0x10;   // context sub-object -> stream-type word
const unsigned  kStreamGuidance = 0xa02;  // the guidance/nav audio stream

// push_buffer's return value when the frame was buffered because the pipeline is inactive.
const int kRingedRet = -2;

// How many buffered guidance frames to accumulate before self-activating. This is a fast-onset
// knob, NOT a jitter cushion: keep it as small as possible so the voice starts as early as
// possible. Activating on the very first frame can come up before the pipeline is settled;
// three frames is the smallest count that reliably brings the pipeline up, and it is far below
// the player's ring capacity so accumulating it never drops the head of the prompt. Raising it
// would delay the voice onset — which for a nav prompt is a safety regression — so the fix for
// jitter garble lives on the ALSA sink side instead (see audio.cpp), not here.
const int kPrerollFloor = 3;

typedef int  (*push_buffer_fn)(void *player, void *a1, void *data, unsigned int len);
typedef void (*audio_active_fn)(int stream_type);

unsigned read_stream_type(void *player)
{
    if (!player) return 0;
    uintptr_t ctx = *reinterpret_cast<uintptr_t *>(
        reinterpret_cast<char *>(player) + kPlayerCtxOff);
    if (!ctx) return 0;
    return *reinterpret_cast<unsigned *>(ctx + kStreamTypeOff);
}

} // namespace

extern "C" PRELOAD_EXPORT
int gst_media_audio_player_push_buffer(void *player, void *a1, void *data, unsigned int len)
{
    // Resolve the real OEM push once (RTLD_NEXT skips this interpose; aap_service is the main
    // exe and libaap_adplayer.so is a global DT_NEEDED, so RTLD_NEXT sees it directly).
    static void          *h_push = nullptr;
    static push_buffer_fn real   = reinterpret_cast<push_buffer_fn>(resolve_real_symbol(
        "gst_media_audio_player_push_buffer", "libaap_adplayer.so",
        "/usr/lib/libaap_adplayer.so",
        reinterpret_cast<void *>(&gst_media_audio_player_push_buffer), &h_push));
    if (!real)
        return kRingedRet;   // unresolved (shouldn't happen) — mimic the OEM inactive return

    // Opt-in gate: libpatch.conf `aa_audio_low_latency` (default false). Off => pure pass-through.
    static int enabled = -1;
    if (enabled < 0) {
        libpatch_config::load(reinterpret_cast<const void *>(&gst_media_audio_player_push_buffer));
        enabled = libpatch_config::aa_audio_low_latency() ? 1 : 0;
    }

    const int ret = real(player, a1, data, len);
    if (!enabled)
        return ret;

    // Only the guidance stream (0xa02) suffers the onset underrun.
    if (read_stream_type(player) != kStreamGuidance)
        return ret;

#if LOG_LEVEL <= LOG_LEVEL_VERBOSE
    // Measurement-only (compiled out of release): one line per guidance frame so a single capture
    // reveals the head profile without any behaviour change — ret (-2 buffered / 0 accepted marks
    // where playback started), len (chunk size => pre-roll depth in bytes/ms), and dt (inter-frame
    // arrival gap => the delivery jitter that underruns the sink). The tool derives pre-roll ms
    // and the max early gap from this stream.
    {
        static struct timespec probe_prev = { 0, 0 };
        struct timespec probe_now;
        clock_gettime(CLOCK_MONOTONIC, &probe_now);
        long probe_dt = (probe_prev.tv_sec || probe_prev.tv_nsec)
            ? (probe_now.tv_sec - probe_prev.tv_sec) * 1000L
              + (probe_now.tv_nsec - probe_prev.tv_nsec) / 1000000L
            : -1;
        probe_prev = probe_now;
        LOGV("gd frame ret=%d len=%u dt=%ldms", ret, len, probe_dt);
    }
#endif

    // This push runs on the player's single audio worker — the same thread AudioActive runs on —
    // so no lock is needed and calling AudioActive from here is thread-correct (and idempotent
    // anyway: a redundant call is a no-op).
    //   ret == 0  : the frame was accepted => the pipeline is already playing => reset the count
    //               and re-arm for the next prompt's cold start.
    //   ret == -2 : the frame was buffered because the pipeline is inactive. Count it; once
    //               kPrerollFloor frames have accumulated, self-activate so the buffered cushion
    //               (head frame included) drains into the sink and it starts cleanly — earlier
    //               than the head unit's own (late) go-active.
    //
    // `ringed` resets only on ret == 0, so a prompt shorter than the floor that never plays leaves
    // it non-zero; the effect is bounded (the next start may activate a frame or two early) and
    // self-heals on the first accepted frame. Real utterances are dozens of frames, so this is not
    // reachable in practice.
    //
    // Fail-open: the head unit always sends its own go-active after focus is granted, so a prompt
    // that never reaches the floor is still started by that path — worst case is stock timing,
    // never a hang. No timer involved.
    static bool armed  = true;
    static int  ringed = 0;
    if (ret == 0) {
        armed  = true;
        ringed = 0;
        return ret;
    }
    if (ret != kRingedRet || !armed)
        return ret;
    if (++ringed < kPrerollFloor)   // accumulate the minimal cushion before activating
        return ret;
    armed = false;

    static void           *h_aa = nullptr;
    static audio_active_fn  aa   = reinterpret_cast<audio_active_fn>(resolve_real_symbol(
        "AudioActive", "libaap_adplayer.so", "/usr/lib/libaap_adplayer.so",
        nullptr /* not interposed */, &h_aa));
    if (aa) {
        aa(static_cast<int>(kStreamGuidance));
        LOGD("self-activated guidance pipeline after %d-frame pre-roll "
             "(onset start-cutoff fix); go-active race bypassed", kPrerollFloor);
    }
    return ret;
}
