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
#include "hud_lane.h"   // oem_lane_glyph — OEM lane code -> cluster glyph (mapped on this wire)
#include "../oem/libjcivbsnaviclient.h"
#include "common/thread_util.h"

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

// Empty-slot sentinel for VBS_NAVI_SetRecommLaneReq's (ay) lane array: the OEM
// method hides a slot with this byte on the wire. VBS-only — the canonical
// snapshot hidden value is OEM_LANE_NONE (0); we remap 0 -> this at emit.
constexpr uint8_t kVbsLaneHidden = 0xFF;

// map_distance_unit() (distance-unit enum) comes from hud_nav.h, shared with
// the svcnavi transport. The turn-icon mapping (kTurnIcons / compute_turn_icon)
// now lives in hud.cpp — this transport receives an already-resolved glyph.

// === Shared snapshot (seqlock-protected) ======================
//
// Single producer (the AAP cb), single consumer (the sender
// thread). g_seq is the seqlock: even = consistent / readable,
// odd = write in progress. We never block writers, never block
// readers; readers retry on torn snapshots.
struct NaviSnapshot {
    char     road_name[64];      // NUL-terminated UTF-8
    uint32_t dir_icon;           // Mazda HUD maneuver glyph (resolved by hud.cpp)
    int32_t  distance_dec;       // value * 10 (one decimal place)
    uint8_t  distance_unit;      // ALREADY mapped to Mazda enum
    uint8_t  lanes[8];           // OEM lane codes: 0=hidden, 1..70 (mapped to glyphs at emit)
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

// Reset the last HUD snapshot before a new sender session starts.
// If the previous trip left a road name / icon behind, a reconnect can
// display stale guidance until the next nav event arrives.
void reset_sender_state()
{
    std::memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_seq.store(0, std::memory_order_release);
}

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
    // new maneuver glyph both start a new instance. hud.cpp already
    // resolved the glyph, so the diff is a plain field compare.
    uint32_t cur_icon  = cur.dir_icon;
    uint32_t prev_icon = prev.dir_icon;

    bool event_changed = (std::strncmp(cur.road_name, prev.road_name,
                                       sizeof(cur.road_name)) != 0) ||
                         (cur_icon != prev_icon);
    bool distance_changed = event_changed ||
                            cur.distance_dec  != prev.distance_dec  ||
                            cur.distance_unit != prev.distance_unit;

    // Lanes ride a SEPARATE OEM method (SetRecommLaneReq, see below). The
    // snapshot already holds Mazda lane bytes (hud.cpp encoded them; 0=hidden),
    // so just diff the two frames (the 1.5 path never sets lanes -> both
    // all-hidden -> never sent).
    bool lanes_changed = std::memcmp(cur.lanes, prev.lanes, sizeof(cur.lanes)) != 0;

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

    // A lane array carries no sync of its own — the HUD pairs it with the
    // maneuver + street frames of the SAME sync generation. So a lane change
    // starts a new generation just like a maneuver change, and whenever lanes
    // are sent we re-send the maneuver + street frames carrying that same bumped
    // sync (this is what the OEM / lane_test do: all three per generation). A
    // distance-only tick stays in the current generation (no bump, no lanes).
    bool send_msg2  = event_changed || lanes_changed;   // street strip (carries the sync)
    bool send_disp  = distance_changed || send_msg2;     // maneuver/distance frame
    // Re-send lanes with a new sync generation only when there is a lane to
    // show — an all-hidden array has nothing to pair with the generation, and
    // shown->hidden transitions are already covered by lanes_changed.
    bool cur_lanes_visible = false;
    for (size_t i = 0; i < sizeof(cur.lanes); ++i) {
        if (cur.lanes[i] != OEM_LANE_NONE) { cur_lanes_visible = true; break; }
    }
    bool send_lanes = lanes_changed || (send_msg2 && cur_lanes_visible);

    if (send_msg2) {
        // Bump 1..7 cyclically — the HUD treats a new sync_bit as the start of a
        // new maneuver/street/lane generation.
        sync_bit = static_cast<uint8_t>((sync_bit % 7) + 1);
        road = cur.road_name;
        msg2.guidancePointName = road.c_str();
        msg2.syncBit           = sync_bit;
    }

    if (send_disp) {
        disp.nextManeuverInfo  = cur_icon;
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
    if (send_lanes) {
        // Same sync generation as the disp/msg2 just sent (send_msg2 is forced
        // true whenever lanes go out, so sync was bumped above). The HUD ties the
        // lane array to that generation. This transport bypasses svcjcinavi, so it
        // maps each OEM lane code to its cluster glyph here (Table A for now —
        // svcjcinavi's own static default, which renders on any cluster), then hides
        // empty slots with kVbsLaneHidden (0xFF), SetRecommLaneReq's per-slot hide
        // code. The OEM setter dereferences both middle args (see
        // libjcivbsnaviclient.h), so it gets pointers to the data pointer and the count.
        uint8_t wire[8];
        for (int i = 0; i < 8; ++i) {
            uint8_t code = cur.lanes[i];   // OEM lane code, 0 = hidden
            wire[i] = (code == OEM_LANE_NONE)
                          ? kVbsLaneHidden
                          : oem_lane_glyph((OemLaneCode)code, OEM_LANE_TABLE_A);
        }
        const uint8_t *lane_data  = wire;
        const uint32_t lane_count = sizeof(wire);
        int rc = VBS_NAVI_SetRecommLaneReq(g_conn, &lane_data, &lane_count,
                                           nullptr, nullptr);
        LOGV("hud_send: SetRecommLaneReq sync=%u [%u %u %u %u %u %u %u %u] rc=%d",
             static_cast<unsigned>(sync_bit),
             wire[0], wire[1], wire[2], wire[3],
             wire[4], wire[5], wire[6], wire[7], rc);
        if (rc != 0) {
            LOGE("hud_send: SetRecommLaneReq failed rc=%d", rc);
        }
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
        // kVbsLaneHidden (0xFF) is SetRecommLaneReq's per-slot hide code.
        {
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
                              { std::lock_guard<std::mutex> lk(g_cv_mu);
                                g_cv.notify_one(); } }

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

    // Reconnects need a clean slate: if we don't zero the snapshot and the
    // seqlock counter, the sender can keep replaying the previous route's
    // guidance until a new nav event arrives.
    reset_sender_state();

    // Keep session start cheap: spawn the sender thread and return
    // immediately. ALL D-Bus work — dispatcher init, the synchronous
    // HUD-presence probe, and the service-bus attach — happens on
    // that thread (sender_setup). If the HUD is absent the thread
    // just exits and nav events are ignored for this session.
    g_stop.store(false, std::memory_order_release);
    g_active.store(false, std::memory_order_release);

    if (preload_thread_create(&g_sender_thread, sender_main, nullptr) != 0) {
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
    {
        std::lock_guard<std::mutex> lk(g_cv_mu);
        g_stop.store(true, std::memory_order_release);
        g_cv.notify_all();
    }

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
        { std::lock_guard<std::mutex> lk(g_cv_mu);
          g_cv.notify_one(); }
    }
}

void vbs_tx_next_turn(const char *road_name, uint32_t dir_icon)
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
    // Relay: hud.cpp already resolved the Mazda glyph. The 1.5 path carries no
    // lanes (the snapshot's lane bytes default to all-hidden).
    g_snapshot.dir_icon = dir_icon;
    seqlock_end();
}

void vbs_tx_distance(int32_t dist_dec, uint8_t dist_unit)
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

void vbs_tx_lanes(const uint8_t *lanes)
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
