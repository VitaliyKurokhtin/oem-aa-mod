// PLT shadows for aap_create_session / aap_destroy_session.
//
// Both symbols are dynamically exported by /usr/lib/libaap_interface.so
// (verified by `nm -D`). The OEM caller is blmjciaapa.so's
// RaceAap::Init / RaceAap::UnInit. Because our library is
// LD_PRELOAD'ed into the {L_jciAAPA} launcher PID before
// blmjciaapa.so is dlopen'd, the dynamic linker binds OEM PLT lookups
// of these names to OUR exported symbols.
//
// We use them as a pure lifecycle bracket for every per-session
// shim thread (touch, HUD, Bluetooth pairing, and play/pause):
// session up   -> install callbacks and start configured workers
// session down -> stop configured workers, then chain through
//
// These are the only cleanly preload-shadowable lifecycle anchors.
// Resolving HMIEventHandler::Init/UnInit via dlsym(RTLD_NEXT) does NOT work
// — those symbols are LOCAL in blmjciaapa.so's static symtab, and
// intra-library OEM callers don't go through the PLT anyway.

#include "log.h"
#include "lifecycle.h"
#include "common/config.h"
#include "audio/audio_keepalive.h"
#include "audio/audio_preopen.h"
#include "audio/audio_stopdelay.h"
#include "hud/hud.h"
#include "bt16pair/bt16pair.h"
#include "monitor/navi_monitor.h"
#include "playpause/playpause.h"
#include "oem/blmjciaapa.h"
#include "touch/touch.h"
#include "common/preload.h"   // PRELOAD_EXPORT + resolve_real_symbol

#include <dlfcn.h>
#include <pthread.h>
#include <string.h>

