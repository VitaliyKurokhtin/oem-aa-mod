// SPDX-License-Identifier: AGPL-3.0-or-later
// aap_service patch entry point. Process gating and module startup live here;
// feature implementation belongs in the navi/ and audio/ modules.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define LOG_TAG "CORE"
#include "log.h"
#include "audio/audio.h"
#include "navi/navi.h"
#include "common/config.h"
#include "common/preload_guard.h"

#include <unistd.h>

namespace {

__attribute__((constructor))
void aap_service_patch_init()
{
    if (!preload_in_aap_service())
        return;

    libpatch_config::load(reinterpret_cast<const void *>(&aap_service_patch_init));

    LOGD("loading aap_service modules (pid=%d)", (int)getpid());
    preload_install_fatal_handler();

    if (libpatch_config::aa_audio_low_latency()) {
        aap_service_audio::init();
        aap_service_audio::sem_clockfix_init();
    } else {
        LOGD("aa_audio_low_latency=false -> stock ALSA start threshold; "
             "audio module inert");
    }
    if (libpatch_config::use_protocol_v1_6()) {
        aap_service_navi::init();
    } else {
        LOGD("use_protocol_v1_6=false -> GAL 1.5 (stock); navi module inert");
    }
}

} // namespace
