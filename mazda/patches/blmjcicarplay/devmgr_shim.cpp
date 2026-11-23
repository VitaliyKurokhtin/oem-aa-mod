// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CarPlay -> HUD bridge for Mazda Connect (CMU150).
// Copyright (C) 2026 KID MIXER-MODER.
//
// Part of an AGPL-3.0 work - see NOTICE.md for full attribution.
//
// PLT shadows for the devmgr client API that jciCARPLAY (blmjcicarplay.so)
// imports from /usr/lib/libdevmgr_interface.so, PLUS a libc `msgrcv` interposer
// that passively taps the device-manager receive path.
//
// Why msgrcv: RE of eDevMgrRegisterProcess + the dispatch loop (FUN_000402c8 in
// libdevmgr_interface.so) shows the process handle stores cbTable@+0x6c, ctx@+0x70,
// msgq@+0x14, and the receive thread (vDevMgrProcessRcvThread, running INSIDE the
// jciCARPLAY process) pulls every event with race_msg_queue_receivemessage ->
// libc `msgrcv` (imported via PLT, confirmed). ipoddev's viAP2NavRouteManeuverUpdate
// pushes a 0x348-byte message (guidance = 0x640). So interposing msgrcv lets us
// SEE every message ipoddev delivers — including maneuvers — without touching the
// OEM vtable/dispatch (zero crash risk, fully passive: we always chain the real
// call and never alter the data). This is BOTH a definitive probe (does iOS ever
// send maneuvers?) AND the hook point a future build-2b can decode -> hud_send.
//
// ARM EABI: devmgr fns take args in r0..r3; we declare 4 void* and forward all.

#include "patch.h"
#include "oem/libjcidbus.h"
#include "common/config.h"   // runtime libpatch-carplay.conf (carplay_*_diag keys)

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

