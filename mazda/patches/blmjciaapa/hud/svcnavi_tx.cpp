// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Adapted from headunit (https://github.com/Trevelopment/headunit),
// licensed under GNU AGPL v3.
// See NOTICE.md at the repo root for the full attribution.
//
// svcjcinavi HUD transport — emits com.NNG.Api.Server.Guidance
// .GuidanceChangedForHUD signals so the OEM navi service forwards
// our guidance to the HUD ECU as the single frame writer.
//
// See svcnavi_tx.h for the rationale, the signal signature, and
// why this transport requires svcjcinavi (nav SD card) to be present.
//
// Structure intentionally mirrors vbs_tx.cpp: a seqlock-protected
// latest-wins snapshot written by the SDK callback thread, drained
// by a dedicated sender thread that owns the OEM connection. Only the
// output stage differs — we build and emit the D-Bus signal with raw
// libdbus-1 (exactly as the NNG engine does), NOT through libjcidbus.
//
// Why not libjcidbus: its JCIDBUS_signal_send is a serve-only path
// (calls signal_registered() and returns -1 / "Signal %s is not added
// in any Interface!" for anything we didn't register as a served
// object). We are a client impersonating NNG, so we emit the bare
// signal over libdbus-1 — see oem/libdbus.h.

#define LOG_TAG "SVCNAVI"
#include "../log.h"
#include "svcnavi_tx.h"
#include "hud_nav.h"
#include "hud_lane.h"
#include "common/string_safe.h"
#include "../oem/libdbus.h"
#include "../../common/oem/vbs_navi_hud.h"   // kAapSpeedSentinel (shared with svcjcinavi)
#include "common/thread_util.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <string>
#include <atomic>
#include <mutex>
#include <pthread.h>
#include <sched.h>

namespace {

// Service-bus address. The OEM exports it in $JCI_SERVICE_BUS; the
// shipped value is unix:path=/tmp/dbus_service_socket, which we fall
// back to if the env var is unset in our PID.
constexpr const char *kServiceBusFallback = "unix:path=/tmp/dbus_service_socket";

// The GuidanceChangedForHUD destination, verified against the
// svcjcinavi subscription (jcidbus_signal_enable with path
// /com/NNG/Api/Server, interface com.NNG.Api.Server.Guidance).
constexpr const char *kSignalPath   = "/com/NNG/Api/Server";
constexpr const char *kSignalIface  = "com.NNG.Api.Server.Guidance";
constexpr const char *kSignalMember = "GuidanceChangedForHUD";

// Reserved speedLimit value marking a frame as AAP-originated lives in
// common/oem/vbs_navi_hud.h (kAapSpeedSentinel) so the svcjcinavi merge
// patch keys on the exact same value. On the GuidanceChangedForHUD
// wire the speedLimit arg is int32; 0xFFFF round-trips unchanged.

// map_distance_unit() (distance-unit enum) comes from hud_nav.h, shared with
// the vbs transport. The turn-icon mapping (kTurnIcons / compute_turn_icon) now
// lives in hud.cpp — this transport receives an already-resolved glyph.

// === Shared snapshot (seqlock-protected) ======================
// Single producer (SDK cb), single consumer (sender thread). Same
// pattern as vbs_tx.cpp.
struct NaviSnapshot {
    char     road_name[64];
    uint32_t dir_icon;       // Mazda HUD maneuver glyph (resolved by hud.cpp)
    int32_t  distance_dec;   // value * 10
    uint8_t  distance_unit;  // mapped to Mazda enum
    uint8_t  lanes[8];       // OEM lane codes: 0=hidden, 1..70 (svcjcinavi maps code->glyph)
};

NaviSnapshot            g_snapshot = {};
std::atomic<uint32_t>   g_seq{0};
std::condition_variable g_cv;
std::mutex              g_cv_mu;
std::atomic<bool>       g_active{false};
std::atomic<bool>       g_stop{false};

pthread_t g_sender_thread    = 0;
bool      g_sender_thread_up = false;

void *g_conn = nullptr;

// Append one INT32 to the open iterator. Returns false on OOM.
bool append_i32(DBusMessageIter *iter, int32_t value)
{
    return dbus_message_iter_append_basic(iter, DBUS_TYPE_INT32, &value) != 0;
}

// Emit one GuidanceChangedForHUD signal. Signature iiisiiiiiiiiii:
//   dirIcon, maneuverDist, distUnit, streetName,
//   speedLimit(=sentinel), speedUnit, lane0..lane7.
// Built and sent with raw libdbus-1 on our private connection, the
// way the NNG engine emits it (libjcidbus's signal_send would reject
// a signal we don't serve).
void emit_guidance(uint32_t dir_icon, int32_t maneuver_dist,
                   int32_t dist_unit, const char *street,
                   const uint8_t *lanes)
{
    void *msg = dbus_message_new_signal(kSignalPath, kSignalIface, kSignalMember);
    if (msg == nullptr) {
        LOGE("svcnavi sender: dbus_message_new_signal failed (OOM?)");
        return;
    }

    const char *s = street ? street : "";
    DBusMessageIter it;
    dbus_message_iter_init_append(msg, &it);

    bool ok = true;
    ok = ok && append_i32(&it, static_cast<int32_t>(dir_icon));
    ok = ok && append_i32(&it, maneuver_dist);
    ok = ok && append_i32(&it, dist_unit);
    ok = ok && (dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &s) != 0);
    ok = ok && append_i32(&it, kAapSpeedSentinel); // speedLimit
    ok = ok && append_i32(&it, 0);                 // speedUnit
    // lane0..7: the Mazda lane bytes verbatim. Our canonical hidden code is 0,
    // which is exactly what GuidanceChangedForHUD wants for an empty slot (its
    // handler validates each lane arg to 0..0x46), so no remap is needed.
    for (int i = 0; i < 8 && ok; ++i) {
        ok = append_i32(&it, static_cast<int32_t>(lanes ? lanes[i] : 0));
    }
    if (!ok) {
        LOGE("svcnavi sender: marshalling GuidanceChangedForHUD failed (OOM)");
        dbus_message_unref(msg);
        return;
    }

