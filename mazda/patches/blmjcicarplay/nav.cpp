// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CarPlay -> HUD bridge for Mazda Connect (CMU150).
// Copyright (C) 2026 KID MIXER-MODER.
//
// Part of an AGPL-3.0 work - see NOTICE.md for full attribution.
//
// Nav-side glue for the CarPlay->HUD bridge — BUILD-19 (maneuverList[0] imminent select; 0x346 exit angle).
//
// === 2026-06-12 ROOT-CAUSE FIXES ===========================================
// (1) off-by-4 maneuverType: the field is at +0x128 (NOT +0x124 = desc length).
// (2) ROUNDABOUT ANGLE [BUILD-19, disasm-proven on the 74.00.324A ipoddev]: the chosen-exit bearing is
//     junctionElementExitAngle@0x346 (signed deg, 0=straight, neg=left/CCW, pos=right/CW). The OTHER field
//     junctionElementAngle@0x344 is DEAD (nonzero on 0/57 captured roundabouts — Apple never fills it; the
//     pre-BUILD-19 comment had these two BACKWARDS). 0x346==0 -> straight exit -> base roundabout glyph.
//     The old exit_number*90 bearing-synth was REMOVED (wrong convention). [needs-HUD: glyph orientation.]
// (3) maneuver SELECTION [BUILD-19]: read Apple's authoritative active maneuverList[0] (iAP2 TLV 0x000D)
//     from the decoded guidance msg — head burst-index @0x458, count @0x656. The 0x8059 maneuver burst
//     RE-LISTS from idx0=StartRoute every recalc, so the burst index is a WINDOW position, NOT a stable
//     route position; maneuverList[0] is the only authoritative "next maneuver" pointer. We buffer the
//     displayable maneuvers of the current burst (idx0/context stripped, each tagged with its burst idx),
//     then select the buffered entry whose idx == head (or head+1 when head points at a context/heading
//     entry). Head not yet buffered (mid-rebuild/transient) -> KEEP the previous selection (never flash
//     buf[0]). Replaces the BUILD-7..17 content-anchor + dist-jump heuristic (matched the dist-oracle only
//     ~45-49% on recalc-heavy drives). validInfo bits remain FIELD-PRESENCE gates only (0x10=road,
//     0x800=roundabout junction).
//
// REAL 0x8059 (840B) layout (offsets from the producer disasm):
//   +0x1c  u32       validInfo bitmask     (0xE0 group => Apple actively guiding = imminent)
//   +0x20  u32       index << 16            (the maneuver LIST is streamed idx 0..N)
//   +0x24  char[256] maneuverDesc            (pre-rendered instruction text)
//   +0x124 u32       maneuverDesc LENGTH     (NOT the type — old bug)
//   +0x128 u32       maneuverType            (the REAL field — Apple iAP2/CPManeuverType)
//   +0x12c char[256] afterManeuverRoadName   (the road this maneuver leads onto)
//   +0x33c u32       driveSide               (0=RHT, 1=LHT)
//   +0x340 u32       junctionType            (0=intersection, 1=ROUNDABOUT)
//   +0x344 s16       junctionElementAngle    ([BUILD-19] DEAD: nonzero on 0/57 captured roundabouts — do NOT use)
//   +0x346 s16       junctionElementExitAngle ([BUILD-19] REAL chosen-exit bearing; signed deg, 0=straight neg=L pos=R)
// GUIDANCE (0x8058, 1628B): +0x34c u32 distToNextManeuver (METERS, counts down);
//   +0x458 u16 maneuverList[0] = imminent burst-index (iAP2 TLV 0x000D); +0x656 u16 maneuverList count.
//
// maneuverType IS the standard Apple iAP2 / CPManeuverType enum (ground-truth on
// cmu_pull/oncar/nav_applemaps.log: 'Rẽ trái'->1, 'Rẽ phải'->2, StartRoute->11, KeepRight->14;
// real roundabout in the live log: type 7=ExitRoundabout / 29=RoundaboutExit2 / 32=Exit5 with
// junctionType==1):
//   0 NoTurn 1 Left 2 Right 3 Straight 4 UTurn 5 Follow 6 EnterRoundabout
//   7 ExitRoundabout 8 OffRamp 9 OnRamp 10 ArriveEnd 11 StartRoute 12 Arrive
//   13 KeepLeft 14 KeepRight 15/16/17 Ferry 18 StartRouteUTurn 19 UTurnAtRoundabout
//   20/21 L/R-TurnAtEnd 22/23 HwyOffRampL/R 24/25 ArriveL/R 26 UTurnWhenPossible
//   27 ArriveEndOfDirections 28..46 RoundaboutExit1..19 47/48 SharpL/R
//   49/50 SlightL/R 51 ChangeHwy 52/53 ChangeHwyL/R

