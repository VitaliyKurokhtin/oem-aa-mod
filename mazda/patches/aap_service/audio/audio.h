// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef LIBPATCH_AAP_SERVICE_AUDIO_H
#define LIBPATCH_AAP_SERVICE_AUDIO_H

namespace aap_service_audio {

void init();

// Audio EOS-drain "tail" fix (sem_clockfix.cpp). Call once from the constructor after config load,
// so the gate is resolved up front, not on the first sem_timedwait.
void sem_clockfix_init();

} // namespace aap_service_audio

#endif // LIBPATCH_AAP_SERVICE_AUDIO_H
