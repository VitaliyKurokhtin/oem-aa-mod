// SPDX-License-Identifier: AGPL-3.0-or-later
//
// svcjcimmui patch — entry point.
//
// LD_PRELOAD into the sm_svclauncher PID that hosts jciMMUI
// (/jci/mmui/svcjcimmui.so). Installs an activatePrevious hook inside
// libjciuiasystem.so so disconnect / reconnect-timer fallback selects
// Audio Off instead of FM (see fallback.cpp).

#define LOG_TAG "CORE"
#include "log.h"
#include "fallback.h"
#include "common/config.h"
#include "common/preload_guard.h"

#include <unistd.h>

namespace {

__attribute__((constructor))
void on_load()
{
    char cmdline[256];
    preload_read_cmdline(cmdline, sizeof(cmdline));
    if (!preload_is_launcher_process(cmdline)) {
        return;
    }

    LOGD("loading (jciMMUI Audio Off fallback) pid=%d ppid=%d cmdline=[%s]",
         (int)getpid(), (int)getppid(), cmdline);
    preload_install_fatal_handler();

    try {
        libpatch_config::load(reinterpret_cast<const void *>(&on_load));
    } catch (...) {
        LOGE("config: load threw — keeping defaults");
    }

    svcjcimmui_fallback::start();
}

} // namespace
