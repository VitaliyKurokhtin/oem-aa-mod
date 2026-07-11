// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Adapted from headunit (https://github.com/Trevelopment/headunit),
// licensed under GNU AGPL v3.
// See NOTICE.md at the repo root for the full attribution.
//
// HUD output module — D-Bus client that converts the AAP nav events
// surfaced by hud.cpp into Mazda "Active Driving Display" HUD frames.
//
// === What this file does =====================================
//
// On every 0x500 / 0x501 / 0x502 event the SDK delivers to hud.cpp,
// hud.cpp calls one of the `vbs_tx_*` push functions in vbs_tx.h.
// Those run on the SDK protobuf-decode thread and MUST NOT block,
// so all they do here is:
//
//   1. Grab a seqlock-protected snapshot struct (g_snapshot).
//   2. Write the new fields into it.
//   3. Bump g_seq (acq_rel) twice, framing the write between odd
//      and even values so a reader can detect torn snapshots.
//   4. Notify the sender thread's condition variable.
//
// The dedicated sender thread (`sender_main` below) then:
//
//   1. Wakes from the cv.
//   2. Re-reads the snapshot via the standard seqlock pattern.
//   3. Diffs against the previous snapshot to decide which OEM
//      D-Bus calls are warranted (typically `SetHUDDisplayMsgReq`
//      for the maneuver/distance block, `SetHUD_Display_Msg2` for
//      a road-name change).
//   4. Calls the proxies and goes back to sleep.
//
// Threading is intentionally seqlock-with-cv rather than a SPSC
// ring because the HUD only cares about the LATEST state — if the
// phone sends 20 distance updates while the sender thread is busy
// with one D-Bus round-trip, we coalesce them into a single send
// of the latest values.

#define LOG_TAG "HUD"
#include "../log.h"
#include "vbs_tx.h"
#include "hud_nav.h"
#include "../oem/libjcivbsnaviclient.h"
#include "common/config.h"   // libpatch_config::hud_lanes()

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <string>
#include <atomic>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace {

// Well-known name we acquire on the service bus. libjcidbus's
// conn_by_env_name registers it via dbus_bus_request_name; a novel
// name avoids colliding with the real com.jci.vbs.navi *service*
// (which we only send method calls to, never host).
//
// The OEM D-Bus ABI (libjcidbus + libjcivbsnaviclient typedefs, the
// VbsNaviHud* wire structs, the resolved g_oem table) and its
// dlopen/dlsym symbol resolution live in
// oem/libjcivbsnaviclient.{h,cpp}.
constexpr const char *kBusName = "com.jci.aapa.hud";

// Empty-slot sentinel for VBS_NAVI_SetRecommLaneReq's (ay) lane array: 0xFF hides a
// slot. This is the OEM VBS convention for that method (svcjcinavi's own upstream
// GuidanceChangedForHUD path uses 0, validated 0..0x46 — a different domain).
constexpr uint8_t kVbsLaneHidden = 0xFF;

// The HUD turn-icon enum, kTurnIcons table, roundabout_icon(),
// map_distance_unit(), and compute_turn_icon() are shared with the
// svcnavi transport — see hud_nav.h.

// === Shared snapshot (seqlock-protected) ======================
//
// Single producer (the AAP cb), single consumer (the sender
// thread). g_seq is the seqlock: even = consistent / readable,
// odd = write in progress. We never block writers, never block
// readers; readers retry on torn snapshots.
struct NaviSnapshot {
    char     road_name[64];      // NUL-terminated UTF-8
    uint32_t turn_side;          // proto TURN_SIDE: 1=L, 2=R, 3=U
    uint32_t turn_event;         // proto TURN_EVENT: 0..19 sparse
    int32_t  turn_angle;         // degrees, signed
    int32_t  turn_number;        // maneuver / exit number
    int32_t  distance_dec;       // value * 10 (one decimal place)
    uint8_t  distance_unit;      // ALREADY mapped to Mazda enum
    int32_t  time_until;         // seconds — ETA to next maneuver
    // GAL 1.6 path: glyph precomputed (Mazda code) instead of derived from
    // turn_event/side/angle. The 1.5 path leaves use_precomp false (zeroed).
    bool        use_precomp;
    uint32_t    precomp_icon;
    uint8_t     n_lanes;
    AaLane lanes[HUD_NAV16_MAX_LANES];
};

