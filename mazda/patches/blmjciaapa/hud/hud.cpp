// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Adapted from headunit (https://github.com/Trevelopment/headunit),
// licensed under GNU AGPL v3.
// See NOTICE.md at the repo root for the full attribution.

#define LOG_TAG "HUD"
#include "../log.h"
#include "hud.h"
#include "common/config.h"   // libpatch_config::hud_transport()

// HUD transport selection. Two transports send our guidance to the
// HUD by different routes:
//   * vbs     (vbs_tx.{h,cpp})       — writes the HUD frame directly to
//                                      com.jci.vbs.navi (libjcimod_navigation
//                                      inside the jciVBS process). Works with
//                                      no navigation SD card.
//   * svcnavi (svcnavi_tx.{h,cpp})   — emits GuidanceChangedForHUD to
//                                      svcjcinavi, which forwards as the single
//                                      HUD-frame writer. Needs the nav SD card.
// Both backends are compiled in; the active one is chosen at RUNTIME
// from libpatch.conf (hud_transport=svcnavi|vbs), defaulting to svcnavi.
// See config.{h,cpp}.
#include "svcnavi_tx.h"
#include "vbs_tx.h"
#include "translit.h"   // hud_translit::fold() — precomposed-Latin street-name fold
#include "hud_nav16.h"  // 1.6 nav decoder + AaLane + aa_to_mazda_unit/parse_dist_x10
#include "common/aa_nav16_msg.h"   // AA_NAV16_MSG_* — the sender/receiver msgId contract

#include <stdint.h>
#include <string.h>