namespace {

typedef int (*pfn_aap_create_session)(const char *cfg, void *unknown_r1,
                                      void *cb_list, void **out_handle);
typedef int (*pfn_aap_destroy_session)(void *handle);

pfn_aap_create_session  g_real_create  = nullptr;
pfn_aap_destroy_session g_real_destroy = nullptr;

pthread_mutex_t g_session_mu = PTHREAD_MUTEX_INITIALIZER;
void           *g_session    = nullptr;   // last successfully-created handle
bool            g_session_up = false;     // per-session shim threads (touch + HUD) alive
bool            g_enabled    = false;     // true after blmjciaapa.so is confirmed mapped
bool            g_self_gate_done = false;

void oem_self_gate(void)
{
    if (g_self_gate_done) return;
    g_self_gate_done = true;

    // RTLD_NOLOAD: don't actually load if absent. Returns NULL if
    // blmjciaapa.so isn't already mapped, i.e. we're in the wrong PID.
    void *h = dlopen("/jci/aapa/blmjciaapa.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        // Expected when LD_PRELOAD'd into the wrong launcher PID;
        // shim degrades to inert passthrough.
        LOGW("self-gate: blmjciaapa.so not mapped — disabling shim");
        g_enabled = false;
        return;
    }

    g_enabled = true;
    LOGD("self-gate: enabled, blmjciaapa.so handle=%p", h);
}

// Real libaap_interface.so symbols are resolved via the shared
// resolve_real_symbol() helper (common/preload.h): it tries
// dlsym(RTLD_NEXT) first, then NOLOAD-dlopens the defining library
// and dlsyms that handle (libaap_interface.so lands in blmjciaapa.so's
// *local* scope — RTLD_NEXT alone misses it), with a self-loop guard
// that refuses to return our own exported shim. g_libaap_handle is the
// caller-owned NOLOAD handle cache so repeat resolutions don't leak
// refcounts.
void *g_libaap_handle = nullptr;

// Forward declarations of our own exported PLT shadows so we can pass
// their addresses as the self-loop guard to resolve_real_symbol.
// Defined further down in this file with PRELOAD_EXPORT.
extern "C" int aap_create_session(const char *, void *, void *, void **);
extern "C" int aap_destroy_session(void *);

// IMPORTANT: the OEM's libaap_interface.so `aap_create_session`
// takes FOUR register arguments, not three.
//
// The Ghidra decompilation of libaap_interface.so shows only three
// parameters because Ghidra dropped the always-NULL r1 argument.
// The OEM caller in blmjciaapa.so's RaceAap::Init at
// blmjciaapa.so:0x8c7a0..0x8c7b4 (verified with `objdump -d`)
// actually sets up:
//
//   r0 = cfg            (config_file_name string)
//   r1 = 0              (always NULL in shipped builds)
//   r2 = &local_cb_list (88-byte stack struct of callbacks)
//   r3 = this+4         (where the session handle should be written)
//
// The callee:
//   * uses r2 as cb_list  (reads function pointers at offsets 4/8/c/10)
//   * uses r3 as out_handle (writes the malloc'd session struct ptr there)
//
// If we declare the function pointer with 3 args:
//   - the compiler emits `blx r3` with r3=&aap_create_session, so the
//     callee writes the session handle to its own .text (read-only) page
//     and we SIGSEGV.
// If we declare with 3 args + dummy `int*` for r3:
//   - the crash goes away, but the OEM writes the handle to our local
//     dummy, NOT to blmjciaapa.so's `this+4` slot, so subsequent SDK
//     calls see handle=NULL and the SDK errors with
//     "ERR::AAP_SDK_IFACE::3262::Invalid handle 0, actual handle X".
//
// The correct shape is 4 args; we just forward `unknown_r1` verbatim
// from caller to callee so the OEM's NULL stays NULL and the
// real `out_handle` (= this+4) gets the session pointer it expects.

void resolve_real_once(void)
{
    if (!g_real_create) {
        g_real_create = reinterpret_cast<pfn_aap_create_session>(
            resolve_real_symbol("aap_create_session",
                                "libaap_interface.so",
                                "/usr/lib/libaap_interface.so",
                                reinterpret_cast<void *>(&aap_create_session),
                                &g_libaap_handle));
    }
    if (!g_real_destroy) {
        g_real_destroy = reinterpret_cast<pfn_aap_destroy_session>(
            resolve_real_symbol("aap_destroy_session",
                                "libaap_interface.so",
                                "/usr/lib/libaap_interface.so",
                                reinterpret_cast<void *>(&aap_destroy_session),
                                &g_libaap_handle));
    }
}

} // namespace