NaviSnapshot           g_snapshot   = {};
std::atomic<uint32_t>  g_seq{0};
std::condition_variable g_cv;
std::mutex             g_cv_mu;
std::atomic<bool>      g_active{false};   // sender pipeline live: producers may write
std::atomic<bool>      g_stop{false};     // stop requested: sender thread should wind down

// Shared sender-thread handle (both backends run the same loop).
pthread_t g_sender_thread       = 0;
bool      g_sender_thread_up    = false;

// === OEM connection state =====================================
//
// The connection handle is created and owned by the sender thread
// for its whole lifetime. libjcidbus spawns its OWN worker thread
// (JCIDBUS_worker_start) to do socket I/O, so — unlike the old
// dbus-c++ path — we run no event loop ourselves and have nothing
// polling a dead socket after a source switch.
void *g_conn = nullptr;

// HUD-present gate, fail-OPEN. Default false (= "not known absent",
// i.e. send). A one-shot VBS_NAVI_GetHUDStatus reply flips this to
// true on vehicles with no HUD. Fail-open means a lost/late reply
// never silences us for the whole drive (the failure mode of the
// old synchronous probe); the cost is a few harmless frames on a
// no-HUD car until the reply lands (the OEM module just returns an
// error byte and forwards nothing). sm recycles the PID, which
// re-probes, so a per-process latch is fine.
std::atomic<bool> g_hud_absent{false};

// libjcidbus connection error callback (stored at conn+0x10 and
// invoked by the library as cb(conn, closure) on connection-level
// errors). Must be non-NULL — the library dereferences and calls
// it. Keep it trivial and non-throwing; with exit-on-disconnect
// disabled, a bus drop is no longer fatal.
extern "C" void hud_dbus_error_cb(void *conn, void *closure)
{
    (void)conn; (void)closure;
    LOGW("hud sender: libjcidbus signalled a service-bus connection "
         "error (non-fatal; exit-on-disconnect is off)");
}

// VBS_NAVI_GetHUDStatus async reply. ABI from the decompiled
// internal trampoline: cb(conn, status_byte, user). A zero status
// byte means the HUD is not installed / not powered.
extern "C" void hud_status_cb(void *conn, unsigned char status, void *user)
{
    (void)conn; (void)user;
    const bool absent = (status == 0);
    g_hud_absent.store(absent, std::memory_order_release);
    LOGD("hud sender: GetHUDStatus reply = %u (%s)",
         static_cast<unsigned>(status), absent ? "no HUD" : "HUD present");
}

// Which code->glyph table to map lanes through. Forced to Table A for now.
OemLaneTable vbs_lane_table() { return OEM_LANE_TABLE_A; }

