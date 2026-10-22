// SPDX-License-Identifier: AGPL-3.0-or-later
//
// svcjciblmsettings HUD display-language patch.
//
// This LD_PRELOAD library is injected into the sm_svclauncher PID that
// hosts the OEM settings service (/jci/settings/svcjciblmsettings.so,
// service "jciBLMSettings"). It interposes exactly one OEM call:
//
//   int VBS_SETTINGS_SetCMUControlReq(Dbus_conn_s *conn,
//                                     uint8_t type, uint8_t value,
//                                     uint32_t userInfo,
//                                     void *cb, void *user)
//
// an UND (PLT import from /jci/lib/libjcivbssettingsclient.so) in
// svcjciblmsettings.so. Control type 2 is "Display Characters": the
// display language the head unit sends to the instrument cluster and
// the HUD (CAN group 0x131, PID 0x4B). jciBLMSettings sends it on every
// boot and on every language change (callers: BLM_SETTINGS_Set_VBS_Language
// and BLM_SETTINGS_Set_VBS_S16_Languages; value from
// BLM_SETTINGS_Languages_LANG_to_VBS16).
//
// === The problem this solves =================================
//
// The HUD only draws the street-name line for some display languages.
// With the head unit in e.g. Hungarian (Display Characters = 21) the HUD
// shows the maneuver arrow and distance but never the street, even
// though the street string is delivered to it (SetHUD_Display_Msg2).
// With English (7) the street appears. Verified on a 2019 Mazda 6 (EU,
// 74.00.324A): sending 7 while the head unit stays Hungarian makes the
// street name appear, the head-unit UI stays Hungarian. The instrument
// cluster on that car has no Hungarian texts anyway (always English).
//
// === What we do ==============================================
//
// If `hud_display_language` is set in libpatch.conf, rewrite the value
// of every type-2 request to that code before forwarding it. Every other
// request — and every request when the key is off (the default) — is
// forwarded untouched. Nothing else in the process is touched.
//
// === Safety =================================================
//
// jciBLMSettings is reset_board="yes" in sm.conf: if this PID dies the
// whole head unit reboots. So this shim does nothing at load time except
// a cmdline check and a log line, allocates nothing, starts no threads,
// and on any doubt (wrong process, real symbol not found) degrades to a
// transparent passthrough.

#define LOG_TAG "LANG"
#include "log.h"
#include "../common/config.h"
#include "../common/preload.h"
#include "../common/preload_guard.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

// Forward declaration of our exported PLT shadow, so init() can take
// its address for the resolve_real_symbol self-loop guard and for the
// config lookup. Defined at file scope below.
extern "C" int VBS_SETTINGS_SetCMUControlReq(void *conn, uint8_t type,
                                             uint8_t value, uint32_t user_info,
                                             void *cb, void *user);

namespace {

// Control type 2 = "Display Characters" (libjcimod_settings.so:
// VBS_SETTINGS_SetCMUControlReq_svc case 2 -> CAN PID 0x4B).
constexpr uint8_t kTypeDisplayCharacters = 2;

constexpr const char *kSettingsSo = "/jci/settings/svcjciblmsettings.so";
// The client library has no DT_SONAME, so resolve it by absolute path.
constexpr const char *kClientSo   = "/jci/lib/libjcivbssettingsclient.so";

typedef int (*SetCmuControlReqFn)(void *, uint8_t, uint8_t, uint32_t,
                                  void *, void *);

pthread_once_t     g_once    = PTHREAD_ONCE_INIT;
SetCmuControlReqFn g_real    = nullptr;
bool               g_enabled = false;   // right process AND key set
uint8_t            g_code    = 0;       // forced Display Characters code
void              *g_client  = nullptr; // NOLOAD handle cache

void init()
{
    libpatch_config::load(reinterpret_cast<const void *>(&init));

    g_real = reinterpret_cast<SetCmuControlReqFn>(resolve_real_symbol(
        "VBS_SETTINGS_SetCMUControlReq", nullptr, kClientSo,
        reinterpret_cast<void *>(&VBS_SETTINGS_SetCMUControlReq),
        &g_client));

    void *h = dlopen(kSettingsSo, RTLD_NOW | RTLD_NOLOAD);
    const bool in_settings = (h != nullptr);
    if (h != nullptr) {
        dlclose(h);   // only drops our extra NOLOAD refcount
    }

    g_code    = libpatch_config::hud_display_language();
    g_enabled = in_settings && g_real != nullptr && g_code != 0;

    LOGD("init: real=%p in_settings=%d hud_display_language=%u -> %s",
         reinterpret_cast<void *>(g_real), in_settings ? 1 : 0,
         static_cast<unsigned>(g_code),
         g_enabled ? "ACTIVE" : "passthrough");
    if (g_real == nullptr) {
        LOGE("init: real VBS_SETTINGS_SetCMUControlReq not found");
    }
}

__attribute__((constructor))
void on_load()
{
    // Gate on argv[0] so the CMU's watchdog children (which also inherit
    // LD_PRELOAD) stay silent. Nothing else happens at load time.
    char cmdline[256];
    preload_read_cmdline(cmdline, sizeof(cmdline));
    if (!preload_is_launcher_process(cmdline)) {
        return;
    }
    LOGD("loading (svcjciblmsettings HUD display-language hook) pid=%d "
         "cmdline=[%s]", (int)getpid(), cmdline);
}

} // namespace

// Exported PLT shadow. Default visibility so the loader binds
// svcjciblmsettings.so's import to this.
extern "C" PRELOAD_EXPORT
int VBS_SETTINGS_SetCMUControlReq(void *conn, uint8_t type, uint8_t value,
                                  uint32_t user_info, void *cb, void *user)
{
    pthread_once(&g_once, init);

    if (g_real == nullptr) {
        // Cannot forward; report failure the same way the real client
        // does when it cannot build the request.
        return -1;
    }

    if (g_enabled && type == kTypeDisplayCharacters && value != g_code) {
        LOGD("Display Characters %u -> %u", static_cast<unsigned>(value),
             static_cast<unsigned>(g_code));
        value = g_code;
    }

    return g_real(conn, type, value, user_info, cb, user);
}