#include "patch.h"
#include "hud/hud_send.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <atomic>

namespace {
constexpr uint32_t kMtManeuver      = 0x8059;
constexpr uint32_t kMtGuidance      = 0x8058;
constexpr int      kBurstIdxOff     = 0x20;   // u32: index << 16
constexpr int      kValidInfoOff    = 0x1c;   // u32 per-maneuver validInfo bitmask
                                              // (field-presence gates only: 0x10=road,
                                              // 0x800=roundabout junction, 0x400=jAngle valid;
                                              // NO bit marks the imminent maneuver)
constexpr int      kManeuverTextOff = 0x24;
constexpr int      kManeuverTextMax = 256;
constexpr int      kManeuverTypeOff = 0x128;  // u32 maneuverType (was 0x124 = desc len)
constexpr int      kRoadNameOff     = 0x12c;  // NUL-term afterManeuverRoadName
constexpr int      kDriveSideOff    = 0x33c;  // u32 driveSide: 0=RHT, 1=LHT
constexpr int      kJunctionTypeOff = 0x340;  // u32 junctionType: 1=roundabout
constexpr int      kJunctionAngleOff = 0x346; // [BUILD-19] s16 junctionElementExitAngle = REAL chosen-exit
                                              // bearing (signed deg, 0=straight neg=L pos=R). disasm-proven
                                              // (builder 0x39a74: ldrh [r5,#0x36]->0x346). 0x344 is DEAD
                                              // (junctionElementAngle: nonzero on 0/57 captured roundabouts).
constexpr int      kGuidanceDistOff = 0x34c;  // u32 distToNextManeuver, METERS
// [BUILD-19] iAP2 active MANEUVER_LIST (TLV 0x000D) decoded into the 0x8058 guidance msg = Apple's
// authoritative imminent pointer. maneuverList[0] (u16 burst-index of the next maneuver) @0x458, list
// count (u16) @0x656. disasm-proven (guidance builder 0x39fa4: memcpy list -> sp+0x144c -> SysV 0x458).
constexpr int      kManListOff      = 0x458;  // u16 maneuverList[0] = imminent burst index
constexpr int      kManListCntOff   = 0x656;  // u16 maneuverList count

// hu.proto NAVTurnMessage.TURN_EVENT — used as the hud_send kTurnIcons index.
// (kTurnIcons in hud_send.cpp covers 0..19; 13 = roundabout, handled there by
// roundabout_icon(angle, side).)
constexpr uint32_t EV_DEPART = 1, EV_NAME_CHANGE = 2, EV_SLIGHT = 3, EV_TURN = 4,
                   EV_SHARP = 5, EV_UTURN = 6, EV_ON_RAMP = 7, EV_OFF_RAMP = 8,
                   EV_FORK = 9, EV_MERGE = 10, EV_ROUNDABOUT = 13, EV_STRAIGHT = 14,
                   EV_FERRY = 16, EV_DEST = 19;
constexpr uint32_t SIDE_LEFT = 1, SIDE_RIGHT = 2, SIDE_NONE = 3;

// --- latched display state (persists between bursts; the 2Hz hud keep-alive resends it) ---
uint32_t g_disp_event  = EV_STRAIGHT;
uint32_t g_disp_side   = SIDE_NONE;
int32_t  g_disp_angle  = 0;
int32_t  g_disp_number = 0;
char     g_disp_road[256] = {0};
// last emitted (dedup)
uint32_t g_emit_event  = 0xffffffff;
uint32_t g_emit_side   = 0xffffffff;
int32_t  g_emit_angle  = 0x7fffffff;
int32_t  g_emit_number = 0x7fffffff;
char     g_emit_road[256] = {0};
// [2026-06-13 #3] Set by the HUD-clear path (sender thread) so the next maneuver burst resets our
// emit-dedup state on the msgrcv thread — else re-starting the SAME route after a nav-off finds the
// first maneuver == the last-emitted, dedups, and leaves the HUD blank until something changes.
std::atomic<bool> g_nav_reset_req{false};

// maneuverType values that describe the CURRENT segment (heading), not a maneuver to
// display. idx 0 is also always such an entry; this catches them at idx >= 1 too.
//   0 NoTurn  5 FollowRoad  11 StartRoute  18 StartRouteWithUTurn
// [2026-06-14] type 3 (StraightAhead) REMOVED from this filter — it IS a real displayed maneuver
// ("Đi thẳng" / go-straight-onto-road-X), the CarPlay map shows it. Filtering it dropped ~18 real
// straights on one drive: the HUD froze on the PREVIOUS turn while the map showed straight, AND the
// imminent count ran one ahead ("sớm 1 nhịp"). Now buffered -> classify default = EV_STRAIGHT.
bool is_context_type(uint32_t m)
{
    return m == 0 || m == 5 || m == 11 || m == 18;
}

struct Maneuver {
    uint32_t event;
    uint32_t side;     // 1=LEFT, 2=RIGHT, 3=unspecified
    int32_t  angle;    // roundabout exit angle, normalized [0,360); else 0
    int32_t  number;   // roundabout exit number (1..19); else 0
};

// One displayable (non-context) maneuver buffered from the current burst.
struct BufManeuver {
    uint32_t mtype;
    uint32_t idx;         // [BUILD-19] FULL burst index (g_buf is compacted, so array-pos != burst-idx);
                          // the guidance maneuverList head is matched against this.
    uint32_t validInfo;
    Maneuver mv;
    char     road[256];
    char     desc[256];   // [#2] maneuver instruction text, used as the strip text when road is empty
};
constexpr int kMaxBurst = 48;          // routeGuidManeuverCount peaked ~33; 48 = headroom
BufManeuver g_buf[kMaxBurst];
int         g_buf_count = 0;
uint32_t    g_prev_idx  = 0xffffffff;
// [BUILD-19] Imminent selection is DRIVEN BY the guidance message's active maneuverList[0] (Apple's
// authoritative "next maneuver" burst-index, disasm-proven @0x458), NOT a derived heuristic. We only
// remember the last head so a new maneuver burst (recalc) can repaint the same maneuver and a stationary
// re-start paints before the next guidance frame. (Replaces the BUILD-17 content-anchor: g_imm_type/desc/
// valid/pos + find_imm_pos + the dist-jump advance — that matched the dist-oracle only ~45-49% on
// recalc-heavy drives, leaving the HUD on an already-passed maneuver.)
uint32_t    g_last_head       = 0;
bool        g_last_head_valid = false;

// Map the standard CPManeuverType + junction fields to a HUD turn_event/side/angle.
Maneuver classify(uint32_t mtype, uint32_t junctionType,
                  int32_t junctionAngle, uint32_t driveSide)
{
    Maneuver m{EV_STRAIGHT, SIDE_NONE, 0, 0};

    // Roundabout: by maneuverType OR the junctionType flag. The HUD picks a directional
    // glyph from the exit bearing (event 13 -> roundabout_icon(angle, side)).
    bool ra = (junctionType == 1) || mtype == 6 || mtype == 7 || mtype == 19 ||
              (mtype >= 28 && mtype <= 46);
    if (ra) {
        m.event = EV_ROUNDABOUT;
        m.side  = (driveSide == 1) ? SIDE_LEFT : SIDE_RIGHT;   // LHT vs RHT glyph bank
        int32_t a = junctionAngle % 360;
        if (a < 0) a += 360;                                   // hud expects [0,360)
        if (mtype >= 28 && mtype <= 46) m.number = (int32_t)mtype - 27;  // exit N
        // [BUILD-19] junctionAngle now = junctionElementExitAngle@0x346 = the REAL signed exit bearing
        // (0=straight, neg=left/CCW, pos=right/CW), disasm-proven LIVE (0x344 was DEAD: 0/57 roundabouts).
        // The old exit_number*90 synth is REMOVED: it guessed the wrong convention (exit2 -> 180 = backward
        // vs real -45 = slight-left) and would corrupt a real straight-through exit (bearing 0). When Apple
        // leaves 0x346 == 0 the exit is straight -> angle 0 -> base roundabout glyph (correct by convention).
        // NOTE [needs-HUD]: the glyph-bank zero-reference/rotation is still unverified on a physical HUD.
        m.angle = a;
        return m;
    }

    switch (mtype) {
    case 1:  m.event = EV_TURN;   m.side = SIDE_LEFT;  break;  // LeftTurn
    case 2:  m.event = EV_TURN;   m.side = SIDE_RIGHT; break;  // RightTurn
    case 20: m.event = EV_TURN;   m.side = SIDE_LEFT;  break;  // LeftTurnAtEnd
    case 21: m.event = EV_TURN;   m.side = SIDE_RIGHT; break;  // RightTurnAtEnd
    case 47: m.event = EV_SHARP;  m.side = SIDE_LEFT;  break;  // SharpLeft
    case 48: m.event = EV_SHARP;  m.side = SIDE_RIGHT; break;  // SharpRight
    case 49: m.event = EV_SLIGHT; m.side = SIDE_LEFT;  break;  // SlightLeft
    case 50: m.event = EV_SLIGHT; m.side = SIDE_RIGHT; break;  // SlightRight
    case 13: m.event = EV_FORK;   m.side = SIDE_LEFT;  break;  // KeepLeft  (bear/fork)
    case 14: m.event = EV_FORK;   m.side = SIDE_RIGHT; break;  // KeepRight (bear/fork)
    case 51: m.event = EV_FORK;   m.side = SIDE_NONE;  break;  // ChangeHighway
    case 52: m.event = EV_FORK;   m.side = SIDE_LEFT;  break;  // ChangeHighwayLeft
    case 53: m.event = EV_FORK;   m.side = SIDE_RIGHT; break;  // ChangeHighwayRight
    case 4:  m.event = EV_UTURN;  m.side = (driveSide == 1) ? SIDE_RIGHT : SIDE_LEFT; break; // UTurn
    case 26: m.event = EV_UTURN;  m.side = (driveSide == 1) ? SIDE_RIGHT : SIDE_LEFT; break; // UTurnWhenPossible
    case 8:  m.event = EV_OFF_RAMP; m.side = SIDE_NONE;  break; // OffRamp
    case 22: m.event = EV_OFF_RAMP; m.side = SIDE_LEFT;  break; // HighwayOffRampLeft
    case 23: m.event = EV_OFF_RAMP; m.side = SIDE_RIGHT; break; // HighwayOffRampRight
    case 9:  m.event = EV_ON_RAMP;  m.side = SIDE_NONE;  break; // OnRamp
    case 15: case 16: case 17: m.event = EV_FERRY; m.side = SIDE_NONE; break; // Ferry
    case 10: case 12: case 27:  m.event = EV_DEST; m.side = SIDE_NONE;  break; // Arrive*
    case 24: m.event = EV_DEST; m.side = SIDE_LEFT;  break;  // ArriveDestinationLeft
    case 25: m.event = EV_DEST; m.side = SIDE_RIGHT; break;  // ArriveDestinationRight
    // 0/3/5/11/18 = context (also filtered by is_context_type); anything else -> straight
    default: m.event = EV_STRAIGHT; m.side = SIDE_NONE; break;
    }
    return m;
}

const char *read_str(const uint8_t *b, long n, int off, char *out, int outsz)
{
    int k = 0;
    if (off < 0) { out[0] = '\0'; return out; }
    for (; (long)(off + k) < n && k < outsz - 1 && b[off + k]; ++k) out[k] = (char)b[off + k];
    out[k] = '\0';
    return out;
}

uint32_t read_u32(const uint8_t *b, long n, int off)
{
    if (off < 0 || (long)(off + 4) > n) return 0;
    return *reinterpret_cast<const uint32_t *>(b + off);
}
int32_t read_s16(const uint8_t *b, long n, int off)
{
    if (off < 0 || (long)(off + 2) > n) return 0;
    return *reinterpret_cast<const int16_t *>(b + off);
}
uint32_t read_u16(const uint8_t *b, long n, int off)
{
    if (off < 0 || (long)(off + 2) > n) return 0;
    return *reinterpret_cast<const uint16_t *>(b + off);
}

// [BUILD-19] Map the guidance maneuverList head (a FULL burst index) to a buffered entry. g_buf is
// compacted (idx0 + context types skipped), so a head pointing at the heading/context entry won't match;
// fall through to head+1 (the next displayable, which the window always carries). Returns a g_buf index,
// or -1 if neither is buffered yet (caller KEEPS the previous selection — never flash buf[0]).
int select_by_head(uint32_t head)
{
    for (int i = 0; i < g_buf_count; ++i) if (g_buf[i].idx == head)     return i;
    for (int i = 0; i < g_buf_count; ++i) if (g_buf[i].idx == head + 1) return i;
    return -1;
}

// [BUILD-19] Latch + emit the chosen buffered maneuver (sel = a g_buf index from select_by_head).
// Selection is decided by the caller from the guidance maneuverList head; this only paints + dedups.
void emit_selection(int sel)
{
    if (sel < 0 || sel >= g_buf_count) return;
    const BufManeuver &im = g_buf[sel];
    // Road = THIS maneuver's OWN afterManeuverRoadName. Do NOT borrow the next maneuver's road when
    // empty — that painted a LATER maneuver's road ("ten duong sai"). Empty road -> blank.
    // [2026-06-13 #2] No afterManeuverRoadName (Apple leaves it empty for many turns) -> fall back to
    // the maneuver DESC ("Rẽ trái"/"Rẽ phải"/"Vòng ngược lại") so the strip shows the turn text
    // instead of going blank (we removed the old "borrow the next maneuver's road" hack).
    const char *road = im.road[0] ? im.road : im.desc;

    g_disp_event  = im.mv.event;
    g_disp_side   = im.mv.side;
    g_disp_angle  = im.mv.angle;
    g_disp_number = im.mv.number;
    strncpy(g_disp_road, road, sizeof(g_disp_road) - 1);
    g_disp_road[sizeof(g_disp_road) - 1] = '\0';

    LOGD("nav PICK sel=%d/%d idx=%u type=%u vi=%u ev=%u side=%u ang=%d num=%d road=\"%s\"",
         sel, g_buf_count, im.idx, im.mtype, im.validInfo,
         g_disp_event, g_disp_side, g_disp_angle, g_disp_number, g_disp_road);

    // Emit on change; the 2Hz keep-alive in hud_send persists it between bursts.
    if (g_disp_event != g_emit_event || g_disp_side != g_emit_side ||
        g_disp_angle != g_emit_angle || g_disp_number != g_emit_number ||
        strcmp(g_disp_road, g_emit_road) != 0) {
        g_emit_event = g_disp_event; g_emit_side = g_disp_side;
        g_emit_angle = g_disp_angle; g_emit_number = g_disp_number;
        strncpy(g_emit_road, g_disp_road, sizeof(g_emit_road) - 1);
        g_emit_road[sizeof(g_emit_road) - 1] = '\0';
        LOGD("nav HUD: event=%u side=%u angle=%d number=%d road=\"%s\"",
             g_disp_event, g_disp_side, g_disp_angle, g_disp_number, g_disp_road);
        hud_on_status(1);
        hud_on_next_turn(g_disp_road[0] ? g_disp_road : " ",
                         g_disp_side, g_disp_event, g_disp_angle, g_disp_number);
    }
}
} // namespace