extern "C" PRELOAD_EXPORT
int aap_create_session(const char *cfg, void *unknown_r1,
                       void *cb_list, void **out_handle)
{
    LOGD("aap_create_session: ENTER cfg=%p r1=%p cb_list=%p out_handle=%p",
         (const void *)cfg, unknown_r1, cb_list, (void *)out_handle);

    resolve_real_once();
    LOGD("aap_create_session: resolve_real_once done "
         "(real_create=%p real_destroy=%p)",
         (void *)g_real_create, (void *)g_real_destroy);

    // First invocation: try to set g_enabled by confirming the OEM
    // BLM is mapped. We have to do this here (not in the constructor)
    // because blmjciaapa.so isn't dlopen'd at constructor time.
    oem_self_gate();
    LOGD("aap_create_session: self-gate done (g_enabled=%d)",
         g_enabled ? 1 : 0);

    if (!g_real_create) {
        // Nothing we can do — without the original, the OEM startup
        // breaks. Return the SDK's "internal error" code.
        LOGC("aap_create_session: real implementation not resolved "
             "— passing through impossible");
        return 0x103;
    }

    // Nav-side hijack: substitute cb_list[10] (E_AAP_EVENT_NAV_DATA_CB)
    // with our dump callback before the SDK copies the table into the
    // session handle. The OEM leaves slot 10 NULL, so without this
    // every 0x500 / 0x501 / 0x502 event the phone sends gets dropped
    // by libaap_interface.so's dispatcher (case 0xc, branches on
    // handle+0x48 == NULL). Gated on g_enabled so a misdeployed
    // library still transparently passes through.
    //
    // Modifying the caller's stack buffer in place is safe: inside
    // aap_create_session the SDK does memcpy(handle+0x20, cb_list, 76)
    // before returning. Gated on the HUD config flag too: with HUD off
    // we leave slot 10 NULL so nav events are dropped by the SDK as on
    // a stock library (no point capturing data we won't forward).
    if (g_enabled && libpatch_config::hud_enabled()) {
        hud_pre_aap_create_session(cb_list);
    }

    // Wireless GAL-1.6 BT-pairing bypass: wrap ProjectionStatusCb
    // (cb_list slot 4) so we can synthesize the pairing success that the
    // AAWireless dongle omits at GAL >= 1.6 (see bt16pair/). Gated on
    // use_protocol_v1_6; the wrapper is transparent and only acts when the
    // connected USB device is "AAWireless", so wired and non-1.6
    // sessions are unaffected. Must run before the real create so the
    // SDK copies our wrapper into the session handle's callback table.
    if (g_enabled && libpatch_config::use_protocol_v1_6()) {
        bt16pair_pre_aap_create_session(cb_list);
    }

    LOGD("aap_create_session: calling real impl at %p",
         (void *)g_real_create);
    int rc = g_real_create(cfg, unknown_r1, cb_list, out_handle);
    LOGD("aap_create_session: real impl returned rc=%d *out_handle=%p",
         rc, (out_handle && *out_handle) ? *out_handle : nullptr);

    if (rc != 0 || !g_enabled || !out_handle || !*out_handle) {
        LOGD("aap_create_session: NOT starting touch reader "
             "(rc=%d g_enabled=%d out_handle=%p *out_handle=%p)",
             rc, g_enabled ? 1 : 0, (void *)out_handle,
             (out_handle && *out_handle) ? *out_handle : nullptr);
        return rc;
    }

    // OEM retries with sleep(1) on rc==0x104; our reader is keyed off
    // the FIRST successful create, so we only start once per process.
    pthread_mutex_lock(&g_session_mu);
    g_session = *out_handle;
    if (!g_session_up) {
        if (libpatch_config::touch_enabled()) {
            LOGD("aap_create_session: spawning touch reader");
            touch_post_aap_create_session();
            LOGD("touch reader started");
        } else {
            LOGD("aap_create_session: touch disabled by config — skipping reader");
        }

        // HUD post-create hook: opens the OEM HMI + Service D-Bus
        // connections and brings up dispatcher + sender threads.
        // Same "once per process" gate as
        // touch_post_aap_create_session — the sender thread itself
        // handles HUD-not-installed and routing start/stop
        // transitions internally.
        if (libpatch_config::hud_enabled()) {
            LOGD("aap_create_session: spawning HUD sender");
            hud_post_aap_create_session();
            LOGD("HUD post-create hook completed");
        } else {
            LOGD("aap_create_session: HUD disabled by config — skipping sender");
        }

        // Play/pause bridge: watch the OEM audio manager's user-mute
        // signal (com.jci.vbs.am / EntertainmentMuteStatus) and pause/resume
        // the phone's AA media accordingly. Same once-per-process gate; the
        // watcher thread owns its own service-bus connection and tolerates the
        // session not being fully connected yet.
        if (libpatch_config::mute_pauses_phone()) {
            LOGD("aap_create_session: spawning play/pause watcher");
            playpause_post_aap_create_session();
            LOGD("play/pause watcher hook completed");
        } else {
            LOGD("aap_create_session: mute_pauses_phone disabled by config — "
                  "skipping play/pause watcher");
        }

        // AA guidance head pre-open: install the once-per-process entry detour
        // on AudioManager::SendReplyRequestFocus so the amp's NAVI mix opens at
        // the OEM's own duck-grant (ahead of its late stream-start focus
        // request), keeping the head of a guidance prompt from being clipped.
        // The install is idempotent, so running it on the same session edge as
        // the other post-create hooks is safe; blmjciaapa.so is confirmed
        // mapped by this point (the constructor is too early). Gated on the
        // shared low-latency key.
        if (libpatch_config::aa_audio_low_latency()) {
            LOGD("aap_create_session: installing audio head pre-open detour");
            audio_preopen_post_aap_create_session();
            LOGD("audio head pre-open hook completed");

            // Cold-open keepalive: hold the shared dmix client open so the first
            // guidance prompt over a tuner doesn't pay the multi-second cold open
            // of the hw:0,0 slave. Once-per-process (idempotent); the holder does
            // the blocking open on its own thread, so this call doesn't stall
            // session bring-up. Same low-latency key.
            LOGD("aap_create_session: starting audio cold-open keepalive");
            audio_keepalive_init();
            LOGD("audio cold-open keepalive hook completed");

            // End-cutoff amp hold: bring up the drain-done event + helper thread up front (same
            // low-latency key), so the timer_settime interpose's gate is resolved here, not lazily.
            LOGD("aap_create_session: arming audio end-cutoff amp hold");
            audio_stopdelay_init();
            LOGD("audio end-cutoff amp hold armed");
        } else {
            LOGD("aap_create_session: aa_audio_low_latency disabled by config — "
                  "skipping audio head pre-open + cold-open keepalive");
        }

#if defined(DEBUG) && BLMJCIAAPA_ENABLE_NAVI_MONITOR
        // Debug-only: eavesdrop on com.jci.vbs.navi(.tmc) traffic and
        // forward dbus-monitor output to the patch log. Compiled out
        // of release builds entirely (both call site and module body).
        LOGD("aap_create_session: starting navi dbus-monitor");
        navi_monitor_post_aap_create_session();
        LOGD("navi dbus-monitor start hook completed");
#endif

        g_session_up = true;
    }
    pthread_mutex_unlock(&g_session_mu);

    LOGD("aap_create_session: LEAVE rc=%d", rc);
    return rc;
}

