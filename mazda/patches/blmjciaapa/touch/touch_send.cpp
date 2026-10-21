// Action state machine + send path for the touch shim.
//
// Receives evdev MtState snapshots from touch.cpp on every
// SYN_REPORT. Diffs against the previous snapshot to figure out
// which Android-MotionEvent-style events the phone should see, then
// builds an AAP_TouchEvent and hands it to RaceAap::SendTouchInput
// (resolved through the anchor-and-offset machinery in oem/blmjciaapa.h).
//
// The OEM VideoManager::IsAAVideoInFocus gate is enforced before any
// send — this is the only behaviour we cherry-pick from
// HMIEventHandler::InputTouchScreen. SBN handling and contact-id
// dedup are deliberately skipped.

#define LOG_TAG "TOUCH"
#include "../oem/blmjciaapa.h"
#include "../log.h"
#include "touch_send.h"

#include <pthread.h>
#include <string.h>

namespace {

// ---------------------------------------------------------------
// Coordinate scaling.
//
// AAP_TouchEvent expects display pixels (0..799 X, 0..479 Y).
// /dev/input/filtered-touchscreen0 reports raw touch-controller units
// spanning 0..~4099. The OEM HMIEventHandler::InputTouchScreen maps
// these to pixels with fixed constants (float immediates decoded from
// blmjciaapa.so: 0x44480000=800.0, 0x43f00000=480.0, 0x45801800=4099.0):
//
//     x_px = (int)(raw_x * 800.0f / 4099.0f)
//     y_px = (int)(raw_y * 480.0f / 4099.0f)
//
// We reproduce that mapping exactly and unconditionally. There is no
// per-touch heuristic and no runtime axis probe: the panel range is a
// fixed property of this firmware, so hardcoding the stock constants
// guarantees identical behaviour to the native HMI. (A first-touch
// heuristic was tried and removed — it mis-detected a raw panel as
// "already pixels" whenever the first tap landed in the top-left
// region where both raw coords fall below the pixel thresholds,
// collapsing the whole usable area into a corner.) Integer division
// truncates toward zero, matching the OEM's __aeabi_f2iz.
// ---------------------------------------------------------------

constexpr uint32_t kDisplayWidth  = 800;
constexpr uint32_t kDisplayHeight = 480;
constexpr uint32_t kRawMax        = 4099;   // OEM divisor (blmjciaapa.so)

inline uint32_t scale_x(uint32_t v)
{
    return static_cast<uint32_t>(
        static_cast<uint64_t>(v) * kDisplayWidth / kRawMax);
}

inline uint32_t scale_y(uint32_t v)
{
    return static_cast<uint32_t>(
        static_cast<uint64_t>(v) * kDisplayHeight / kRawMax);
}

// ---------------------------------------------------------------
// AAP_TouchEvent build + send.
// ---------------------------------------------------------------

// Build an AAP_TouchEvent from a list of (slot_idx, finger) pairs.
// The output's pointer arrays are filled in input order; the caller
// decides slot ordering. action_index is the array position of the
// "interesting" finger (the one POINTER_UP/POINTER_DOWN refers to);
// pass 0 for DOWN / UP / MOVE.
struct SnapEntry {
    int    slot;            // for debug only
    Finger fg;
};

void send_event(const SnapEntry *entries, int n,
                AAPTouchAction action, int action_index)
{
    if (n <= 0 || n > kMaxFingers) return;

    // Respect the OEM video-focus gate — don't push touches into a
    // phone that isn't currently the foreground video sink. The OEM
    // accessors self-resolve; a nullptr means blmjciaapa.so/the
    // AapProc singleton isn't available yet, so we just skip.
    void *aap = Singleton_AapProc_GetInstance();
    if (!aap) {
        LOGV("send_event: AapProc instance not available, skipping");
        return;
    }

    if (!VideoManager_IsAAVideoInFocus(AapProc_GetVideoManager(aap))) {
        // Common when AAP is connected but the user is on the native
        // HMI / CarPlay; suppress quietly.
        LOGV("send_event: video not in focus, suppressing action=%u n=%d",
             static_cast<uint32_t>(action), n);
        return;
    }

    AAP_TouchEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.device_type  = 1;                              // touchscreen
    evt.finger_count = static_cast<uint32_t>(n);
    evt.action       = static_cast<uint32_t>(action);
    evt.action_index = static_cast<uint32_t>(action_index);

    for (int i = 0; i < n; ++i) {
        evt.x[i]          = scale_x(entries[i].fg.x);
        evt.y[i]          = scale_y(entries[i].fg.y);
        evt.pointer_id[i] = static_cast<uint32_t>(entries[i].fg.tracking_id);
    }

    // Verbose dump of the outgoing event — one line per finger so
    // the format string stays bounded.
    LOGV("send_event: action=%u n=%d action_index=%d",
         static_cast<uint32_t>(action), n, action_index);
    for (int i = 0; i < n; ++i) {
        LOGV("send_event:   [%d] slot=%d id=%u xy=(%u,%u)",
             i, entries[i].slot, evt.pointer_id[i], evt.x[i], evt.y[i]);
    }

    void *race = AapProc_GetRaceAap(aap);
    if (!race) {
        LOGV("send_event: RaceAap not available, skipping");
        return;
    }
    int rc = RaceAap_SendTouchInput(race, &evt);
    LOGV("send_event: SendTouchInput -> %d", rc);
    if (rc != 0) {
        // 0x108 = AAP_SESSION_NOT_CONNECTED — expected briefly after
        //         create until the SDK reaches AAP_SESSION_STATE_CONNECTED
        //         (state value 0x302). Stay quiet for that one at
        //         error level; verbose log above still records it.
        if (rc != 0x108) {
            LOGE("SendTouchInput action=%u n=%d -> %d",
                      static_cast<uint32_t>(action), n, rc);
        }
    }
}

// ---------------------------------------------------------------
// Per-frame diff + dispatch.
// ---------------------------------------------------------------

// Snapshot persistence across calls. touch_on_frame() is called by
// the reader thread only — no need for atomics on the snapshot itself.
MtState g_prev;
bool    g_prev_init = false;

// Build a "survivors" snapshot: for every slot that exists in both
// prev and cur with the same tracking id, take the cur finger
// (current position). Output is sorted by slot index. Returns count.
int build_survivors(const MtState &prev, const MtState &cur,
                    SnapEntry *out)
{
    int n = 0;
    for (int slot = 0; slot < kMaxFingers; ++slot) {
        int pid = prev.fingers[slot].tracking_id;
        int cid = cur.fingers[slot].tracking_id;
        if (pid >= 0 && cid == pid) {
            out[n].slot = slot;
            out[n].fg   = cur.fingers[slot];
            ++n;
        }
    }
    return n;
}

// Insert a finger into a slot-sorted snapshot. Returns the array
// index where the new entry was placed.
int insert_sorted(SnapEntry *arr, int n, int slot, const Finger &fg)
{
    int pos = n;
    for (int i = 0; i < n; ++i) {
        if (arr[i].slot > slot) { pos = i; break; }
    }
    for (int i = n; i > pos; --i) arr[i] = arr[i - 1];
    arr[pos].slot = slot;
    arr[pos].fg   = fg;
    return pos;
}

void reset_prev_state()
{
    for (int s = 0; s < kMaxFingers; ++s) {
        g_prev.fingers[s].tracking_id = -1;
        g_prev.fingers[s].x = 0;
        g_prev.fingers[s].y = 0;
    }
    g_prev.n_active = 0;
}

} // namespace

