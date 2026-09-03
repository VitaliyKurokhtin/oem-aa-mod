// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Session-lifecycle entry point for the Android Auto audio END-cutoff amp hold.

#ifndef LIBPATCH_BLMJCIAAPA_AUDIO_AUDIO_STOPDELAY_H
#define LIBPATCH_BLMJCIAAPA_AUDIO_AUDIO_STOPDELAY_H

// Defined in audio_stopdelay.cpp. Brings up (once per process) the drain-done SysV semaphore and
// the persistent helper thread that releases the amplifier's AA mix the instant the audio tail
// drains. Idempotent — safe to call on every session bring-up. Called from aap_create_session
// (lifecycle.cpp), gated on aa_audio_low_latency, so the switch is resolved up front (not on the
// first timer_settime) and the interpose's hot path is a bare flag check.
void audio_stopdelay_init(void);

#endif // LIBPATCH_BLMJCIAAPA_AUDIO_AUDIO_STOPDELAY_H