    if (dbus_connection_send(g_conn, msg, nullptr) == 0) {
        LOGE("svcnavi sender: dbus_connection_send failed (OOM / disconnected)");
    } else {
        dbus_connection_flush(g_conn);
        LOGV("svcnavi sender: GuidanceChangedForHUD dirIcon=%u dist=%d unit=%d "
             "street=\"%s\" speedLimit=0x%x",
             static_cast<unsigned>(dir_icon), maneuver_dist, dist_unit, s,
             static_cast<unsigned>(kAapSpeedSentinel));
    }
    dbus_message_unref(msg);
}

// snap -> emit. svcjcinavi owns the sync bit / street-strip pairing,
// so we only decide WHEN to push. We push on EVERY fresh snapshot, with
// no content change-detection: we are not the HUD frame writer
// (svcjcinavi is), and it can silently ignore an individual frame —
// e.g. one that arrives right as the HUD is being enabled. Both AAP and
// the OEM nav engine re-send guidance at roughly 1 Hz, so re-asserting
// our guidance on every update keeps the HUD from sitting blank for a
// long stretch (until the next genuine value change) when an early
// frame was dropped.
void send_one(const NaviSnapshot &cur)
{
    // hud.cpp already resolved the glyph and encoded the lanes; emit verbatim.
    emit_guidance(cur.dir_icon,
                  cur.distance_dec,
                  cur.distance_unit,
                  cur.road_name,
                  cur.lanes);
}

// Bring up the service-bus connection on the sender thread. Returns
// true once connected; false on failure or stop-during-setup. We open
// our OWN private libdbus connection (not shared with libjcidbus),
// register with the bus, and disable exit-on-disconnect so a torn
// socket on a source switch never raises a fatal signal.
bool sender_setup()
{
    if (g_stop.load(std::memory_order_relaxed)) {
        LOGD("svcnavi sender: stop requested before connect — aborting setup");
        return false;
    }

    const char *addr = getenv("JCI_SERVICE_BUS");
    if (addr == nullptr || addr[0] == '\0') {
        addr = kServiceBusFallback;
        LOGW("svcnavi sender: $JCI_SERVICE_BUS unset — falling back to %s", addr);
    }

    g_conn = dbus_connection_open_private(addr, nullptr);
    if (g_conn == nullptr) {
        LOGC("svcnavi sender: dbus_connection_open_private(%s) failed "
             "(libdbus unavailable or bus down) — no HUD this session", addr);
        return false;
    }

    // exit_on_disconnect FALSE before register, so a disconnect during
    // bring-up can't take the process down.
    dbus_connection_set_exit_on_disconnect(g_conn, 0);

    if (dbus_bus_register(g_conn, nullptr) == 0) {
        LOGE("svcnavi sender: dbus_bus_register failed — no HUD this session");
        dbus_connection_close(g_conn);
        dbus_connection_unref(g_conn);
        g_conn = nullptr;
        return false;
    }

    LOGD("svcnavi sender: service bus connected (%s), registered", addr);
    return !g_stop.load(std::memory_order_relaxed);
}

// Clear the HUD (blank guidance) and release the connection. Runs on
// the sender thread as it winds down.
void sender_teardown()
{
    if (g_conn == nullptr) {
        return;
    }

    // Emit a zeroed guidance frame so svcjcinavi blanks the HUD
    // instead of holding our last maneuver. dirIcon 0, dist 0, empty
    // street, no lanes; sentinel speed still marks it as ours.
    emit_guidance(0, 0, 0, "", nullptr);
    LOGD("svcnavi sender: sent blanking GuidanceChangedForHUD");

    // Private connections must be closed before the final unref.
    dbus_connection_close(g_conn);
    dbus_connection_unref(g_conn);
    g_conn = nullptr;
    LOGD("svcnavi sender: connection closed and freed");
}