namespace {

// Transport forwarders — bind once to the configured backend, then stay
// transport-agnostic at the call sites. Both backends expose the same
// five free functions with identical signatures, so an ops table of
// plain function pointers selects between them with no per-call branch.
struct HudTransportOps {
    void (*start)();
    void (*stop)();
    void (*status)(uint32_t status);
    void (*next_turn)(const char *road, uint32_t side, uint32_t event,
                      int32_t angle, int32_t number);
    void (*distance)(int32_t dist_m, int32_t time_s,
                     int32_t disp_dist, uint32_t disp_unit);
    void (*guidance_v16)(uint32_t glyph, const char *road, int32_t dist_dec,
                         uint8_t unit, const AaLane *lanes, uint8_t n);
};

// Forward to the vbs transport. Lanes are carried through (the vbs transport
// holds them); see vbs_tx.cpp for the emit-side OEM-ABI note on the direct frame.
void vbs_v16_adapter(uint32_t glyph, const char *road, int32_t dist_dec,
                     uint8_t unit, const AaLane *lanes, uint8_t n)
{
    vbs_tx_v16(glyph, road, dist_dec, unit, lanes, n);
}

const HudTransportOps kSvcnaviOps = {
    &svcnavi_tx_start, &svcnavi_tx_stop, &svcnavi_tx_status,
    &svcnavi_tx_next_turn, &svcnavi_tx_distance, &svcnavi_tx_v16,
};
const HudTransportOps kVbsOps = {
    &vbs_tx_start, &vbs_tx_stop, &vbs_tx_status,
    &vbs_tx_next_turn, &vbs_tx_distance, &vbs_v16_adapter,
};

// The active backend. Bound once in hud_tx_start() (the first transport
// call of every session) from the config value, then used directly by
// the per-event forwarders that only ever fire after start. Defaults to
// svcnavi so a stray call before binding still has a valid target.
const HudTransportOps *g_tx = &kSvcnaviOps;

inline void hud_tx_start()
{
    g_tx = (libpatch_config::hud_transport() == libpatch_config::HUD_TRANSPORT_VBS)
               ? &kVbsOps : &kSvcnaviOps;
    g_tx->start();
}
inline void hud_tx_stop()   { g_tx->stop(); }
// NOTE: the single-writer invariant is enforced at the producer ENTRY points,
// not on these shared forwarders — our_nav_cb (1.5) returns early once 1.6
// frames are actually arriving (hud_nav16_rx_seen), and the 1.6 path's only
// writer is the rx thread (which only runs under v1.6).
// hud_tx_status is intentionally NOT guarded: the 1.6 CLEAR path calls it.
inline void hud_tx_status(uint32_t status) { g_tx->status(status); }
inline void hud_tx_next_turn(const char *road, uint32_t side, uint32_t event,
                             int32_t angle, int32_t number)
{
    // Fold HUD-unrenderable precomposed Latin letters (Latin Extended
    // Additional, U+1E00..U+1EFF) down to their base forms so accented
    // street names show legibly instead of gapping (gated by
    // hud_fold_latin, default off). Copy the SDK-owned (const) road name
    // into a local buffer first, then fold in place — the fold only ever
    // shrinks, so 256 bounds it (the transports truncate to their own
    // buffer anyway).
    if (road != nullptr && libpatch_config::hud_fold_latin()) {
        char buf[256];
        strncpy(buf, road, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        hud_translit::fold(buf);
        g_tx->next_turn(buf, side, event, angle, number);
        return;
    }
    g_tx->next_turn(road, side, event, angle, number);
}
inline void hud_tx_distance(int32_t dist_m, int32_t time_s,
                            int32_t disp_dist, uint32_t disp_unit)
{ g_tx->distance(dist_m, time_s, disp_dist, disp_unit); }

// GAL 1.6 guidance forwarder. Unlike hud_tx_next_turn, no fold here: the road
// arrives ALREADY folded — hud_feed_nav16_raw folds once at 0x8006 ingest, so
// the per-tick distance emits (every 0x8007 on final approach) do zero string
// work on the rx thread.
inline void hud_tx_guidance_v16(uint32_t glyph, const char *road, int32_t dist_dec,
                                uint8_t unit, const AaLane *lanes, uint8_t n)
{
    // Single-writer guard: 1.5 cb thread owns g_snapshot in v1.5 mode. The rx
    // thread only runs under v1.6, so this should never fire — log it if it
    // somehow does (config changed under us) rather than dropping silently.
    if (!libpatch_config::use_protocol_v1_6()) {
        LOGV("hud_tx_guidance_v16: dropped — use_protocol_v1_6 not set");
        return;
    }
    g_tx->guidance_v16(glyph, road, dist_dec, unit, lanes, n);
}

// cb_list shape: 19 word slots, total 76 bytes. The SDK memcpy's
// all 76 bytes into the session handle at handle+0x20 inside
// aap_create_session.
constexpr int kCbListWordSlots = 19;       // 0x4c bytes
constexpr int kCbListNavSlot   = 10;       // byte offset 0x28 — E_AAP_EVENT_NAV_DATA_CB
constexpr int kCbListUserSlot  = 18;       // byte offset 0x48 — passed as first arg

// Tag values that prefix every 36-byte event header. Pinned by the
// producer in aap_service.c (navigationStatusCallback emits 0x500
// from a NAVMessagesStatus proto; navigationNextTurnCallback emits
// 0x501 from a NAVTurnMessage; navigationNextTurnDistanceCallback
// emits 0x502 from a NAVDistanceMessage).
constexpr uint32_t kTagStatus    = 0x500;   // NAVMessagesStatus
constexpr uint32_t kTagNextTurn  = 0x501;   // NAVTurnMessage
constexpr uint32_t kTagDistance  = 0x502;   // NAVDistanceMessage

// NAVMessagesStatus.STATUS — proto enum, hu.proto:
//   START = 1; STOP = 2;
// Any other value seen on the wire is an unspecified / unknown
// state; the producer in aap_service is the only thing in front of
// us that could synthesise one, and it forwards verbatim.
enum NavStatusEnum : uint32_t {
    NAV_STATUS_START = 1,
    NAV_STATUS_STOP  = 2,
};

// NAVTurnMessage.TURN_SIDE — proto enum, hu.proto:
//   TURN_LEFT = 1; TURN_RIGHT = 2; TURN_UNSPECIFIED = 3;
enum NavTurnSideEnum : uint32_t {
    NAV_TURN_LEFT        = 1,
    NAV_TURN_RIGHT       = 2,
    NAV_TURN_UNSPECIFIED = 3,
};

// NAVTurnMessage.TURN_EVENT — proto enum, hu.proto (sparse — 15
// and 18 are intentionally unassigned in the .proto, so 0x11=17
// and 0x13=19 are the only valid values in that gap):
//   0  TURN_UNKNOWN              10 TURN_MERGE
//   1  TURN_DEPART               11 TURN_ROUNDABOUT_ENTER
//   2  TURN_NAME_CHANGE          12 TURN_ROUNDABOUT_EXIT
//   3  TURN_SLIGHT_TURN          13 TURN_ROUNDABOUT_ENTER_AND_EXIT
//   4  TURN_TURN                 14 TURN_STRAIGHT
//   5  TURN_SHARP_TURN           16 TURN_FERRY_BOAT
//   6  TURN_U_TURN               17 TURN_FERRY_TRAIN
//   7  TURN_ON_RAMP              19 TURN_DESTINATION
//   8  TURN_OFF_RAMP
//   9  TURN_FORK
enum NavTurnEventEnum : uint32_t {
    NAV_TURN_EVENT_UNKNOWN                  =  0,
    NAV_TURN_EVENT_DEPART                   =  1,
    NAV_TURN_EVENT_NAME_CHANGE              =  2,
    NAV_TURN_EVENT_SLIGHT_TURN              =  3,
    NAV_TURN_EVENT_TURN                     =  4,
    NAV_TURN_EVENT_SHARP_TURN               =  5,
    NAV_TURN_EVENT_U_TURN                   =  6,
    NAV_TURN_EVENT_ON_RAMP                  =  7,
    NAV_TURN_EVENT_OFF_RAMP                 =  8,
    NAV_TURN_EVENT_FORK                     =  9,
    NAV_TURN_EVENT_MERGE                    = 10,
    NAV_TURN_EVENT_ROUNDABOUT_ENTER         = 11,
    NAV_TURN_EVENT_ROUNDABOUT_EXIT          = 12,
    NAV_TURN_EVENT_ROUNDABOUT_ENTER_AND_EXIT= 13,
    NAV_TURN_EVENT_STRAIGHT                 = 14,
    NAV_TURN_EVENT_FERRY_BOAT               = 16,
    NAV_TURN_EVENT_FERRY_TRAIN              = 17,
    NAV_TURN_EVENT_DESTINATION              = 19,
};

// The value at +0x10 is NOT the raw proto turn_event — the producer
// re-bases it first. Before forwarding, the producer compacts the
// sparse proto enum (which skips 15 and 18) into a dense 0..17 enum,
// i.e. it deletes the two gaps. Its mapping is, equivalently:
//
//     proto 0..14            -> unchanged   (0..14)
//     proto 16 FERRY_BOAT    -> 15
//     proto 17 FERRY_TRAIN   -> 16
//     proto 19 DESTINATION   -> 17
//     anything else          -> garbage (producer logs a warning and
//                               forwards an uninitialised local)
//
// So the byte we receive for the destination maneuver is 17, which —
// if read as a proto value — collides with FERRY_TRAIN and resolves
// to a blank glyph. Undo the compaction here so the rest of the HUD
// path (NavTurnEventEnum, nav_turn_event_name, kTurnIcons) can keep
// indexing by the genuine proto values it was built and validated
// against. Only the top three events move; 0..14 are identity.
uint32_t decode_turn_event(uint32_t compacted)
{
    switch (compacted) {
    case 15: return NAV_TURN_EVENT_FERRY_BOAT;   // 15 -> 16
    case 16: return NAV_TURN_EVENT_FERRY_TRAIN;  // 16 -> 17
    case 17: return NAV_TURN_EVENT_DESTINATION;  // 17 -> 19
    default: return compacted;                   // 0..14 pass through;
                                                 // 18/19/… never appear
                                                 // (producer can't emit
                                                 // them) but stay benign.
    }
}

// NAVDistanceMessage.DISPLAY_DISTANCE_UNIT — proto enum, hu.proto:
//   METERS       = 1  // meters    * 1000 (display < 1000 m)
//   KILOMETERS10 = 2  // km        * 1000 (display > 10 km)
//   KILOMETERS   = 3  // km        * 1000 (display 1..10 km)
//   MILES10      = 4  // miles     * 1000 (display > 10 mi)
//   MILES        = 5  // miles     * 1000 (display 1..10 mi)
//   FEET         = 6  // feet      * 1000
enum NavDistanceUnitEnum : uint32_t {
    NAV_DISTUNIT_METERS       = 1,
    NAV_DISTUNIT_KILOMETERS10 = 2,
    NAV_DISTUNIT_KILOMETERS   = 3,
    NAV_DISTUNIT_MILES10      = 4,
    NAV_DISTUNIT_MILES        = 5,
    NAV_DISTUNIT_FEET         = 6,
};

// 0x501 header field offsets — sized for IMAGE_CODES_ONLY cluster
// mode but the layout itself is identical in CUSTOM_IMAGES_SUPPORTED
// mode (icon pointer/length are just NULL/0 when the receiver-lib
// advertises IMAGE_CODES_ONLY). All offsets are byte offsets into
// the 36-byte buffer the SDK hands us.
//
// Layout (verified against §5.2 of the impl doc and the dispatcher
// prep at libaap_interface.so:0x14f48):
//   +0x00  uint32   tag                  (= 0x501)
//   +0x04  char *   road_name            (heap, freed by SDK on return;
//                                         proto calls this `event_name`)
//   +0x08  uint32   road_name_len
//   +0x0C  uint32   turn_side            (NAVTurnMessage.TURN_SIDE)
//   +0x10  uint32   turn_event           (producer-COMPACTED TURN_EVENT,
//                                         dense 0..17 — NOT the raw proto
//                                         value; decode_turn_event() maps it
//                                         back to the sparse proto enum)
//   +0x14  void *   image                (PNG bytes when IMAGE mode;
//                                         NULL in IMAGE_CODES_ONLY mode)
//   +0x18  uint32   image_len            (0 in IMAGE_CODES_ONLY mode)
//   +0x1C  int32    turn_angle           (degrees, signed)
//   +0x20  int32    turn_number          (maneuver/exit number)
struct NextTurnHdr {
    uint32_t    tag;
    const char *road_name;
    uint32_t    road_name_len;
    uint32_t    turn_side;
    uint32_t    turn_event;   // producer-compacted; run through decode_turn_event()
    const void *image;
    uint32_t    image_len;
    int32_t     turn_angle;
    int32_t     turn_number;
};
static_assert(sizeof(NextTurnHdr) == 36, "NextTurnHdr must match 36-byte SDK buffer");

// 0x500 header — only status is meaningful, the remaining 28 bytes
// are zeroed by the dispatcher's memset(stack_buf, 0, 36) at
// libaap_interface.so:0x14cb0. The wire proto only defines
// START=1 / STOP=2; the producer forwards verbatim, so an
// out-of-range value here means a malformed or version-skewed phone.
struct StatusHdr {
    uint32_t tag;       // = 0x500
    uint32_t status;    // NAVMessagesStatus.STATUS — 1=START, 2=STOP
    uint32_t reserved[7];
};
static_assert(sizeof(StatusHdr) == 36, "StatusHdr must match 36-byte SDK buffer");

// 0x502 header — distance / ETA. Last 16 bytes reserved.
//
// `display_distance` is `uint64` on the proto wire but the SDK's
// 36-byte buffer only allocates 4 bytes for it (the next field —
// `display_distance_unit` — sits immediately after at +0x10), so
// the producer truncates to the low 32 bits. In practice values are
// always small (millimetre-precision distance up to ~10 mi/km, well
// under 2^31), so the truncation is loss-free.
struct DistanceHdr {
    uint32_t tag;                      // = 0x502
    int32_t  distance;                 // proto `distance` — meters to next maneuver
    int32_t  time_until;               // proto `time_until` — seconds to next maneuver
    int32_t  display_distance;         // proto `display_distance` low 32 bits
                                       // (raw unit value * 1000; see enum below)
    uint32_t display_distance_unit;    // NAVDistanceMessage.DISPLAY_DISTANCE_UNIT, 1..6
    uint32_t reserved[4];
};
static_assert(sizeof(DistanceHdr) == 36, "DistanceHdr must match 36-byte SDK buffer");

const char *nav_status_name(uint32_t v)
{
    switch (v) {
    case NAV_STATUS_START: return "START";
    case NAV_STATUS_STOP:  return "STOP";
    default:               return "?";
    }
}

const char *nav_turn_side_name(uint32_t v)
{
    switch (v) {
    case NAV_TURN_LEFT:        return "LEFT";
    case NAV_TURN_RIGHT:       return "RIGHT";
    case NAV_TURN_UNSPECIFIED: return "UNSPECIFIED";
    default:                   return "?";
    }
}

const char *nav_turn_event_name(uint32_t v)
{
    switch (v) {
    case NAV_TURN_EVENT_UNKNOWN:                   return "UNKNOWN";
    case NAV_TURN_EVENT_DEPART:                    return "DEPART";
    case NAV_TURN_EVENT_NAME_CHANGE:               return "NAME_CHANGE";
    case NAV_TURN_EVENT_SLIGHT_TURN:               return "SLIGHT_TURN";
    case NAV_TURN_EVENT_TURN:                      return "TURN";
    case NAV_TURN_EVENT_SHARP_TURN:                return "SHARP_TURN";
    case NAV_TURN_EVENT_U_TURN:                    return "U_TURN";
    case NAV_TURN_EVENT_ON_RAMP:                   return "ON_RAMP";
    case NAV_TURN_EVENT_OFF_RAMP:                  return "OFF_RAMP";
    case NAV_TURN_EVENT_FORK:                      return "FORK";
    case NAV_TURN_EVENT_MERGE:                     return "MERGE";
    case NAV_TURN_EVENT_ROUNDABOUT_ENTER:          return "ROUNDABOUT_ENTER";
    case NAV_TURN_EVENT_ROUNDABOUT_EXIT:           return "ROUNDABOUT_EXIT";
    case NAV_TURN_EVENT_ROUNDABOUT_ENTER_AND_EXIT: return "ROUNDABOUT_ENTER_AND_EXIT";
    case NAV_TURN_EVENT_STRAIGHT:                  return "STRAIGHT";
    case NAV_TURN_EVENT_FERRY_BOAT:                return "FERRY_BOAT";
    case NAV_TURN_EVENT_FERRY_TRAIN:               return "FERRY_TRAIN";
    case NAV_TURN_EVENT_DESTINATION:               return "DESTINATION";
    default:                                       return "?";
    }
}

const char *nav_distance_unit_name(uint32_t v)
{
    switch (v) {
    case NAV_DISTUNIT_METERS:       return "METERS";
    case NAV_DISTUNIT_KILOMETERS10: return "KILOMETERS10";
    case NAV_DISTUNIT_KILOMETERS:   return "KILOMETERS";
    case NAV_DISTUNIT_MILES10:      return "MILES10";
    case NAV_DISTUNIT_MILES:        return "MILES";
    case NAV_DISTUNIT_FEET:         return "FEET";
    default:                        return "?";
    }
}

// Forward declaration so substitute_nav_cb() below can take its
// address before the body is seen.
void our_nav_cb(void *user_ctx, void *hdr36);

void dump_status(const StatusHdr *h)
{
    LOGD("nav 0x500 NAVMessagesStatus: status=%u(%s)",
         static_cast<unsigned>(h->status),
         nav_status_name(h->status));
}

void dump_next_turn(const NextTurnHdr *h, uint32_t turn_event)
{
    // road_name is a heap pointer the SDK frees the moment we
    // return; for the log we copy at most a sane cap into a stack
    // buffer so the format-string read is bounded even if the SDK
    // ever forgets to NUL-terminate. road_name_len is the
    // authoritative length (excludes the trailing NUL).
    //
    // (The proto field name is `event_name` but observed payloads
    // and the SDK's internal naming both treat this as the road
    // name string. Keeping `road_name` everywhere for clarity.)
    constexpr size_t kRoadCap = 255;
    char road[kRoadCap + 1];
    road[0] = '\0';
    if (h->road_name && h->road_name_len) {
        size_t n = h->road_name_len;
        if (n > kRoadCap) n = kRoadCap;
        memcpy(road, h->road_name, n);
        road[n] = '\0';
    }

    // h->turn_event is the producer's compacted value; `turn_event`
    // (passed in) has already been mapped back to the proto enum, so
    // the name lookup matches the proto table. Log both so the field
    // semantics stay traceable: raw= what arrived, = proto value.
    LOGD("nav 0x501 NAVTurnMessage: road=\"%s\" road_len=%u "
         "turn_side=%u(%s) turn_event=%u(%s) raw_event=%u "
         "turn_angle=%d turn_number=%d image=%p image_len=%u",
         road,
         static_cast<unsigned>(h->road_name_len),
         static_cast<unsigned>(h->turn_side),
         nav_turn_side_name(h->turn_side),
         static_cast<unsigned>(turn_event),
         nav_turn_event_name(turn_event),
         static_cast<unsigned>(h->turn_event),
         static_cast<int>(h->turn_angle),
         static_cast<int>(h->turn_number),
         h->image,
         static_cast<unsigned>(h->image_len));

    // Sanity check: in IMAGE_CODES_ONLY cluster mode we expect
    // image_len==0 and image==NULL. Surface a warning if the XML
    // wasn't flipped to ENUM (or if the phone ignores the advertised
    // cluster_type and sends a PNG anyway).
    if (h->image_len != 0 || h->image != nullptr) {
        LOGW("nav 0x501: unexpected image payload in IMAGE_CODES_ONLY "
             "mode (image=%p image_len=%u) — check "
             "<cluster_type>ENUM</> in aap_system_attributes*.xml "
             "(image bytes are PNG-encoded per hu.proto)",
             h->image, static_cast<unsigned>(h->image_len));
    }
}

void dump_distance(const DistanceHdr *h)
{
    LOGD("nav 0x502 NAVDistanceMessage: distance=%dm time_until=%ds "
         "display_distance=%d display_distance_unit=%u(%s)",
         static_cast<int>(h->distance),
         static_cast<int>(h->time_until),
         static_cast<int>(h->display_distance),
         static_cast<unsigned>(h->display_distance_unit),
         nav_distance_unit_name(h->display_distance_unit));
}

void our_nav_cb(void *user_ctx, void *hdr36)
{
    (void)user_ctx;  // We don't set cb_list[18], so this is NULL.

    if (!hdr36) {
        LOGW("nav cb: NULL header — SDK contract violation, ignoring");
        return;
    }

    // Single-writer guard + 1.5 fallback: drop the legacy 0x500/0x501/0x502
    // callbacks only once 1.6 frames are actually arriving on the rx socket —
    // NOT on the config flag. aap_service (which advertises 1.6) and this
    // process share only the config key, not the negotiated session, so with
    // the flag set but the 1.6 chain broken (shim not preloaded, byte-verify
    // aborted, phone declined) the phone keeps speaking 1.5 and these
    // callbacks are the only guidance there is — dropping them would leave
    // the HUD dark. A session speaks one protocol, so the two snapshot
    // writers still never race (the seqlock is SPSC); the latch just picks
    // the producer per session.
    if (hud_nav16_rx_seen()) return;

    const uint32_t tag = *static_cast<const uint32_t *>(hdr36);
    switch (tag) {
    case kTagStatus: {
        const StatusHdr *s = static_cast<const StatusHdr *>(hdr36);
        dump_status(s);
        hud_tx_status(s->status);
        break;
    }
    case kTagNextTurn: {
        const NextTurnHdr *t = static_cast<const NextTurnHdr *>(hdr36);
        const uint32_t turn_event = decode_turn_event(t->turn_event);
        dump_next_turn(t, turn_event);
        hud_tx_next_turn(t->road_name, t->turn_side, turn_event,
                         t->turn_angle, t->turn_number);
        break;
    }
    case kTagDistance: {
        const DistanceHdr *d = static_cast<const DistanceHdr *>(hdr36);
        dump_distance(d);
        hud_tx_distance(d->distance, d->time_until,
                        d->display_distance, d->display_distance_unit);
        break;
    }
    default:
        LOGW("nav cb: unknown tag 0x%x — first 16 bytes: "
             "%02x %02x %02x %02x  %02x %02x %02x %02x  "
             "%02x %02x %02x %02x  %02x %02x %02x %02x",
             tag,
             static_cast<const uint8_t *>(hdr36)[0],
             static_cast<const uint8_t *>(hdr36)[1],
             static_cast<const uint8_t *>(hdr36)[2],
             static_cast<const uint8_t *>(hdr36)[3],
             static_cast<const uint8_t *>(hdr36)[4],
             static_cast<const uint8_t *>(hdr36)[5],
             static_cast<const uint8_t *>(hdr36)[6],
             static_cast<const uint8_t *>(hdr36)[7],
             static_cast<const uint8_t *>(hdr36)[8],
             static_cast<const uint8_t *>(hdr36)[9],
             static_cast<const uint8_t *>(hdr36)[10],
             static_cast<const uint8_t *>(hdr36)[11],
             static_cast<const uint8_t *>(hdr36)[12],
             static_cast<const uint8_t *>(hdr36)[13],
             static_cast<const uint8_t *>(hdr36)[14],
             static_cast<const uint8_t *>(hdr36)[15]);
        break;
    }
}

} // namespace

// Called from the aap_create_session PLT shim BEFORE chaining
// through. Modifying cb_list[10] in place is safe because the SDK
// copies the full 76-byte table into the session handle inside
// aap_create_session; once that copy is done, the caller's stack
// buffer is dead memory.
//
// We leave cb_list[18] (the user-context slot) untouched — the OEM
// never sets it, and we have no per-session state to thread through.
void hud_pre_aap_create_session(void *cb_list)
{
    if (!cb_list) {
        LOGW("hud_pre_aap_create_session: NULL cb_list, skipping "
             "(real aap_create_session will reject it anyway)");
        return;
    }

    void **slots = static_cast<void **>(cb_list);
    void  *prev  = slots[kCbListNavSlot];
    slots[kCbListNavSlot] = reinterpret_cast<void *>(&our_nav_cb);

    LOGD("hud_pre_aap_create_session: cb_list=%p slot[%d] %p -> %p (our_nav_cb)",
         cb_list, kCbListNavSlot, prev,
         reinterpret_cast<void *>(&our_nav_cb));

    // Compile-time-ish guard against a future firmware that bumps
    // the table size. If kCbListUserSlot ever drifts past the end
    // of what we model, the touch path and the rest of the lib
    // also need rewiring — re-harvest the cb_list layout from
    // libaap_interface.so / blmjciaapa.so before adjusting.
    static_assert(kCbListUserSlot < kCbListWordSlots,
                  "user-context slot out of cb_list bounds");
}

// NavigationStatus (0x8003) enum (aasdk): UNAVAILABLE=0, ACTIVE=1, INACTIVE=2,
// REROUTING=3. INACTIVE/UNAVAILABLE -> blank the HUD; ACTIVE/REROUTING -> keep.
enum { NAV_UNAVAILABLE = 0, NAV_ACTIVE = 1, NAV_INACTIVE = 2, NAV_REROUTING = 3 };

// Quantize the display distance (value*10) to its natural on-HUD granularity, so
// a frame is only re-sent when the SHOWN figure would change — not on every
// position tick. mi/km are already 0.1-granular; m/yd/ft step by magnitude.
int32_t quantize_dist_x10(int32_t v, uint8_t mazda_unit)
{
    if (v < 0) return 0;
    if (mazda_unit == 2 || mazda_unit == 3) return v;        // mi/km: 0.1 already
    if (v >= 5000) return (v + 250) / 500 * 500;             // far  -> 50-unit steps
    if (v >= 1000) return (v +  50) / 100 * 100;             // mid  -> 10-unit steps
    if (v >=  200) return (v +  25) /  50 *  50;             // near ->  5-unit steps
    return v;                                                // final approach -> as-is
}

// Accumulated Mazda-domain guidance: maneuver/road/lanes come from 0x8006,
// distance from 0x8007, so the two streams are merged here before display. Laid
// out gap-free (and zero-initialised as a static) so the memcmp change-gate is
// exact — KEEP IT GAP-FREE if you add a field.
struct AaNav16HudState {
    uint32_t glyph;                       // Mazda HUD maneuver glyph (MazdaIcon; 0=blank)
    int32_t  dist_dec;                    // display distance * 10
    uint8_t  dist_unit;                   // Mazda HUD unit 0..5
    uint8_t  n_lanes;                     // 0..HUD_NAV16_MAX_LANES
    uint8_t  pad0, pad1;                  // keep gap-free for the memcmp change-gate
    char     road[64];                    // street name (UTF-8, folded at 0x8006 ingest)
    AaLane   lanes[HUD_NAV16_MAX_LANES];
};
static_assert(sizeof(AaNav16HudState) == 108,
              "AaNav16HudState must stay gap-free (memcmp change-gate)");

static AaNav16HudState g_nav16_acc;   // merged guidance (zero-init: gap-free memcmp)
static AaNav16HudState g_nav16_last;  // last frame fed to the transport (change-gate)
static bool g_nav16_have_last = false;

// Reset the accumulator + change-gate. Owned by the rx lifecycle: called from
// hud_nav16_rx_start() BEFORE the receiver thread exists (the only other
// toucher of this state), so it needs no locking. Without this, a session that
// ends abnormally (cable pull — the phone never sends INACTIVE/STOP) leaves a
// stale change-gate that can suppress the next session's first frame against
// an already-blanked HUD.
void hud_feed_nav16_reset(void)
{
    memset(&g_nav16_acc, 0, sizeof(g_nav16_acc));
    g_nav16_have_last = false;
    LOGV("nav: accumulator + change-gate reset");
}

// Render a RAW GAL 1.6 nav frame relayed from the aap_service shim (runs on the
// nav16 receiver thread). This is the SINGLE place that decodes the AA protocol
// and maps it to the Mazda HUD domain: 0x8006 -> maneuver/road/lanes, 0x8007 ->
// distance, 0x8003 -> lifecycle, 0x8002 -> blank. Decoded guidance goes through
// the shared transport (glyph + fold + lanes), reusing the existing renderer.
void hud_feed_nav16_raw(const uint8_t *frame, int len)
{
    if (!frame || len < 2) return;

    AaNav16HudState &acc  = g_nav16_acc;
    AaNav16HudState &last = g_nav16_last;
    bool &have_last       = g_nav16_have_last;

    // Change-gate: only feed the transport when the displayed frame changed. The
    // svcnavi transport has no consumer-side diff, so this is what prevents a
    // duplicate/continuous-tick flood on the HUD D-Bus.
    auto emit_if_changed = [&]() {
        if (have_last && memcmp(&acc, &last, sizeof(acc)) == 0) return;
        hud_tx_guidance_v16(acc.glyph, acc.road, acc.dist_dec, acc.dist_unit,
                            acc.lanes, acc.n_lanes);
        last = acc;
        have_last = true;
    };

    const uint32_t id = ((uint32_t)frame[0] << 8) | frame[1];

    if (id == AA_NAV16_MSG_NAV_STATE) {  // maneuver + road + lanes
        AaGuidance g;
        hud_nav16_on_frame(frame, len, &g, nullptr);
#if LOG_LEVEL <= LOG_LEVEL_VERBOSE
        char line[320]; hud_nav16_format_guidance(&g, line, sizeof(line)); LOGV("%s", line);
#endif
        uint32_t glyph = hud_nav16_glyph(&g);
        if (glyph > 60) glyph = 0;                         // clamp untrusted glyph
        // Fold once here, at road ingest, not in the per-emit forwarder —
        // distance ticks re-emit the road far more often than 0x8006 changes
        // it. Folding g.road (our stack copy) BEFORE the strncpy matters: the
        // fold shrinks in place without clearing its residue, and the gap-free
        // memcmp change-gate needs acc.road's tail zero-padded — which strncpy
        // does when the source is already its final length.
        if (libpatch_config::hud_fold_latin()) {
            hud_translit::fold(g.road);
        }
        // A different maneuver means the held distance belongs to the PREVIOUS
        // step (distance only ever arrives on 0x8007) — invalidate it so the
        // new arrow is never shown against the old step's ticking-down figure.
        // 0 + unit 0 ("none") is the same not-yet-any-distance state every
        // session starts in; the real figure lands with the next 0x8007 (~1 s).
        if (glyph != acc.glyph || strcmp(g.road, acc.road) != 0) {
            acc.dist_dec  = 0;
            acc.dist_unit = 0;
        }
        acc.glyph = glyph;
        strncpy(acc.road, g.road, sizeof(acc.road) - 1);
        acc.road[sizeof(acc.road) - 1] = '\0';
        int nl = g.n_lanes;
        if (nl < 0) nl = 0;
        if (nl > HUD_NAV16_MAX_LANES) nl = HUD_NAV16_MAX_LANES;
        acc.n_lanes = (uint8_t)nl;
        for (int i = 0; i < HUD_NAV16_MAX_LANES; ++i) {
            acc.lanes[i].present_mask   = (i < nl) ? g.lanes[i].present_mask   : 0;
            acc.lanes[i].highlight_mask = (i < nl) ? g.lanes[i].highlight_mask : 0;
        }
        emit_if_changed();
    } else if (id == AA_NAV16_MSG_POSITION) {      // distance to next maneuver
        AaPosition p;
        hud_nav16_on_frame(frame, len, nullptr, &p);
#if LOG_LEVEL <= LOG_LEVEL_VERBOSE
        char line[320]; hud_nav16_format_position(&p, line, sizeof(line)); LOGV("%s", line);
#endif
        if (p.have_step) {
            uint8_t unit = aa_to_mazda_unit(p.step_units);
            if (unit > 5) unit = 0;                        // clamp untrusted unit
            acc.dist_unit = unit;
            acc.dist_dec  = quantize_dist_x10(parse_dist_x10(p.step_display), unit);
            emit_if_changed();
        }
    } else if (id == AA_NAV16_MSG_STATUS) {        // lifecycle
        int status = -1;
        hud_nav16_read_status(frame, len, &status);
        if (status == NAV_INACTIVE || status == NAV_UNAVAILABLE) {
            hud_tx_status(2);                              // guidance ended -> blank
            hud_feed_nav16_reset();
            LOGD("nav: NavigationStatus=%d -> HUD cleared", status);
        } else {
            LOGV("nav: NavigationStatus=%d (active/rerouting) -> HUD kept", status);
        }
    } else if (id == AA_NAV16_MSG_CLUSTER_STOP) {
        hud_tx_status(2);
        hud_feed_nav16_reset();
        LOGD("nav: cluster STOP (0x8002) -> HUD cleared");
    }
    // CLUSTER_START / NEXT_TURN / NEXT_TURN_DIST: nothing to render
}

void hud_post_aap_create_session(void)
{
    hud_tx_start();
    // Start the GAL 1.6 receiver only when that protocol is enabled — otherwise
    // the aap_service shim never sends and the socket would idle for nothing.
    if (libpatch_config::use_protocol_v1_6()) {
        hud_nav16_rx_start();
    }
}

void hud_pre_aap_destroy_session(void)
{
    if (libpatch_config::use_protocol_v1_6()) {
        hud_nav16_rx_stop();
    }
    hud_tx_stop();
}