extern "C" PRELOAD_EXPORT
int aap_destroy_session(void *handle)
{
    LOGD("aap_destroy_session: ENTER handle=%p", handle);
    resolve_real_once();

    if (g_enabled) {
        pthread_mutex_lock(&g_session_mu);
        if (g_session_up) {
            if (libpatch_config::touch_enabled()) {
                LOGD("aap_destroy_session: stopping touch reader");
                touch_pre_aap_destroy_session();
                LOGD("touch reader stopped");
            }

            // Tear the HUD sender down on the same edge that
            // stops the touch reader. The hook is idempotent so
            // the gate on g_session_up keeps it matched 1:1 with
            // the start above. config flags are immutable after
            // load, so start/stop stay symmetric.
            if (libpatch_config::hud_enabled()) {
                LOGD("aap_destroy_session: stopping HUD sender");
                hud_pre_aap_destroy_session();
                LOGD("HUD pre-destroy hook completed");
            }

            if (libpatch_config::mute_pauses_phone()) {
                LOGD("aap_destroy_session: stopping play/pause watcher");
                playpause_pre_aap_destroy_session();
                LOGD("play/pause watcher stopped");
            }

#if defined(DEBUG) && BLMJCIAAPA_ENABLE_NAVI_MONITOR
            LOGD("aap_destroy_session: stopping navi dbus-monitor");
            navi_monitor_pre_aap_destroy_session();
            LOGD("navi dbus-monitor stop hook completed");
#endif

            g_session_up = false;
        }
        g_session = nullptr;
        pthread_mutex_unlock(&g_session_mu);
    }

    if (!g_real_destroy) {
        LOGC("aap_destroy_session: real implementation not resolved");
        return 0x103;
    }

    LOGD("aap_destroy_session: calling real impl at %p",
         (void *)g_real_destroy);
    int rc = g_real_destroy(handle);
    LOGD("aap_destroy_session: LEAVE rc=%d", rc);
    return rc;
}