// [2026-06-13 #3] Called by the HUD sender when it wipes the HUD (nav-off / unplug). We only FLAG
// it here — the actual reset of the emit-dedup + selection state happens in nav_on_devmgr_msg below,
// on the msgrcv thread that owns that state (no cross-thread race on the plain globals).
extern "C" void nav_request_reset(void)
{
    g_nav_reset_req.store(true, std::memory_order_relaxed);
}

extern "C" void nav_on_devmgr_msg(const void *msgp, long n)
{
    if (!msgp || n < (kManeuverTextOff + 4)) return;
    // [STABILITY] This runs on the OEM msgrcv RECEIVE thread (via the devmgr_shim msgrcv
    // interposer). An exception escaping into the non-exception-aware OEM frames calls
    // std::terminate -> SIGABRT -> jciCARPLAY dies (CarPlay + HUD lost until reboot) — the
    // same brick class the hud sender loop already guards, and the wrap AA puts on our_nav_cb.
    // Today's decode is non-throwing C-style code, so this is zero-behaviour future-proofing.
    try {
    const uint8_t *b = static_cast<const uint8_t *>(msgp);
    uint32_t mt = (*reinterpret_cast<const uint32_t *>(b)) & 0xffff;

    // [2026-06-13 #3] HUD was cleared (nav-off/unplug) -> reset selection + emit-dedup on THIS thread
    // so a re-started route always re-emits (even the same route, stationary). One-shot.
    if (g_nav_reset_req.exchange(false, std::memory_order_relaxed)) {
        g_buf_count = 0; g_last_head_valid = false; g_last_head = 0;
        g_prev_idx = 0xffffffff;
        g_emit_event = 0xffffffff; g_emit_side = 0xffffffff;
        g_emit_angle = 0x7fffffff; g_emit_number = 0x7fffffff;
        g_emit_road[0] = '\0';
    }

    if (mt == kMtManeuver) {
        uint32_t idx = (n >= kBurstIdxOff + 4)
                     ? (*reinterpret_cast<const uint32_t *>(b + kBurstIdxOff) >> 16) : 0;

        // New burst? (index restarts at 0 or drops). Flush the now-complete previous burst
        // if a guidance message didn't already (back-to-back bursts), then start fresh.
        bool burst_start = (idx == 0) || (idx < g_prev_idx);
        g_prev_idx = idx;
        // [BUILD-19] New burst (idx reset / drop): just clear the window. Selection is driven by the
        // guidance maneuverList head, not buf position, so there is nothing to "flush" here. g_last_head is
        // KEPT so the rebuilt window re-selects the same imminent.
        if (burst_start) {
            g_buf_count = 0;
        }

        uint32_t mtype     = read_u32(b, n, kManeuverTypeOff);
        // Skip idx0 (heading) and in-list context entries — buffer only displayable turns.
        if (idx == 0 || is_context_type(mtype)) return;
        if (g_buf_count >= kMaxBurst) return;

        uint32_t validInfo = read_u32(b, n, kValidInfoOff);
        uint32_t driveSide = read_u32(b, n, kDriveSideOff);
        uint32_t junction  = read_u32(b, n, kJunctionTypeOff);
        int32_t  jAngle    = read_s16(b, n, kJunctionAngleOff);
        char     road[256];
        read_str(b, n, kRoadNameOff, road, sizeof(road));
        char     text[kManeuverTextMax + 1];
        read_str(b, n, kManeuverTextOff, text, sizeof(text));

        Maneuver mv = classify(mtype, junction, jAngle, driveSide);

        BufManeuver &e = g_buf[g_buf_count++];
        e.mtype = mtype; e.idx = idx; e.validInfo = validInfo; e.mv = mv;
        strncpy(e.road, road, sizeof(e.road) - 1);
        e.road[sizeof(e.road) - 1] = '\0';
        strncpy(e.desc, text, sizeof(e.desc) - 1);   // [#2] keep desc as the strip-text fallback
        e.desc[sizeof(e.desc) - 1] = '\0';

        LOGD("nav MAN idx=%u type=%u vi=%u jt=%u jAng=%d ev=%u side=%u ang=%d num=%d "
             "desc=\"%s\" road=\"%s\"",
             idx, mtype, validInfo, junction, jAngle, mv.event, mv.side, mv.angle, mv.number,
             text, road);

        // [BUILD-19] Re-paint with the last known imminent head so a fresh burst (recalc) updates the same
        // maneuver, and a stationary re-start paints before the next guidance frame. If the head isn't
        // buffered yet (mid-rebuild) select_by_head returns -1 -> keep the current display (no flash).
        if (g_last_head_valid) {
            int sel = select_by_head(g_last_head);
            if (sel >= 0) emit_selection(sel);
        } else if (g_buf_count > 0) {
            // [VIETMAP-FALLBACK] Some CarPlay Maps apps (notably VietMap) stream 0x8059
            // maneuvers but leave the guidance active-maneuverList (iAP2 TLV 0x000D /
            // @0x458,@0x656) empty. Without a head we previously never called
            // emit_selection, so the HUD stayed blank forever. Bootstrap on the first
            // displayable entry; a later real head (Apple Maps) still overrides.
            LOGD("nav MAN bootstrap sel=0 (no guidance head yet, buf=%d)", g_buf_count);
            emit_selection(0);
        }

    } else if (mt == kMtGuidance) {
        // [BUILD-19] Imminent = Apple's active maneuverList[0] (iAP2 TLV 0x000D) carried in THIS guidance
        // msg — the authoritative "next maneuver" burst-index (disasm-proven @0x458, count @0x656). This
        // REPLACES the old content-anchor + dist-jump advance (which matched the dist-oracle only ~45-49%
        // on recalc-heavy drives, holding the HUD on an already-passed maneuver). Select the buffered
        // maneuver with that index; a context/heading head resolves to head+1 inside select_by_head.
        uint32_t cnt = read_u16(b, n, kManListCntOff);
        if (cnt > 0) {
            uint32_t head = read_u16(b, n, kManListOff);
            int sel = select_by_head(head);
            if (sel >= 0) {
                g_last_head = head;
                g_last_head_valid = true;
                emit_selection(sel);
            }
            // sel < 0: head not buffered yet (mid-rebuild / transient ~0.2%) -> KEEP previous selection.
        } else if (g_buf_count > 0 && !g_last_head_valid) {
            // [VIETMAP-FALLBACK] Guidance distance ticks arrive, but maneuverList count is 0.
            // Keep painting the first buffered turn so the HUD is not stuck blank.
            LOGD("nav GUIDE bootstrap sel=0 (maneuverList cnt=0, buf=%d)", g_buf_count);
            emit_selection(0);
        }
        // Distance readout (independent of selection).
        if (n >= kGuidanceDistOff + 4) {
            int32_t dist = (int32_t)(*reinterpret_cast<const uint32_t *>(b + kGuidanceDistOff));
            if (dist > 0 && dist <= 100000) {
                int32_t  disp; uint32_t unit;
                if (dist < 1000) { unit = 1; disp = dist * 1000; }  // meters
                else             { unit = 3; disp = dist;        }  // km
                hud_on_distance(dist, /*time_until*/0, disp, unit);
            }
        }
    }
    } catch (...) {
        LOGE("nav: nav_on_devmgr_msg exception swallowed — event dropped (no OEM-thread escape)");
    }
}