void touch_send_reset(void)
{
    // Force the phantom-finger fix-up to run again on the next frame so
    // stale finger state from a previous session can't corrupt the
    // first diff of a new one.
    g_prev_init = false;
}

void touch_on_frame(const MtState &cur)
{
    // If AA is not the foreground video target, we deliberately drop all touch
    // events and discard the local diff baseline. When focus returns, the next
    // frame starts a fresh touch sequence instead of replaying stale local state.
    void *aap = Singleton_AapProc_GetInstance();
    if (!aap || !VideoManager_IsAAVideoInFocus(AapProc_GetVideoManager(aap))) {
        reset_prev_state();
        g_prev_init = true;
        return;
    }

    if (!g_prev_init) {
        // One-shot init for g_prev. The "slot empty" sentinel is
        // tracking_id = -1, but `MtState g_prev;` at file scope is
        // zero-initialised (tracking_id = 0 in every slot), which
        // collides with valid kernel-assigned IDs. Without this
        // fix-up the very first diff would see "old phantom finger
        // id=0 vs new finger id=N" in every used slot and emit
        // spurious UP/POINTER_UP events for fingers that never
        // existed — corrupting single-touch's first DOWN and (worse)
        // breaking the first multi-touch sequence on each fresh
        // slot. AA would silently drop the phantom UPs (no prior
        // DOWN to match) but the wire stream is malformed and
        // gesture detectors can react unpredictably.
        reset_prev_state();
        g_prev_init = true;
        // Fall through into the normal diff path.
    }

    // Categorise per-slot transitions.
    int falling_slots[kMaxFingers]; int n_falling = 0;
    int rising_slots [kMaxFingers]; int n_rising  = 0;
    bool any_moved = false;

    for (int s = 0; s < kMaxFingers; ++s) {
        int pid = g_prev.fingers[s].tracking_id;
        int cid = cur     .fingers[s].tracking_id;

        if (pid < 0 && cid < 0) {
            continue;                       // empty -> empty
        } else if (pid < 0 && cid >= 0) {
            rising_slots[n_rising++] = s;   // new finger
        } else if (pid >= 0 && cid < 0) {
            falling_slots[n_falling++] = s; // finger released
        } else if (pid != cid) {
            // Different finger in same slot — treat as release-then-rise.
            falling_slots[n_falling++] = s;
            rising_slots [n_rising++]  = s;
        } else {
            // Same finger; possibly moved.
            if (g_prev.fingers[s].x != cur.fingers[s].x ||
                g_prev.fingers[s].y != cur.fingers[s].y) {
                any_moved = true;
            }
        }
    }

    // Survivors (slots in both prev and cur with same id) — snapshot
    // base used for all generated events.
    SnapEntry survivors[kMaxFingers];
    int n_survivors = build_survivors(g_prev, cur, survivors);

    LOGV("on_frame: falling=%d rising=%d moved=%d survivors=%d",
         n_falling, n_rising, any_moved ? 1 : 0, n_survivors);

    // ----- Step 1: POINTER_UP / UP for each falling slot ------------
    //
    // Build a snapshot with ALL fingers that were alive in prev: survivors
    // (continuing) plus all falling fingers (at their prev positions).
    // Then iteratively peel off one falling finger per event. When the
    // snapshot has >1 finger the action is POINTER_UP; the very last
    // removal is UP.
    SnapEntry up_snap[kMaxFingers];
    memcpy(up_snap, survivors, sizeof(SnapEntry) * n_survivors);
    int up_n = n_survivors;
    for (int i = 0; i < n_falling; ++i) {
        insert_sorted(up_snap, up_n, falling_slots[i],
                      g_prev.fingers[falling_slots[i]]);
        ++up_n;
    }

    for (int i = 0; i < n_falling; ++i) {
        int slot = falling_slots[i];

        // Find this finger in the snapshot.
        int idx = -1;
        for (int j = 0; j < up_n; ++j) {
            if (up_snap[j].slot == slot) { idx = j; break; }
        }
        if (idx < 0) continue;  // should not happen

        AAPTouchAction act = (up_n == 1) ? kAAPTouchActionUp
                                         : kAAPTouchActionPointerUp;
        int action_index = (up_n == 1) ? 0 : idx;
        send_event(up_snap, up_n, act, action_index);

        // Remove this finger from the snapshot for the next iteration.
        for (int j = idx; j < up_n - 1; ++j) up_snap[j] = up_snap[j + 1];
        --up_n;
    }

    // ----- Step 2: POINTER_DOWN / DOWN for each rising slot ---------
    //
    // Incremental snapshot: start from survivors, add risen fingers
    // one at a time, emitting an event each time.
    SnapEntry inc[kMaxFingers];
    memcpy(inc, survivors, sizeof(SnapEntry) * n_survivors);
    int inc_n = n_survivors;

    for (int i = 0; i < n_rising; ++i) {
        int slot = rising_slots[i];
        int idx  = insert_sorted(inc, inc_n, slot, cur.fingers[slot]);
        ++inc_n;

        AAPTouchAction act = (inc_n == 1) ? kAAPTouchActionDown
                                          : kAAPTouchActionPointerDown;
        int action_index = (inc_n == 1) ? 0 : idx;
        send_event(inc, inc_n, act, action_index);
    }

    // ----- Step 3: MOVE if nothing else changed but positions did ---
    if (n_falling == 0 && n_rising == 0 && any_moved) {
        // All current fingers, in slot order.
        SnapEntry move_snap[kMaxFingers];
        int n = 0;
        for (int s = 0; s < kMaxFingers; ++s) {
            if (cur.fingers[s].tracking_id < 0) continue;
            move_snap[n].slot = s;
            move_snap[n].fg   = cur.fingers[s];
            ++n;
        }
        send_event(move_snap, n, kAAPTouchActionMove, 0);
    }

    g_prev = cur;
}
