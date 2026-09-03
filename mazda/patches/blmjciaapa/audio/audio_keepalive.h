// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Session-lifecycle entry point for the Android Auto audio cold-open keepalive.

#ifndef LIBPATCH_BLMJCIAAPA_AUDIO_AUDIO_KEEPALIVE_H
#define LIBPATCH_BLMJCIAAPA_AUDIO_AUDIO_KEEPALIVE_H

// Defined in audio_keepalive.cpp. Starts (once per process) a short detached
// thread that opens the shared dmix client and exits; the fd is process-owned,
// so the client stays open forever and the first AA prompt over a tuner doesn't
// pay the multi-second cold open of the shared hw:0,0 slave. Idempotent — safe
// to call on every session bring-up.
// Called from aap_create_session (lifecycle.cpp), gated on aa_audio_low_latency:
// the launcher-process detection and the config gate live there, alongside
// touch/hud/play-pause, instead of a duplicated constructor guard in the module.
void audio_keepalive_init(void);

#endif // LIBPATCH_BLMJCIAAPA_AUDIO_AUDIO_KEEPALIVE_H
