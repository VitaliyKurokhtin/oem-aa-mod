// SPDX-License-Identifier: AGPL-3.0-or-later
//
// svcjcinavi patch — entry point.
//
// This LD_PRELOAD library is injected into the sm_svclauncher PID that
// hosts the OEM navigation service (/jci/navi/svcjcinavi.so). It hosts
// two modules:
//
//   merge.cpp   — interposes the OEM HUD setters so that, while Android
//                 Auto is feeding guidance through the blmjciaapa
//                 svcnavi transport, the OEM nav engine's competing HUD
//                 frames are reconciled with AAP's maneuver instead of
//                 alternating with it (the card-in flicker), and the OEM
//                 speed limit is spliced onto AAP's frames.
//   compass.cpp — interposes the two VBS BCM speed-restriction calls so
//                 the instrument-cluster compass is not gated on road
//                 speed (and not killed outright by disabling the NVRAM
//                 speed restriction).
//
// Both self-gate on their first interposed call — confirming
// svcjcinavi.so is really mapped in this PID — and resolve the real OEM
// implementations lazily, because at constructor time sm_svclauncher has
// not dlopen'd svcjcinavi.so yet. So all the constructor does is install
// the shared crash handler (after the same argv[0]=="sm_svclauncher"
// gate the other patches use), settle the config, and arm the modules
// whose switch is on — leaving a misdeployed library obvious in the log.

#define LOG_TAG "CORE"
#include "log.h"
#include "compass.h"
#include "common/config.h"
#include "common/preload_guard.h"

#include <unistd.h>

namespace {

__attribute__((constructor))
void on_load()
{
    // Gate on argv[0] so the CMU's once-a-second `sh -c killall ...`
    // watchdog children (which also inherit LD_PRELOAD) don't spam
    // logs or install handlers. Silent no-op otherwise.
    char cmdline[256];
    preload_read_cmdline(cmdline, sizeof(cmdline));
    if (!preload_is_launcher_process(cmdline)) {
        return;
    }

    LOGD("loading (svcjcinavi HUD-merge + compass hooks) pid=%d ppid=%d "
         "cmdline=[%s]",
         (int)getpid(), (int)getppid(), cmdline);
    preload_install_fatal_handler();
    LOGD("fatal-signal handler installed (SEGV/BUS/ABRT/FPE/ILL) pid=%d",
         (int)getpid());

    // Settle libpatch.conf now, so the module gates below are decided
    // before svcjcinavi's initializeSettings makes the first interposed
    // call. load() is idempotent, so merge.cpp's own call is a no-op.
    // Wrapped because an exception escaping a library constructor would
    // reach the (non-exception-aware) loader and take the launcher down;
    // on any error we keep the compiled-in defaults.
    try {
        libpatch_config::load(reinterpret_cast<const void *>(&on_load));
    } catch (...) {
        LOGE("config: load threw — keeping defaults (must never escape "
             "the library constructor)");
    }

    if (libpatch_config::compass_always_on()) {
        compass_init();
    } else {
        LOGD("compass_always_on=false -> stock speed-gated compass; "
             "compass module inert");
    }

    LOGD("self-gate + OEM symbol resolution deferred to first call pid=%d",
         (int)getpid());
}

} // namespace