// Helper: snap vs prev → which OEM HUD calls to make. Pulled out
// for readability; sender_main owns the prev/sync_bit state and
// threads them through here. Sends are async (the OEM setters
// marshal and queue, then return); on a non-zero return we just
// log and let the next change retry.
void send_one(const NaviSnapshot &cur,
              const NaviSnapshot &prev,
              uint8_t            &sync_bit)
{
    // HUD reported absent by GetHUDStatus — drain the snapshot but
    // emit nothing (cheap; producers keep updating it regardless).
    if (g_hud_absent.load(std::memory_order_acquire)) {
        return;
    }

    // The OEM's NAVI_SendHUDGuidaceDataToVBS bumps syncBitCount (and
    // re-latches the Msg2 street page) on a new *maneuver instance* —
    // i.e. when the maneuver icon changes OR the street name changes —
    // not on distance/speed ticks. Mirror that: a new road name or a
    // new resolved turn-icon both start a new instance.
    // Resolve the glyph: precomputed (1.6 path) or derived from the turn fields
    // (1.5 path). Compute both cur and prev so the change-detection below sees
    // glyph changes on either path.
    uint32_t cur_icon  = cur.use_precomp
                             ? cur.precomp_icon
                             : compute_turn_icon(cur.turn_event, cur.turn_side, cur.turn_angle);
    uint32_t prev_icon = prev.use_precomp
                             ? prev.precomp_icon
                             : compute_turn_icon(prev.turn_event, prev.turn_side, prev.turn_angle);

    bool event_changed = (std::strncmp(cur.road_name, prev.road_name,
                                       sizeof(cur.road_name)) != 0) ||
                         (cur_icon != prev_icon);
    bool distance_changed = event_changed ||
                            cur.distance_dec  != prev.distance_dec  ||
                            cur.distance_unit != prev.distance_unit ||
                            cur.time_until    != prev.time_until    ||
                            cur.turn_angle    != prev.turn_angle    ||
                            cur.turn_side     != prev.turn_side     ||
                            cur.turn_event    != prev.turn_event;

    // Lane strip: the direct-VBS path is downstream of svcjcinavi, so it applies
    // svcjcinavi's own code->glyph + A/B (basic/extended cluster) map itself and
    // sends the final glyph bytes on VBS_NAVI_SetRecommLaneReq (below). Computed
    // here too so a lane-only change still wakes a send — and, because the strip
    // latches to the sync bit, that change also bumps the sync (see send_msg2).
    const bool lanes_changed = libpatch_config::hud_lanes() &&
        (event_changed ||
         cur.n_lanes != prev.n_lanes ||
         std::memcmp(cur.lanes, prev.lanes,
                     cur.n_lanes * sizeof(cur.lanes[0])) != 0);

    if (!event_changed && !distance_changed && !lanes_changed) {
        LOGV("hud_send: snapshot unchanged vs previous — no HUD frame sent");
        return;
    }

    // Plain-C OEM wire structs (no dbus-c++ marshalling types now).
    //   VbsNaviHudDisplay (uqyqyy): maneuver icon + distance block.
    //   VbsNaviHudMsg2    (sy):     street-name strip.
    // `road` owns the bytes msg2.guidancePointName points at; it must
    // outlive the set_hud_msg2() call below (it does — the library
    // copies the string while marshalling).
    VbsNaviHudDisplay disp = {};
    VbsNaviHudMsg2    msg2 = {};
    std::string       road;

    // A new maneuver instance (icon/street change) OR a lane change bumps the sync:
    // the lane strip latches to the sync bit (SetRecommLaneReq has no sync field of
    // its own), so a lane update must ride a fresh generation. Distance-only ticks
    // stay in the current one.
    bool send_msg2 = event_changed || lanes_changed; // street strip (carries the sync)
    bool send_disp = distance_changed || send_msg2;  // maneuver/distance frame

    if (send_msg2) {
        // Bump 1..7 cyclically — the HUD treats a new sync_bit as the start of a
        // new maneuver/street/lane generation.
        sync_bit = static_cast<uint8_t>((sync_bit % 7) + 1);
        road = cur.road_name;
        msg2.guidancePointName = road.c_str();
        msg2.syncBit           = sync_bit;
    }

    if (send_disp) {
        uint32_t icon = cur_icon;

#ifdef DEBUG
        // Debug aid: when our mapping produced no glyph for this
        // (turn_event, turn_side) pair, hijack the street-name strip
        // to show the raw codes so an observer on the HUD can record
        // them and extend the table later. Stripped in release. Skipped
        // on the 1.6 path (use_precomp): glyph 0 there is a real "blank",
        // and turn_event/side carry no meaning to display.
        char dbg_label[33];
        if (icon == 0 && !cur.use_precomp) {
            snprintf(dbg_label, sizeof(dbg_label),
                     "EV=%u S=%u A=%d",
                          static_cast<unsigned>(cur.turn_event),
                          static_cast<unsigned>(cur.turn_side),
                          static_cast<int>(cur.turn_angle));
            if (!send_msg2) {
                sync_bit = static_cast<uint8_t>((sync_bit % 7) + 1);
            }
            road = dbg_label;
            msg2.guidancePointName = road.c_str();
            msg2.syncBit           = sync_bit;
            icon = MazdaIcon::HUD_EMPTY;
            send_msg2 = true;   // force Msg2 send below
        }
#endif

        disp.nextManeuverInfo  = icon;
        disp.distanceValue     = static_cast<uint16_t>(cur.distance_dec);
        disp.distanceUnit      = cur.distance_unit;
        disp.displaySpeedLimit = 0;     // speed limit — unused here
        disp.displaySpeedUnit  = 0;     // speed units — unused here
        disp.text_ID3          = sync_bit;
        // Lanes are NOT part of this uqyqyy frame — they go on their own OEM
        // method, VBS_NAVI_SetRecommLaneReq ((ay)), sent below in the same sync
        // generation.
    }

    if (send_disp) {
        LOGV("hud_send: SetHUDDisplayMsgReq icon=%u dist=%u unit=%u sync=%u",
             static_cast<unsigned>(disp.nextManeuverInfo),
             static_cast<unsigned>(disp.distanceValue),
             static_cast<unsigned>(disp.distanceUnit),
             static_cast<unsigned>(disp.text_ID3));
        int rc = VBS_NAVI_SetHUDDisplayMsgReq(g_conn, &disp, nullptr, nullptr, nullptr);
        if (rc != 0) {
            LOGE("hud_send: SetHUDDisplayMsgReq failed rc=%d", rc);
        }
    }
    if (send_msg2) {
        LOGV("hud_send: SetHUD_Display_Msg2 road=\"%s\" sync=%u",
             road.c_str(), static_cast<unsigned>(msg2.syncBit));
        int rc = VBS_NAVI_TMC_SetHUD_Display_Msg2(g_conn, &msg2, nullptr, nullptr, nullptr);
        if (rc != 0) {
            LOGE("hud_send: SetHUD_Display_Msg2 failed rc=%d", rc);
        }
    }
    // Lane strip — a separate OEM method ((ay)); 0xFF hides an empty slot. We do
    // svcjcinavi's own code->glyph + A/B map here (it bypasses svcjcinavi).
    if (lanes_changed) {
        const OemLaneTable tbl = vbs_lane_table();
        // Same OEM lane CODES the svcnavi path emits (no duplication), then apply
        // svcjcinavi's own code->glyph map (A/B) in place — empty slots stay 0xFF.
        uint8_t lb[8];
        aa_nav16_lane_codes(cur.lanes, cur.n_lanes, lb, kVbsLaneHidden);
        for (int i = 0; i < 8; ++i) {
            if (lb[i] != kVbsLaneHidden) {
                lb[i] = oem_lane_glyph((OemLaneCode)lb[i], tbl);
            }
        }
        LOGV("hud_send: SetRecommLaneReq table=%s lanes=[%u,%u,%u,%u,%u,%u,%u,%u]",
             tbl == OEM_LANE_TABLE_B ? "B" : "A",
             lb[0], lb[1], lb[2], lb[3], lb[4], lb[5], lb[6], lb[7]);
        const uint8_t *lane_data  = lb;
        const uint32_t lane_count = sizeof(lb);
        int rc = VBS_NAVI_SetRecommLaneReq(g_conn, &lane_data, &lane_count, nullptr, nullptr);
        if (rc != 0) LOGE("hud_send: SetRecommLaneReq failed rc=%d", rc);
    }
}