void *sender_main(void *)
{
    if (!sender_setup()) {
        sender_teardown();
        LOGD("svcnavi sender: setup did not complete; thread exiting");
        return nullptr;
    }

    g_active.store(true, std::memory_order_release);
    LOGD("svcnavi sender: HUD plumbing ready");

    uint32_t last_processed = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        NaviSnapshot snap;
        uint32_t s1, s2;
        for (;;) {
            s1 = g_seq.load(std::memory_order_acquire);
            if (s1 & 1u) { sched_yield(); continue; }
            std::memcpy(&snap, &g_snapshot, sizeof(snap));
            std::atomic_thread_fence(std::memory_order_acquire);
            s2 = g_seq.load(std::memory_order_relaxed);
            if (s1 == s2) break;
        }

        if (s2 != last_processed) {
            send_one(snap);
            last_processed = s2;
        }

        std::unique_lock<std::mutex> lk(g_cv_mu);
        g_cv.wait(lk, [&]() {
            return g_stop.load(std::memory_order_relaxed) ||
                   g_seq.load(std::memory_order_acquire) != last_processed;
        });
    }

    LOGD("svcnavi sender: stop requested — winding down (last seq=%u)",
         static_cast<unsigned>(last_processed));
    g_active.store(false, std::memory_order_release);
    sender_teardown();
    LOGD("svcnavi sender: thread exiting cleanly");
    return nullptr;
}

inline void seqlock_begin() { g_seq.fetch_add(1, std::memory_order_acq_rel); }
inline void seqlock_end()   { g_seq.fetch_add(1, std::memory_order_acq_rel);
                              g_cv.notify_one(); }

std::atomic<uint32_t> g_dropped_inactive{0};

void note_inactive_drop(const char *which)
{
    uint32_t n = g_dropped_inactive.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1u || (n & 0xffu) == 0u) {
        LOGW("svcnavi producer: %s dropped — sender pipeline not active "
             "(g_active=false); %u nav event(s) dropped so far",
             which, static_cast<unsigned>(n));
    }
}

} // namespace

// === Lifecycle (called from hud.cpp via the transport dispatch) ===

void svcnavi_tx_start(void)
{
    if (g_sender_thread_up) {
        LOGD("svcnavi_tx_start: already running");
        return;
    }

    g_stop.store(false, std::memory_order_release);
    g_active.store(false, std::memory_order_release);

    if (preload_thread_create(&g_sender_thread, sender_main, nullptr) != 0) {
        LOGC("svcnavi_tx_start: failed to spawn sender thread");
        return;
    }
    g_sender_thread_up = true;
    LOGD("svcnavi_tx_start: sender thread spawned (D-Bus setup deferred)");
}

void svcnavi_tx_stop(void)
{
    if (!g_sender_thread_up) {
        return;
    }

    g_stop.store(true, std::memory_order_release);
    g_cv.notify_all();

    pthread_join(g_sender_thread, nullptr);
    g_sender_thread_up = false;
    g_sender_thread    = 0;
    g_active.store(false, std::memory_order_release);
    LOGD("svcnavi_tx_stop: sender thread stopped, connection released");
}

// === Producer side (runs on the SDK callback thread) ==========

void svcnavi_tx_status(uint32_t status)
{
    if (!g_active.load(std::memory_order_relaxed)) {
        note_inactive_drop("status (0x500)");
        return;
    }

    if (status != 1u) {
        // STOP (or any non-START): zero the snapshot so send_one
        // emits a blanking signal.
        seqlock_begin();
        std::memset(&g_snapshot, 0, sizeof(g_snapshot));
        seqlock_end();
    } else {
        g_cv.notify_one();
    }
}

void svcnavi_tx_next_turn(const char *road_name, uint32_t dir_icon)
{
    if (!g_active.load(std::memory_order_relaxed)) {
        note_inactive_drop("next_turn (0x501)");
        return;
    }

    seqlock_begin();
    libpatch::copy_utf8_truncated(g_snapshot.road_name,
                                  sizeof(g_snapshot.road_name),
                                  road_name);
    // Relay: hud.cpp already resolved the Mazda glyph. The 1.5 path carries no
    // lanes (the snapshot's lane bytes default to all-hidden).
    g_snapshot.dir_icon = dir_icon;
    seqlock_end();
}

void svcnavi_tx_distance(int32_t dist_dec, uint8_t dist_unit)
{
    if (!g_active.load(std::memory_order_relaxed)) {
        note_inactive_drop("distance (0x502)");
        return;
    }

    seqlock_begin();
    g_snapshot.distance_dec  = dist_dec;
    g_snapshot.distance_unit = dist_unit;
    seqlock_end();
}

void svcnavi_tx_lanes(const uint8_t *lanes)
{
    if (!g_active.load(std::memory_order_relaxed)) {
        note_inactive_drop("lanes");
        return;
    }

    seqlock_begin();
    if (lanes) {
        std::memcpy(g_snapshot.lanes, lanes, sizeof(g_snapshot.lanes));
    } else {
        std::memset(g_snapshot.lanes, OEM_LANE_NONE, sizeof(g_snapshot.lanes));
    }
    seqlock_end();
}
