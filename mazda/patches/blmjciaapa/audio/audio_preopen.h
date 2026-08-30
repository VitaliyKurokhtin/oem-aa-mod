// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Session-lifecycle entry point for the Android Auto guidance head pre-open.

#ifndef LIBPATCH_BLMJCIAAPA_AUDIO_AUDIO_PREOPEN_H
#define LIBPATCH_BLMJCIAAPA_AUDIO_AUDIO_PREOPEN_H

// Defined in audio_preopen.cpp. Installs (once per process) an entry detour on
// the OEM AudioManager::SendReplyRequestFocus so that the amplifier's guidance
// (NAVI) mix is opened early — at the OEM's own duck-grant, ahead of its later
// stream-start focus request — which is what keeps the head of a guidance
// prompt from being clipped. Safe to call on every session bring-up: the detour
// is a permanent code patch and re-calls are no-ops. Called from
// aap_create_session (lifecycle.cpp) once blmjciaapa.so is confirmed mapped;
// the constructor is too early (the OEM library isn't loaded yet).
void audio_preopen_post_aap_create_session(void);

#endif // LIBPATCH_BLMJCIAAPA_AUDIO_AUDIO_PREOPEN_H