// Bring up the OEM service-bus connection the sender needs. Runs ON
// the sender thread so session start stays cheap. Creates + connects
// the libjcidbus connection (which disables exit-on-disconnect for
// us), starts its worker, and fires a one-shot fail-open HUD-presence
// probe. Returns true once the connection is up; false if the OEM
// stack is unavailable, the connect failed, or a stop was requested
// mid-setup. (The OEM entry points self-resolve on first call and
// return a benign failure value if unavailable — see
// oem/libjcivbsnaviclient.h — so there is no separate resolve step.)
bool sender_setup()
{
    if (g_stop.load(std::memory_order_relaxed)) {
        LOGD("hud sender: stop requested before connect — setup aborting");
        return false;
    }

    // Create the connection object. error_cb must be non-NULL. A NULL
    // return also covers "OEM D-Bus symbols unavailable".
    g_conn = JCIDBUS_conn_create(reinterpret_cast<void *>(&hud_dbus_error_cb), 0);
    if (g_conn == nullptr) {
        LOGC("hud sender: JCIDBUS_conn_create failed (OEM symbols "
             "unavailable?) — no HUD this session");
        return false;
    }

    // Connect to the SERVICE bus ($JCI_SERVICE_BUS) and acquire our
    // well-known name. conn_connect returns non-zero on success;
    // internally it sets exit-on-disconnect FALSE — the whole point.
    LOGD("hud sender: connecting to service bus as %s", kBusName);
    if (JCIDBUS_conn_connect(g_conn, kBusName, kJciServiceBus, nullptr) == 0) {
        LOGE("hud sender: JCIDBUS_conn_connect failed (name not acquired "
             "or $JCI_SERVICE_BUS unset) — no HUD this session");
        JCIDBUS_conn_free(g_conn);
        g_conn = nullptr;
        return false;
    }

    // Start libjcidbus's own I/O worker thread.
    JCIDBUS_worker_start(g_conn);
    LOGD("hud sender: service bus connected, worker started");

    if (g_stop.load(std::memory_order_relaxed)) {
        LOGD("hud sender: stop requested after connect — setup aborting");
        return false;
    }

    // Fail-open HUD-presence probe: one async GetHUDStatus. The reply
    // (hud_status_cb) flips g_hud_absent only if there is no HUD.
    VBS_NAVI_GetHUDStatus(g_conn, reinterpret_cast<void *>(&hud_status_cb),
                          nullptr);
    LOGD("hud sender: HUD-status probe sent (sending enabled, fail-open)");
    return true;
}

