// SPDX-License-Identifier: AGPL-3.0-or-later
//
// goactive.cpp — fix the start ("head") of an Android Auto nav/guidance prompt being clipped
// when prompts arrive back-to-back.
//
// The guidance audio pipeline only starts playing once the head unit sends its "go active"
// command, which arrives after the phone has already begun streaming. Until then every
// incoming frame is buffered in the player's small ring. If the pipeline is then activated on
// just one buffered frame, the ALSA sink underruns before the next frame arrives and the first
// few milliseconds are lost; when prompts come back-to-back the head unit's go-active is
// delayed further and more of the head is dropped.
//
// We interpose the player's push-buffer entry point (reached for every incoming frame). When a
// guidance frame is buffered because the pipeline is still inactive, we count it; once a few
// frames have accumulated we self-activate the pipeline via the player's own AudioActive(), so
// it starts with a cushion instead of on one frame. The buffered frames (head included) drain
// into the pipeline on activation, and the head unit's later go-active becomes a harmless no-op.
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

namespace {

// Read the player's stream-type field the same way the player does: player -> context
// sub-object (+0x6c) -> stream-type word (+0x10). Only the guidance/nav stream (0xa02) suffers
// the back-to-back head clip, so media and the third stream are intentionally left alone.
const uintptr_t kPlayerCtxOff   = 0x6c;   // player -> context sub-object
const uintptr_t kStreamTypeOff  = 0x10;   // context sub-object -> stream-type word
const unsigned  kStreamGuidance = 0xa02;  // the guidance/nav audio stream

// push_buffer's return value when the frame was buffered because the pipeline is inactive.
const int kRingedRet = -2;

// How many buffered guidance frames to accumulate before self-activating. Activating on the
// first frame starts the pipeline with ~1 frame buffered, and the ALSA sink (start threshold of
// one period, see audio.cpp) then underruns on the phone's early trickle — the first few ms drop
// and garble. Three frames is the smallest cushion that starts cleanly, and it is far below the
// player's ring capacity so accumulating it never drops the head of the prompt.
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

    // Only the guidance stream (0xa02) suffers the dense-prompt start starvation.
    if (read_stream_type(player) != kStreamGuidance)
        return ret;

    // This push runs on the player's single audio worker — the same thread AudioActive runs on —
    // so no lock is needed and calling AudioActive from here is thread-correct (and idempotent
    // anyway: a redundant call is a no-op).
    //   ret == 0  : the frame was accepted => the pipeline is already playing => reset the count
    //               and re-arm for the next prompt's cold start.
    //   ret == -2 : the frame was buffered because the pipeline is inactive. Count it; once
    //               kPrerollFloor frames have accumulated, self-activate so the buffered cushion
    //               (head frame included) drains into the sink and it starts cleanly.
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
    if (++ringed < kPrerollFloor)   // accumulate the cushion before activating
        return ret;
    armed = false;

    static void           *h_aa = nullptr;
    static audio_active_fn  aa   = reinterpret_cast<audio_active_fn>(resolve_real_symbol(
        "AudioActive", "libaap_adplayer.so", "/usr/lib/libaap_adplayer.so",
        nullptr /* not interposed */, &h_aa));
    if (aa) {
        aa(static_cast<int>(kStreamGuidance));
        LOGD("self-activated guidance pipeline after %d-frame pre-roll "
             "(dense-prompt start-cutoff fix); go-active race bypassed", kPrerollFloor);
    }
    return ret;
}