namespace {

typedef int (*pfn4)(void *, void *, void *, void *);

pfn4  g_real_register  = nullptr;
pfn4  g_real_callstate = nullptr;
pfn4  g_real_commupd   = nullptr;
pfn4  g_real_sendloc   = nullptr;
pfn4  g_real_startnavi = nullptr;

void *g_devmgr_handle = nullptr;
pthread_mutex_t g_mu        = PTHREAD_MUTEX_INITIALIZER;
bool            g_navi_armed = false;
volatile int    g_armed      = 0;   // set once a live CarPlay session has armed nav; gates the passive msgrcv tap (no tapping until a nav session is up)
void *g_procH = nullptr;
void *g_devH  = nullptr;

extern "C" int eDevMgrRegisterProcess(void *, void *, void *, void *);
extern "C" int eDevMgriPodStartCallStateUpdates(void *, void *, void *, void *);
extern "C" int eDevMgriPodStartCommunicationUpdates(void *, void *, void *, void *);
extern "C" int eDevMgriPodSendLocationInformation(void *, void *, void *, void *);

bool addr_is_own(void *a)
{
    return a == (void *)&eDevMgrRegisterProcess ||
           a == (void *)&eDevMgriPodStartCallStateUpdates ||
           a == (void *)&eDevMgriPodStartCommunicationUpdates ||
           a == (void *)&eDevMgriPodSendLocationInformation;
}

void *resolve_oem(const char *name)
{
    void *p = dlsym(RTLD_NEXT, name);
    if (p && !addr_is_own(p)) return p;
    if (!g_devmgr_handle) {
        g_devmgr_handle = dlopen("libdevmgr_interface.so", RTLD_NOW | RTLD_NOLOAD);
        if (!g_devmgr_handle)
            g_devmgr_handle = dlopen("/usr/lib/libdevmgr_interface.so", RTLD_NOW | RTLD_NOLOAD);
        if (!g_devmgr_handle) { LOGE("resolve_oem: dlopen NOLOAD failed: %s", dlerror()); return nullptr; }
    }
    p = dlsym(g_devmgr_handle, name);
    if (!p) { LOGE("resolve_oem(%s): dlsym failed: %s", name, dlerror()); return nullptr; }
    if (addr_is_own(p)) { LOGC("resolve_oem(%s): interposition trap", name); return nullptr; }
    return p;
}

void resolve_all_once(void)
{
    if (!g_real_register)  g_real_register  = (pfn4)resolve_oem("eDevMgrRegisterProcess");
    if (!g_real_callstate) g_real_callstate = (pfn4)resolve_oem("eDevMgriPodStartCallStateUpdates");
    if (!g_real_commupd)   g_real_commupd   = (pfn4)resolve_oem("eDevMgriPodStartCommunicationUpdates");
    if (!g_real_sendloc)   g_real_sendloc   = (pfn4)resolve_oem("eDevMgriPodSendLocationInformation");
    if (!g_real_startnavi) g_real_startnavi = (pfn4)resolve_oem("eDevMgriPodStartNaviRouteGuidenceUpdates");
}

void *navi_retry_thread(void *)
{
    int n = 0;
    for (;;) {
        sleep(5);
        if (!g_real_startnavi || !g_procH) continue;
        void *p3 = (n & 1) ? (void *)0x5555 : (void *)0;
        int rc = g_real_startnavi(g_procH, g_devH, p3, (void *)0);
        LOGD("retry#%d StartNaviRouteGuidence(procH=%p devH=%p p3=%p) rc=%d",
             n, g_procH, g_devH, p3, rc);
        n++;
    }
    return nullptr;
}

// ===== [NAV-END via OEM CarPlay TBT status] =================================
// The OEM blmjcicarplay.so (this very process) broadcasts the explicit CarPlay
// nav-status on the HMI bus's com.jci.carplay interface — the same event AA gets
// decoded from the AAP SDK, which the CarPlay bridge previously lacked (it only
// cleared via a 6s staleness timer). We SUBSCRIBE to it using the OEM's own
// generated client stubs (libjcicarplay_client.so), so the OEM does the match +
// decode for us. Signals of interest:
//   TurnByTurnEntitySignal(entityType u): 0=NONE 1=CARPLAY 2=NATIVE — who owns TBT.
//   SessionDeactiveSignal: CarPlay session ended (e.g. phone unplugged).
// cb ABI (RE-confirmed byte-exact: client dispatcher signal_carplay_* @0xb2d0 +
// consumer svcjcinavi CP_RoutingStatusChanged_cb @0x2e0c4): the worker calls
//   cb(r0=conn, r1=<field0 ptr>, r2=<field1/aux ptr>, r3=userdata)
// with each decoded field delivered BY POINTER (deref r1 for the u32), userdata in
// r3, and r1/r2 freed right after the callback returns (copy out, don't retain).
typedef int (*pfn_cp_enable)(void *conn, int enable, void *cb, void *userdata);
pfn_cp_enable g_cp_tbt_enable      = nullptr;
pfn_cp_enable g_cp_deactive_enable = nullptr;
void *g_cp_hmi_conn   = nullptr;
void *g_cp_client_lib = nullptr;
bool  g_cp_subscribed = false;

void cp_hmi_err(void *conn, void *closure)
{ (void)conn; (void)closure; LOGW("carplay_status: HMI bus error cb (non-fatal)"); }

// TurnByTurnEntitySignal handler. r1 = uint32_t* entityType (deref). DEBUG BUILD:
// LOGD only — wire hud_on_status(0) clear AFTER a real-drive test confirms the values.
void cp_tbt_entity_cb(void *conn, void *p_entity, void *p_aux, void *userdata)
{
    (void)conn; (void)p_aux; (void)userdata;
    try {
        unsigned e = p_entity ? *reinterpret_cast<const uint32_t *>(p_entity) : 0xffffffffu;
        LOGD("carplay TBT entity=%u (%s)",
             e, e == 1 ? "CARPLAY" : e == 2 ? "NATIVE" : e == 0 ? "NONE" : "?");
        // entityType 1=CARPLAY (CarPlay owns turn-by-turn = nav active). Anything else — 0=NONE
        // (nav switched off) or 2=NATIVE (the OEM nav took the HUD) — means CarPlay is no longer
        // doing TBT, so wipe our HUD. Verified live: turning nav off fires entity 1->0. The 6s
        // staleness timer stays as a backstop for any unit where this subscribe didn't come up.
        if (e != 1) hud_request_clear();
    } catch (...) { LOGE("cp_tbt_entity_cb exception swallowed (OEM dbus thread)"); }
}

// SessionDeactiveSignal handler. Multi-field payload (clientId,type,reason,...) — we don't
// read the fields; arrival = session gone. DEBUG: LOGD only.
void cp_deactive_cb(void *a0, void *a1, void *a2, void *a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    LOGD("carplay SessionDeactive (session ended)");
    hud_request_fullclear();   // CarPlay session gone (e.g. phone unplugged) -> FULL wipe incl sign (like AA teardown)
}

void *cp_resolve(const char *name)
{
    void *p = g_cp_client_lib ? dlsym(g_cp_client_lib, name) : nullptr;
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    return p;
}

// Subscribe to the OEM CarPlay TBT-status signals on the HMI bus. Idempotent; best-effort
// (any failure just leaves the staleness timer as the fallback — never fatal).
void carplay_status_start(void)
{
    if (g_cp_subscribed) return;
    // The CARPLAY_*Signal_enable stubs live in the CONSUMER lib libjcicarplay_client.so, which
    // jciCARPLAY (the producer) does not normally load — dlopen it GLOBAL so its JCIDBUS imports
    // resolve against the already-loaded provider.
    if (!g_cp_client_lib) {
        g_cp_client_lib = dlopen("libjcicarplay_client.so", RTLD_NOW | RTLD_GLOBAL);
        if (!g_cp_client_lib)
            g_cp_client_lib = dlopen("/jci/lib/libjcicarplay_client.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_cp_client_lib) { LOGW("carplay_status: dlopen libjcicarplay_client.so failed: %s", dlerror()); return; }
    g_cp_tbt_enable      = (pfn_cp_enable)cp_resolve("CARPLAY_TurnByTurnEntitySignal_enable");
    g_cp_deactive_enable = (pfn_cp_enable)cp_resolve("CARPLAY_SessionDeactiveSignal_enable");
    if (!g_cp_tbt_enable) { LOGW("carplay_status: TurnByTurnEntitySignal_enable unresolved — staleness only"); return; }

    g_cp_hmi_conn = JCIDBUS_conn_create(reinterpret_cast<void *>(&cp_hmi_err), 0);
    if (!g_cp_hmi_conn) { LOGW("carplay_status: HMI conn_create failed"); return; }
    if (JCIDBUS_conn_connect(g_cp_hmi_conn, "com.jci.carplay.hudstatus", kJciHmiBus, nullptr) == 0) {
        LOGW("carplay_status: HMI conn_connect failed — staleness only");
        JCIDBUS_conn_free(g_cp_hmi_conn); g_cp_hmi_conn = nullptr; return;
    }
    JCIDBUS_worker_start(g_cp_hmi_conn);
    int re = g_cp_tbt_enable(g_cp_hmi_conn, 1, reinterpret_cast<void *>(&cp_tbt_entity_cb), nullptr);
    int rd = g_cp_deactive_enable
               ? g_cp_deactive_enable(g_cp_hmi_conn, 1, reinterpret_cast<void *>(&cp_deactive_cb), nullptr)
               : -1;
    g_cp_subscribed = true;
    LOGD("carplay_status: subscribed HMI com.jci.carplay TurnByTurnEntity(rc=%d) + SessionDeactive(rc=%d)", re, rd);
}

void arm_navi(const char *from, void *procH, void *devH)
{
    // Only run inside the real jciCARPLAY launcher (main.cpp's constructor gate set g_enabled).
    if (!g_enabled) { LOGW("carplay: not enabled (%s)", from); return; }
    g_armed = 1;
    pthread_mutex_lock(&g_mu);
    g_procH = procH;
    g_devH  = devH;
    if (!g_navi_armed) {
        g_navi_armed = true;
        LOGD("arm_navi from %s: StartNaviRouteGuidence(procH=%p devH=%p 0)", from, procH, devH);
        // Bring up the HUD sender (D-Bus to com.jci.vbs.navi) so nav_on_devmgr_msg
        // has somewhere to push decoded maneuvers. Idempotent.
        hud_send_start();
        carplay_status_start();   // [NAV-END] subscribe OEM CarPlay TBT-status (debug: LOGD only)
        if (g_real_startnavi) {
            int rc = g_real_startnavi(procH, devH, (void *)0, (void *)0);
            LOGD("StartNaviRouteGuidence rc=%d; spawning retry+watch", rc);
            pthread_t th;
            if (pthread_create(&th, nullptr, navi_retry_thread, nullptr) == 0)
                pthread_detach(th);
        } else { LOGE("arm_navi: StartNavi unresolved"); g_navi_armed = false; }
    }
    pthread_mutex_unlock(&g_mu);
}

// ---- msgrcv tap (passive) --------------------------------------------------
typedef long (*pfn_msgrcv)(int, void *, unsigned, long, int);
pfn_msgrcv      g_real_msgrcv = nullptr;

// [DIAGNOSTIC] In debug builds (DEBUG / !NDEBUG), message types and raw nav
// payloads are automatically logged to the system log (stderr / g_logf) with
// namespaces [CP-MSG] and [CP-NAV] for easy filtering, avoiding flash wear.
void msg_tap(int msqid, const void *msgp, long n)
{
    (void)msqid;

#ifndef NDEBUG
    // In DEBUG mode: log every received message's (type, size) + a payload peek for small messages
    if (msgp && n >= 4) {
        uint32_t mt = (*(const uint32_t *)msgp) & 0xffff;
        char peek[128] = "";
        if (mt != 0x8058 && mt != 0x8059) {          // peek the unknown / small messages
            const unsigned char *b = (const unsigned char *)msgp;
            long dn = (n < 20) ? n : 20;
            char *p = peek;
            for (long i = 0; i < dn; i++) {
                p += snprintf(p, sizeof(peek) - (p - peek), " %02x", b[i]);
            }
        }
        LOGD("[CP-MSG] mt=0x%04x n=0x%lx%s", (unsigned)mt, n, peek);
    }
#endif

    // Only big messages: maneuver=0x348, guidance=0x640. Skips chatty small msgs.
    if (n < 0x300 || !msgp) return;

    // Decode maneuver -> HUD (passive read of the received buffer).
    nav_on_devmgr_msg(msgp, n);

#ifndef NDEBUG
    // In DEBUG mode: dump raw nav payload to system log
    long mt = (*(const long *)msgp) & 0xffff;
    if (mt == 0x8059 || mt == 0x8058) {
        LOGD("[CP-NAV] msgrcv n=0x%lx mtype=0x%lx", n, mt);
        const unsigned char *b = (const unsigned char *)msgp;
        char hex[100];
        for (long i = 0; i < n; i += 16) {
            long chunk = (n - i < 16) ? (n - i) : 16;
            char *p = hex;
            for (long j = 0; j < chunk; j++) {
                p += snprintf(p, sizeof(hex) - (p - hex), " %02x", b[i + j]);
            }
            LOGD("[CP-NAV] +%04lx:%s", i, hex);
        }
    }
#endif
}

} // namespace

#define PRELOAD_EXPORT __attribute__((visibility("default")))

extern "C" PRELOAD_EXPORT
long msgrcv(int msqid, void *msgp, unsigned msgsz, long msgtyp, int msgflg)
{
    if (!g_real_msgrcv) g_real_msgrcv = (pfn_msgrcv)dlsym(RTLD_NEXT, "msgrcv");
    long n = g_real_msgrcv ? g_real_msgrcv(msqid, msgp, msgsz, msgtyp, msgflg) : -1;
    // Only tap once nav is armed by a live CarPlay session (g_armed set in arm_navi).
    if (g_enabled && g_armed && n > 0) msg_tap(msqid, msgp, n);
    return n;
}

extern "C" PRELOAD_EXPORT
int eDevMgrRegisterProcess(void *procStruct, void *role, void *cbTable, void *ctx)
{
    resolve_all_once();
    LOGD("eDevMgrRegisterProcess: procStruct=%p role=%ld cbTable=%p ctx=%p",
         procStruct, (long)role, cbTable, ctx);
    if (!g_real_register) { LOGC("RegisterProcess: real unresolved"); return 0x103; }
    int rc = g_real_register(procStruct, role, cbTable, ctx);
    LOGD("eDevMgrRegisterProcess: rc=%d", rc);
    return rc;
}

extern "C" PRELOAD_EXPORT
int eDevMgriPodStartCallStateUpdates(void *a0, void *a1, void *a2, void *a3)
{
    resolve_all_once();
    LOGD("StartCallStateUpdates: a0=%p a1=%p a2=%p a3=%p", a0, a1, a2, a3);
    int rc = g_real_callstate ? g_real_callstate(a0, a1, a2, a3) : 0x103;
    arm_navi("CallState", a0, a1);
    return rc;
}

extern "C" PRELOAD_EXPORT
int eDevMgriPodStartCommunicationUpdates(void *a0, void *a1, void *a2, void *a3)
{
    resolve_all_once();
    int rc = g_real_commupd ? g_real_commupd(a0, a1, a2, a3) : 0x103;
    arm_navi("CommUpdates", a0, a1);
    return rc;
}

extern "C" PRELOAD_EXPORT
int eDevMgriPodSendLocationInformation(void *a0, void *a1, void *a2, void *a3)
{
    resolve_all_once();
    int rc = g_real_sendloc ? g_real_sendloc(a0, a1, a2, a3) : 0x103;
    arm_navi("SendLocation", a0, a1);
    return rc;
}