// Clear the HUD and release the OEM connection. Runs on the sender
// thread as it winds down (it owns the connection), so the stop path
// on the lifecycle thread only has to signal + join. Safe to call
// when setup never completed: a NULL g_conn skips everything.
void sender_teardown()
{
    if (g_conn == nullptr) {
        return;
    }

    // Send one zeroed-out HUD frame so the display clears instead of
    // holding whatever maneuver was last shown when the phone
    // disconnected. Skipped when the HUD is known absent. We don't
    // touch the street strip (Msg2) — it fades on its own when the
    // main frame blanks, and an empty-string Msg2 is non-trivial
    // (the producer hard-caps at 1 page).
    if (!g_hud_absent.load(std::memory_order_acquire)) {
        VbsNaviHudDisplay clear = {};   // all fields zero
        int rc = VBS_NAVI_SetHUDDisplayMsgReq(g_conn, &clear, nullptr, nullptr, nullptr);
        if (rc != 0) {
            LOGE("hud sender: clear-frame send failed rc=%d", rc);
        } else {
            LOGD("hud sender: sent HUD clear frame");
        }
        // Hide any lanes a 1.6 session left on the HUD (all slots hidden).
        // Only when lane sends are enabled — none were shown otherwise.
        if (libpatch_config::hud_lanes()) {
            uint8_t clear_lanes[8];
            std::memset(clear_lanes, kVbsLaneHidden, sizeof(clear_lanes));
            const uint8_t *lane_data  = clear_lanes;
            const uint32_t lane_count = sizeof(clear_lanes);
            VBS_NAVI_SetRecommLaneReq(g_conn, &lane_data, &lane_count,
                                      nullptr, nullptr);
        }
        // The frames are queued on the libjcidbus worker; give them
        // a brief moment to flush before we stop the worker below.
        usleep(50 * 1000);
    }

    // Clean teardown — the thing the old dbus-c++ dispatcher never
    // did (it was left polling a dead socket, which is what crashed
    // the process). Stop the worker, drop the bus name, free it.
    JCIDBUS_worker_stop(g_conn);
    JCIDBUS_conn_disconnect(g_conn);
    JCIDBUS_conn_free(g_conn);
    g_conn = nullptr;
    LOGD("hud sender: worker stopped, connection freed");
}

void *sender_main(void *)
{
    if (!sender_setup()) {
        // HUD absent, setup failed, or stop during setup. Leave
        // g_active false so producers keep ignoring events; clean up
        // anything sender_setup may have left behind on a stop race.
        sender_teardown();
        LOGD("hud sender: setup did not complete; thread exiting");
        return nullptr;
    }

    // Pipeline is live — let the producers start filling snapshots.
    g_active.store(true, std::memory_order_release);
    LOGD("hud sender: HUD plumbing ready");

    NaviSnapshot prev = {};
    uint8_t      sync_bit = 0;
    uint32_t     last_processed = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        // Seqlock read of g_snapshot. Standard pattern: snapshot
        // is consistent iff seq is even before AND after the
        // memcpy AND they match. Retry forever on tear.
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
            send_one(snap, prev, sync_bit);
            prev = snap;
            last_processed = s2;
        }

        // Predicated wait — wake when seq advances past what we
        // just processed, or when shutdown is requested. Closes
        // the lost-wakeup race window between the snapshot read
        // and the wait.
        std::unique_lock<std::mutex> lk(g_cv_mu);
        g_cv.wait(lk, [&]() {
            return g_stop.load(std::memory_order_relaxed) ||
                   g_seq.load(std::memory_order_acquire) != last_processed;
        });
    }

    // Stop the producers from writing, then clear the HUD and
    // release the clients we own.
    LOGD("hud sender: stop requested — winding down (last processed seq=%u)",
         static_cast<unsigned>(last_processed));
    g_active.store(false, std::memory_order_release);
    sender_teardown();
    LOGD("hud sender: thread exiting cleanly");
    return nullptr;
}

// Seqlock write helpers used by vbs_tx_* below.
inline void seqlock_begin() { g_seq.fetch_add(1, std::memory_order_acq_rel); }
inline void seqlock_end()   { g_seq.fetch_add(1, std::memory_order_acq_rel);
                              g_cv.notify_one(); }

