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
#include "../oem/libdbus.h"
#include "../../common/oem/vbs_navi_hud.h"   // kAapSpeedSentinel (shared with svcjcinavi)
#include "common/config.h"                   // libpatch_config::hud_lanes()

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

// The HUD turn-icon enum, kTurnIcons table, roundabout_icon(),
// map_distance_unit(), and compute_turn_icon() are shared with the
// vbs transport — see hud_nav.h.

// === Shared snapshot (seqlock-protected) ======================
// Single producer (SDK cb), single consumer (sender thread). Same
// pattern as vbs_tx.cpp.
struct NaviSnapshot {
    char     road_name[64];
    uint32_t turn_side;
    uint32_t turn_event;
    int32_t  turn_angle;
    int32_t  turn_number;
    int32_t  distance_dec;   // value * 10
    uint8_t  distance_unit;  // mapped to Mazda enum
    int32_t  time_until;
    // GAL 1.6 path: the glyph is precomputed (Mazda code) rather than derived
    // from turn_event/side/angle, and lanes are carried through. The 1.5 path
    // leaves use_precomp false (zeroed), so it keeps using compute_turn_icon().
    bool        use_precomp;
    uint32_t    precomp_icon;
    uint8_t     n_lanes;
    AaLane lanes[HUD_NAV16_MAX_LANES];
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
                   const AaLane *lanes, uint8_t n_lanes)
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
    // lane0..7: one OEM lane code per lane (see hud_lane.h) — svcjcinavi remaps
    // each code to a cluster glyph itself (basic/extended cluster chosen by
    // CMU-control 0x18), so we never deal in glyphs here. Empty slots are 0, not
    // 0xFF: svcjcinavi validates each lane arg to 0..0x46 and would reject 0xFF.
    // The 1.5 path passes lanes=null / n=0, so all eight come out 0 (no lanes).
    uint8_t lb[8];
    aa_nav16_lane_codes(lanes, n_lanes, lb, /*hidden=*/0);
    for (int i = 0; i < 8 && ok; ++i) {
        ok = append_i32(&it, static_cast<int32_t>(lb[i]));
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
             "street=\"%s\" speedLimit=0x%x lanes=%u,%u,%u,%u,%u,%u,%u,%u",
             static_cast<unsigned>(dir_icon), maneuver_dist, dist_unit, s,
             static_cast<unsigned>(kAapSpeedSentinel),
             (unsigned)lb[0], (unsigned)lb[1], (unsigned)lb[2], (unsigned)lb[3],
             (unsigned)lb[4], (unsigned)lb[5], (unsigned)lb[6], (unsigned)lb[7]);
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
    uint32_t icon = cur.use_precomp
                        ? cur.precomp_icon
                        : compute_turn_icon(cur.turn_event, cur.turn_side, cur.turn_angle);

    // Lane forwarding shares the hud_lanes kill-switch with the vbs
    // transport; off -> all-zero lane slots ("no lane"), byte-identical
    // to the 1.5 path.
    const bool send_lanes = libpatch_config::hud_lanes() && cur.use_precomp;
    emit_guidance(icon,
                  cur.distance_dec,
                  cur.distance_unit,
                  cur.road_name,
                  send_lanes ? cur.lanes : nullptr,
                  send_lanes ? cur.n_lanes : 0);
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
    emit_guidance(0, 0, 0, "", nullptr, 0);
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

    if (pthread_create(&g_sender_thread, nullptr, sender_main, nullptr) != 0) {
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

void svcnavi_tx_next_turn(const char *road_name,
                          uint32_t    turn_side,
                          uint32_t    turn_event,
                          int32_t     turn_angle,
                          int32_t     turn_number)
{
    if (!g_active.load(std::memory_order_relaxed)) {
        note_inactive_drop("next_turn (0x501)");
        return;
    }

    seqlock_begin();
    if (road_name) {
        std::strncpy(g_snapshot.road_name, road_name,
                     sizeof(g_snapshot.road_name) - 1);
        g_snapshot.road_name[sizeof(g_snapshot.road_name) - 1] = '\0';
    } else {
        g_snapshot.road_name[0] = '\0';
    }
    g_snapshot.turn_side   = turn_side;
    g_snapshot.turn_event  = turn_event;
    g_snapshot.turn_angle  = turn_angle;
    g_snapshot.turn_number = turn_number;
    g_snapshot.use_precomp = false;   // 1.5 path: derive the glyph from turn_event
    seqlock_end();
}

void svcnavi_tx_distance(int32_t  /*distance_meters*/,
                         int32_t  time_until_seconds,
                         int32_t  display_distance,
                         uint32_t display_distance_unit)
{
    if (!g_active.load(std::memory_order_relaxed)) {
        note_inactive_drop("distance (0x502)");
        return;
    }

    seqlock_begin();
    g_snapshot.distance_dec  = display_distance / 100;  // *1000 -> *10
    g_snapshot.distance_unit = map_distance_unit(display_distance_unit);
    g_snapshot.time_until    = time_until_seconds;
    seqlock_end();
}

void svcnavi_tx_v16(uint32_t glyph, const char *road,
                    int32_t dist_dec, uint8_t dist_unit,
                    const AaLane *lanes, uint8_t n_lanes)
{
    if (!g_active.load(std::memory_order_relaxed)) {
        note_inactive_drop("v16 guidance");
        return;
    }

    uint8_t nl = (n_lanes > HUD_NAV16_MAX_LANES) ? HUD_NAV16_MAX_LANES : n_lanes;

    seqlock_begin();
    if (road) {
        std::strncpy(g_snapshot.road_name, road, sizeof(g_snapshot.road_name) - 1);
        g_snapshot.road_name[sizeof(g_snapshot.road_name) - 1] = '\0';
    } else {
        g_snapshot.road_name[0] = '\0';
    }
    g_snapshot.use_precomp   = true;
    g_snapshot.precomp_icon  = glyph;
    g_snapshot.distance_dec  = dist_dec;
    g_snapshot.distance_unit = dist_unit;
    g_snapshot.n_lanes       = nl;
    for (uint8_t i = 0; i < HUD_NAV16_MAX_LANES; ++i) {
        if (lanes && i < nl) {
            g_snapshot.lanes[i] = lanes[i];
        } else {
            g_snapshot.lanes[i].present_mask   = 0;
            g_snapshot.lanes[i].highlight_mask = 0;
        }
    }
    seqlock_end();
}