// Diagnostic: nav events arriving while the sender pipeline is not
// live (g_active==false) are silently dropped by the vbs_tx_*
// producers below. The SDK callback fires at high rate, so rate-limit
// the log: emit on the first drop and every 256th thereafter, carrying
// the running total. A session that keeps accumulating drops but never
// logs "HUD plumbing ready" never brought the pipeline up — the whole
// drive shows no HUD output. (A healthy session drops only a handful
// during the brief start-up window before g_active flips true, then
// the count stops climbing.)
std::atomic<uint32_t> g_dropped_inactive{0};

void note_inactive_drop(const char *which)
{
    uint32_t n = g_dropped_inactive.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1u || (n & 0xffu) == 0u) {
        LOGW("hud producer: %s dropped — sender pipeline not active "
             "(g_active=false); %u nav event(s) dropped so far",
             which, static_cast<unsigned>(n));
    }
}

} // namespace

// === Lifecycle (called from lifecycle.cpp PLT shims) ==========

void vbs_tx_start(void)
{
    if (g_sender_thread_up) {
        LOGD("vbs_tx_start: already running");
        return;
    }

    // Keep session start cheap: spawn the sender thread and return
    // immediately. ALL D-Bus work — dispatcher init, the synchronous
    // HUD-presence probe, and the service-bus attach — happens on
    // that thread (sender_setup). If the HUD is absent the thread
    // just exits and nav events are ignored for this session.
    g_stop.store(false, std::memory_order_release);
    g_active.store(false, std::memory_order_release);

    if (pthread_create(&g_sender_thread, nullptr,
                       sender_main, nullptr) != 0) {
        LOGC("vbs_tx_start: failed to spawn sender thread");
        return;
    }
    g_sender_thread_up = true;
    LOGD("vbs_tx_start: sender thread spawned (D-Bus setup deferred to thread)");
}

void vbs_tx_stop(void)
{
    if (!g_sender_thread_up) {
        return;  // not running
    }

    // Signal stop and wake the sender from its cv wait. If it's still
    // mid-setup (e.g. blocked in the synchronous HUD-presence probe),
    // it picks the flag up at the next checkpoint. The sender thread
    // sends the HUD clear frame and releases the D-Bus clients itself
    // on its way out (sender_teardown), so here we only signal + join.
    g_stop.store(true, std::memory_order_release);
    g_cv.notify_all();

    pthread_join(g_sender_thread, nullptr);
    g_sender_thread_up = false;
    g_sender_thread    = 0;
    g_active.store(false, std::memory_order_release);
    LOGD("vbs_tx_stop: sender thread stopped, D-Bus clients released");
}

// === Producer side (runs on the SDK callback thread) ==========

void vbs_tx_status(uint32_t status)
{
    // No sender running (HUD not installed, or start failed) —
    // hud.cpp's dump_status() already logged the event's arrival;
    // record the drop (rate-limited) so a stuck session is visible.
    if (!g_active.load(std::memory_order_relaxed)) {
        note_inactive_drop("status (0x500)");
        return;
    }

    // Per hu.proto NAVMessagesStatus: START=1, STOP=2.
    // Treat STOP (or any non-START) as "clear the HUD": zero the
    // snapshot so the sender's diff produces a blank frame.
    if (status != 1u) {
        seqlock_begin();
        std::memset(&g_snapshot, 0, sizeof(g_snapshot));
        seqlock_end();
    } else {
        // Just wake the sender so it re-evaluates promptly on
        // route start.
        g_cv.notify_one();
    }
}

void vbs_tx_next_turn(const char *road_name,
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

void vbs_tx_distance(int32_t  /*distance_meters*/,
                     int32_t  time_until_seconds,
                     int32_t  display_distance,
                     uint32_t display_distance_unit)
{
    if (!g_active.load(std::memory_order_relaxed)) {
        note_inactive_drop("distance (0x502)");
        return;
    }

    seqlock_begin();
    // raw_unit * 1000 (proto) → raw_unit * 10 (HUD wire).
    g_snapshot.distance_dec  = display_distance / 100;
    g_snapshot.distance_unit = map_distance_unit(display_distance_unit);
    g_snapshot.time_until    = time_until_seconds;
    seqlock_end();
}

void vbs_tx_v16(uint32_t glyph, const char *road,
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
