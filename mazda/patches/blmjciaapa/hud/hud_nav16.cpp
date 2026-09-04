// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Adapted from headunit (https://github.com/Trevelopment/headunit),
// licensed under GNU AGPL v3.
// See NOTICE.md at the repo root for the full attribution.
//
// hud_nav16 — AA GAL 1.6 NavigationState/CurrentPosition decoder + HUD glyph
// map. See header. Pure, bounds-checked; no I/O. Host self-test lives in
// test/nav16_test.cpp (wired into test/Makefile).

#include "hud_nav16.h"
#include "hud_nav.h"   // MazdaIcon glyph IDs (HUD_*) — one enum, shared with the 1.5 path
#include "common/aa_nav16_msg.h"   // AA_NAV16_MSG_* — the sender/receiver msgId contract

#include <cstring>
#include <cstdio>

namespace {

// ---- minimal, bounds-checked protobuf reader --------------------------------
struct Pb { const uint8_t *p; const uint8_t *end; };

bool rd_varint(Pb &c, uint64_t &v)
{
    v = 0; int shift = 0;
    while (c.p < c.end && shift < 64) {
        uint8_t b = *c.p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
    }
    return false;   // truncated or overlong
}

bool rd_tag(Pb &c, uint32_t &field, uint32_t &wire)
{
    if (c.p >= c.end) return false;
    uint64_t t;
    if (!rd_varint(c, t)) return false;
    field = (uint32_t)(t >> 3);
    wire  = (uint32_t)(t & 7);
    return true;
}

// length-delimited: yields a view [data, data+len) and advances past it.
bool rd_bytes(Pb &c, const uint8_t *&data, size_t &len)
{
    uint64_t l;
    if (!rd_varint(c, l)) return false;
    if ((uint64_t)(c.end - c.p) < l) return false;   // would overrun
    data = c.p; len = (size_t)l; c.p += l;
    return true;
}

bool skip(Pb &c, uint32_t wire)
{
    uint64_t v; const uint8_t *d; size_t l;
    switch (wire) {
        case 0: return rd_varint(c, v);                                   // varint
        case 1: if (c.end - c.p < 8) return false; c.p += 8; return true; // 64-bit
        case 2: return rd_bytes(c, d, l);                                 // len-delimited
        case 5: if (c.end - c.p < 4) return false; c.p += 4; return true; // 32-bit
        default: return false;                                            // groups: unexpected
    }
}

void copy_str(char *dst, size_t cap, const uint8_t *s, size_t n)
{
    if (cap == 0) return;
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, s, n);
    dst[n] = '\0';
}

// ---- NavigationDistance { meters=1, display_value=2, display_units=3 } -------
void dec_distance(const uint8_t *b, size_t n,
                  int32_t &meters, char *disp, size_t disp_cap, uint32_t &units)
{
    Pb c{b, b + n}; uint32_t f, w;
    while (rd_tag(c, f, w)) {
        if (f == 1 && w == 0) { uint64_t v; if (!rd_varint(c, v)) break; meters = (int32_t)v; }
        else if (f == 2 && w == 2) { const uint8_t *d; size_t l; if (!rd_bytes(c, d, l)) break; copy_str(disp, disp_cap, d, l); }
        else if (f == 3 && w == 0) { uint64_t v; if (!rd_varint(c, v)) break; units = (uint32_t)v; }
        else if (!skip(c, w)) break;
    }
}

// ---- NavigationRoad { name=1 string } : copy name out -----------------------
void dec_road_name(const uint8_t *b, size_t n, char *dst, size_t cap)
{
    Pb c{b, b + n}; uint32_t f, w;
    while (rd_tag(c, f, w)) {
        if (f == 1 && w == 2) { const uint8_t *d; size_t l; if (!rd_bytes(c, d, l)) break; copy_str(dst, cap, d, l); }
        else if (!skip(c, w)) break;
    }
}

// ---- LaneDirection { shape=1, is_highlighted=2 } ----------------------------
void dec_lane_direction(const uint8_t *b, size_t n, AaLane &lane)
{
    Pb c{b, b + n}; uint32_t f, w;
    uint64_t shape = 0; bool have_shape = false, hi = false;
    while (rd_tag(c, f, w)) {
        if (f == 1 && w == 0) { uint64_t v; if (!rd_varint(c, v)) break; shape = v; have_shape = true; }
        else if (f == 2 && w == 0) { uint64_t v; if (!rd_varint(c, v)) break; hi = (v != 0); }
        else if (!skip(c, w)) break;
    }
    if (have_shape && shape <= 9) {
        lane.present_mask |= (uint16_t)(1u << shape);
        if (hi) lane.highlight_mask |= (uint16_t)(1u << shape);
    }
}

// ---- NavigationLane { repeated LaneDirection lane_directions=1 } -------------
void dec_lane(const uint8_t *b, size_t n, AaLane &lane)
{
    Pb c{b, b + n}; uint32_t f, w;
    while (rd_tag(c, f, w)) {
        if (f == 1 && w == 2) { const uint8_t *d; size_t l; if (!rd_bytes(c, d, l)) break; dec_lane_direction(d, l, lane); }
        else if (!skip(c, w)) break;
    }
}

// ---- NavigationManeuver { type=1, roundabout_exit_number=2, _exit_angle=3 } --
void dec_maneuver(const uint8_t *b, size_t n, AaGuidance &g)
{
    Pb c{b, b + n}; uint32_t f, w;
    while (rd_tag(c, f, w)) {
        if (f == 1 && w == 0) { uint64_t v; if (!rd_varint(c, v)) break; g.maneuver_type = (uint32_t)v; g.have_maneuver = true; }
        else if (f == 2 && w == 0) { uint64_t v; if (!rd_varint(c, v)) break; g.roundabout_exit_number = (int32_t)v; g.have_exit_number = true; }
        else if (f == 3 && w == 0) { uint64_t v; if (!rd_varint(c, v)) break; g.roundabout_exit_angle = (int32_t)v; g.have_exit_angle = true; }
        else if (!skip(c, w)) break;
    }
}

// ---- NavigationStep { maneuver=1, road=2, lanes=3 (rep), cue=4 } -------------
void dec_step(const uint8_t *b, size_t n, AaGuidance &g)
{
    Pb c{b, b + n}; uint32_t f, w;
    while (rd_tag(c, f, w)) {
        if (w == 2) {
            const uint8_t *d; size_t l; if (!rd_bytes(c, d, l)) break;
            if      (f == 1) dec_maneuver(d, l, g);
            else if (f == 2) dec_road_name(d, l, g.road, sizeof(g.road));
            else if (f == 3) {
                if (g.n_lanes < HUD_NAV16_MAX_LANES) {
                    dec_lane(d, l, g.lanes[g.n_lanes]);
                    g.n_lanes++;
                }
            }
            // f == 4 (cue): redundant with road for the HUD strip — skip.
        } else if (!skip(c, w)) break;
    }
}

// ---- NavigationStepDistance { distance=1, time_to_step_seconds=2 } -----------
void dec_step_distance(const uint8_t *b, size_t n, AaPosition &p)
{
    Pb c{b, b + n}; uint32_t f, w;
    while (rd_tag(c, f, w)) {
        if (f == 1 && w == 2) {
            const uint8_t *d; size_t l; if (!rd_bytes(c, d, l)) break;
            dec_distance(d, l, p.step_meters, p.step_display, sizeof(p.step_display), p.step_units);
            p.have_step = true;
        } else if (!skip(c, w)) break;
    }
}

// ---- NavigationDestinationDistance { distance=1, eta=2, ttl_seconds=3 } ------
void dec_dest_distance(const uint8_t *b, size_t n, AaPosition &p)
{
    Pb c{b, b + n}; uint32_t f, w;
    while (rd_tag(c, f, w)) {
        if (f == 1 && w == 2) {
            const uint8_t *d; size_t l; if (!rd_bytes(c, d, l)) break;
            dec_distance(d, l, p.dest_meters, p.dest_display, sizeof(p.dest_display), p.dest_units);
            p.have_dest = true;
        } else if (f == 2 && w == 2) {
            const uint8_t *d; size_t l; if (!rd_bytes(c, d, l)) break; copy_str(p.eta, sizeof(p.eta), d, l);
        } else if (!skip(c, w)) break;
    }
}

const char *kManeuver[] = {
    "UNKNOWN","DEPART","NAME_CHANGE","KEEP_LEFT","KEEP_RIGHT","TURN_SLIGHT_LEFT",
    "TURN_SLIGHT_RIGHT","TURN_NORMAL_LEFT","TURN_NORMAL_RIGHT","TURN_SHARP_LEFT",
    "TURN_SHARP_RIGHT","U_TURN_LEFT","U_TURN_RIGHT","ON_RAMP_SLIGHT_LEFT",
    "ON_RAMP_SLIGHT_RIGHT","ON_RAMP_NORMAL_LEFT","ON_RAMP_NORMAL_RIGHT",
    "ON_RAMP_SHARP_LEFT","ON_RAMP_SHARP_RIGHT","ON_RAMP_U_TURN_LEFT",
    "ON_RAMP_U_TURN_RIGHT","OFF_RAMP_SLIGHT_LEFT","OFF_RAMP_SLIGHT_RIGHT",
    "OFF_RAMP_NORMAL_LEFT","OFF_RAMP_NORMAL_RIGHT","FORK_LEFT","FORK_RIGHT",
    "MERGE_LEFT","MERGE_RIGHT","MERGE_SIDE_UNSPECIFIED","ROUNDABOUT_ENTER",
    "ROUNDABOUT_EXIT","RA_ENTER_EXIT_CW","RA_ENTER_EXIT_CW_ANGLE","RA_ENTER_EXIT_CCW",
    "RA_ENTER_EXIT_CCW_ANGLE","STRAIGHT","FERRY_BOAT","FERRY_TRAIN","DESTINATION",
    "DESTINATION_STRAIGHT","DESTINATION_LEFT","DESTINATION_RIGHT",
};

// AA NavigationType (0..42) -> Mazda HUD glyph. Values cross-referenced to the
// MazdaIcon 1.5 glyph atlas in hud_nav.h (the road-validated reference table);
// KEEP_LEFT/RIGHT are FORK glyphs (NNG-confirmed). On-ramps reuse the turn
// glyphs by severity/side (no dedicated on-ramp glyph). Roundabout ENTER_AND_EXIT
// (32..35) are resolved by exit angle in hud_nav16_glyph(), NOT from this table;
// the entries here are unused fallbacks.
const uint8_t kManeuverGlyph[43] = {
    /* 0  UNKNOWN                */ HUD_EMPTY,
    /* 1  DEPART                 */ HUD_FLAG,
    /* 2  NAME_CHANGE            */ HUD_STRAIGHT,
    /* 3  KEEP_LEFT              */ HUD_FORK_LEFT,
    /* 4  KEEP_RIGHT             */ HUD_FORK_RIGHT,
    /* 5  TURN_SLIGHT_LEFT       */ HUD_SLIGHT_LEFT,
    /* 6  TURN_SLIGHT_RIGHT      */ HUD_SLIGHT_RIGHT,
    /* 7  TURN_NORMAL_LEFT       */ HUD_LEFT,
    /* 8  TURN_NORMAL_RIGHT      */ HUD_RIGHT,
    /* 9  TURN_SHARP_LEFT        */ HUD_SHARP_LEFT,
    /* 10 TURN_SHARP_RIGHT       */ HUD_SHARP_RIGHT,
    /* 11 U_TURN_LEFT            */ HUD_U_TURN_LEFT,
    /* 12 U_TURN_RIGHT           */ HUD_U_TURN_RIGHT,
    /* 13 ON_RAMP_SLIGHT_LEFT    */ HUD_SLIGHT_LEFT,
    /* 14 ON_RAMP_SLIGHT_RIGHT   */ HUD_SLIGHT_RIGHT,
    /* 15 ON_RAMP_NORMAL_LEFT    */ HUD_LEFT,
    /* 16 ON_RAMP_NORMAL_RIGHT   */ HUD_RIGHT,
    /* 17 ON_RAMP_SHARP_LEFT     */ HUD_SHARP_LEFT,
    /* 18 ON_RAMP_SHARP_RIGHT    */ HUD_SHARP_RIGHT,
    /* 19 ON_RAMP_U_TURN_LEFT    */ HUD_U_TURN_LEFT,
    /* 20 ON_RAMP_U_TURN_RIGHT   */ HUD_U_TURN_RIGHT,
    /* 21 OFF_RAMP_SLIGHT_LEFT   */ HUD_OFF_RAMP_LEFT,
    /* 22 OFF_RAMP_SLIGHT_RIGHT  */ HUD_OFF_RAMP_RIGHT,
    /* 23 OFF_RAMP_NORMAL_LEFT   */ HUD_OFF_RAMP_LEFT,
    /* 24 OFF_RAMP_NORMAL_RIGHT  */ HUD_OFF_RAMP_RIGHT,
    /* 25 FORK_LEFT              */ HUD_FORK_LEFT,
    /* 26 FORK_RIGHT             */ HUD_FORK_RIGHT,
    /* 27 MERGE_LEFT             */ HUD_MERGE_LEFT,
    /* 28 MERGE_RIGHT            */ HUD_MERGE_RIGHT,
    /* 29 MERGE_SIDE_UNSPECIFIED */ HUD_STRAIGHT,
    /* 30 ROUNDABOUT_ENTER       */ HUD_STRAIGHT,   // no angle -> generic (OEM has no plain roundabout glyph)
    /* 31 ROUNDABOUT_EXIT        */ HUD_STRAIGHT,
    /* 32 RA_ENTER_EXIT_CW       */ HUD_STRAIGHT,   // resolved by angle in hud_nav16_glyph()
    /* 33 RA_ENTER_EXIT_CW_ANGLE */ HUD_STRAIGHT,
    /* 34 RA_ENTER_EXIT_CCW      */ HUD_STRAIGHT,
    /* 35 RA_ENTER_EXIT_CCW_ANG  */ HUD_STRAIGHT,
    /* 36 STRAIGHT               */ HUD_STRAIGHT,
    /* 37 FERRY_BOAT             */ HUD_EMPTY,       // no glyph
    /* 38 FERRY_TRAIN            */ HUD_EMPTY,
    /* 39 DESTINATION            */ HUD_DESTINATION,
    /* 40 DESTINATION_STRAIGHT   */ HUD_DESTINATION,
    /* 41 DESTINATION_LEFT       */ HUD_DESTINATION_LEFT,
    /* 42 DESTINATION_RIGHT      */ HUD_DESTINATION_RIGHT,
};

// Roundabout exit glyph by angle. The Mazda HUD ECU has 12 directional glyphs
// per traffic side: right-hand traffic (CCW exits) 37..48, left-hand (CW) 49..60.
// id = base + round(angle/30) mod 12. (Formula from hud_nav.h roundabout_icon.)
uint8_t roundabout_glyph(int32_t exit_angle, bool clockwise)
{
    int a = exit_angle % 360; if (a < 0) a += 360;
    int idx = ((a + 15) / 30) % 12;                       // 0..11
    return (uint8_t)((clockwise ? HUD_ROUNDABOUT_CW_BASE
                                : HUD_ROUNDABOUT_CCW_BASE) + idx);
}

// Circulation angle to assume when a roundabout step carries no exit angle,
// keyed by roundabout_exit_number (index 0 = no usable number either).
//
// Exit N of an E-arm roundabout — arms evenly spaced, entry at 0 — sits at
// 360*N/E. E is never on the wire, so the angle can only be estimated; these
// are the values that minimise expected angular error over the plausible arm
// counts, and the choice is unchanged whether 3-arm or 4-arm roundabouts
// dominate. It is N*90 up to exit 3 and then saturates, because a 4th exit
// implies at least 5 arms.
//
// Accuracy: about one 30-degree glyph step on average. The worst cases are a
// 3-arm roundabout taken at exit 2 (truly 240) and a 6-arm at exit 3 (truly
// 180), where the estimate is 60-90 degrees out. That is the accepted cost of
// having no arm count; it is bounded, unlike the alternative of treating an
// absent angle as 0, which lands on the "back out the entry" glyph every time.
int32_t exit_number_angle(int32_t exit_number)
{
    static const int16_t kAngle[] = { 180, 90, 180, 270, 270, 300, 300 };
    if (exit_number < 0) exit_number = 0;
    if (exit_number > 6) exit_number = 6;
    return kAngle[exit_number];
}

// The circulation angle to render for a roundabout: the sender's own angle when
// it sent one, else the exit-number estimate, else straight through.
//
// Handedness is NOT decided here — it comes from the maneuver type (32/33 are
// clockwise, 34/35 counterclockwise), so it is always what the sender stated and
// never inferred from the vehicle or the market. The estimate above is
// handedness-independent because the angle increments in the driving direction
// in both banks, so left-hand-traffic roundabouts need no separate table.
int32_t roundabout_angle(const AaGuidance *g)
{
    if (g->have_exit_angle) return g->roundabout_exit_angle;
    return exit_number_angle(g->have_exit_number ? g->roundabout_exit_number : 0);
}

} // namespace

static const char *hud_nav16_maneuver_name(uint32_t t)
{
    return (t < sizeof(kManeuver)/sizeof(kManeuver[0])) ? kManeuver[t] : "?";
}

uint8_t hud_nav16_glyph(const AaGuidance *g)
{
    if (!g) return HUD_BLANK;
    switch (g->maneuver_type) {
        case 32: case 33:  // RA_ENTER_EXIT_CW (clockwise = left-hand traffic)
            return roundabout_glyph(roundabout_angle(g), /*clockwise=*/true);
        case 34: case 35:  // RA_ENTER_EXIT_CCW (counterclockwise = right-hand traffic)
            return roundabout_glyph(roundabout_angle(g), /*clockwise=*/false);
        default:
            return (g->maneuver_type < 43) ? kManeuverGlyph[g->maneuver_type] : HUD_EMPTY;
    }
}

// AA NavigationDistance.DistanceUnits (0..7) -> Mazda HUD unit
// (1=m, 2=mi, 3=km, 4=yd, 5=ft).
uint8_t aa_to_mazda_unit(uint32_t u)
{
    switch (u) {
        case 1: return 1;          // METERS
        case 2: case 3: return 3;  // KILOMETERS / KILOMETERS_P1
        case 4: case 5: return 2;  // MILES / MILES_P1
        case 6: return 5;          // FEET
        case 7: return 4;          // YARDS
        default: return 0;         // unknown -> HUD renders no unit
    }
}

// Parse the AA display value ("350", "1,3", "1.3") to the HUD's value*10 form
// (one decimal): "350" -> 3500, "1,3" -> 13. Returns 0 on garbage.
int32_t parse_dist_x10(const char *s)
{
    if (!s) return 0;
    long ip = 0; int frac = 0; bool sep = false, any = false;
    for (const char *p = s; *p; ++p) {
        if (*p >= '0' && *p <= '9') {
            if (!sep) {
                // Cap accumulation to prevent signed-overflow UB on ARM32 (long=32-bit);
                // realistic distances are tiny, so this only fires on hostile input.
                if (ip < 100000000L) ip = ip * 10 + (*p - '0');
                any = true;
            } else { frac = *p - '0'; break; }   // first fractional digit only
        } else if (*p == ',' || *p == '.') {
            sep = true;
        }
    }
    return any ? (int32_t)(ip * 10 + frac) : 0;
}

static bool hud_nav16_decode_navstate(const uint8_t *proto, int len, AaGuidance *out)
{
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    if (!proto || len < 0) return false;
    Pb c{proto, proto + len}; uint32_t f, w; bool got_step = false;
    while (rd_tag(c, f, w)) {
        if (f == 1 && w == 2) {                 // repeated NavigationStep steps
            const uint8_t *d; size_t l; if (!rd_bytes(c, d, l)) break;
            out->n_steps++;
            if (!got_step) { dec_step(d, l, *out); got_step = true; }   // step[0] = next maneuver
        } else if (!skip(c, w)) break;          // destinations(2) etc. ignored for the HUD
    }
    return true;
}

static bool hud_nav16_decode_position(const uint8_t *proto, int len, AaPosition *out)
{
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    if (!proto || len < 0) return false;
    Pb c{proto, proto + len}; uint32_t f, w; bool got_dest = false;
    while (rd_tag(c, f, w)) {
        if (f == 1 && w == 2) {                 // step_distance
            const uint8_t *d; size_t l; if (!rd_bytes(c, d, l)) break; dec_step_distance(d, l, *out);
        } else if (f == 2 && w == 2) {          // repeated destination_distances (use first)
            const uint8_t *d; size_t l; if (!rd_bytes(c, d, l)) break;
            if (!got_dest) { dec_dest_distance(d, l, *out); got_dest = true; }
        } else if (!skip(c, w)) break;          // current_road(3) ignored
    }
    return true;
}

uint32_t hud_nav16_on_frame(const uint8_t *raw, int size, AaGuidance *g, AaPosition *p)
{
    if (!raw || size < 2) return 0;
    uint32_t id = ((uint32_t)raw[0] << 8) | raw[1];   // big-endian msgId
    const uint8_t *body = raw + 2; int blen = size - 2;
    if (id == AA_NAV16_MSG_NAV_STATE && g) hud_nav16_decode_navstate(body, blen, g);
    else if (id == AA_NAV16_MSG_POSITION && p) hud_nav16_decode_position(body, blen, p);
    return id;
}

// Read NavigationStatus (0x8003) field 1 (status enum varint) from a full frame
// (leading 2-byte big-endian msgId). Returns true and sets *status_out if found.
// Fully bounds-checked: uses the same Pb/rd_tag/rd_varint/skip reader as the
// other decoders — no hand-rolled walk, no attacker-controlled pointer arithmetic.
// *status_out is initialised to -1 (sentinel) up front; caller sees -1 if the
// field is absent or the frame is truncated/malformed.
bool hud_nav16_read_status(const uint8_t *raw, int size, int *status_out)
{
    if (status_out) *status_out = -1;
    if (!raw || size < 2 || !status_out) return false;
    // skip the 2-byte big-endian msgId, walk the protobuf body
    Pb c{raw + 2, raw + size};
    uint32_t f, w;
    while (rd_tag(c, f, w)) {
        if (f == 1 && w == 0) {          // field 1, wire=0 (varint): NavigationStatusEnum
            uint64_t v;
            if (!rd_varint(c, v)) return false;
            *status_out = (int)(uint32_t)v;
            return true;
        }
        if (!skip(c, w)) return false;   // unknown field or malformed frame
    }
    return false;   // field 1 not present
}

int hud_nav16_format_guidance(const AaGuidance *g, char *buf, int cap)
{
    if (!g || !buf || cap <= 0) return 0;
    int o = snprintf(buf, cap, "nav16 STATE: maneuver=%u(%s) glyph=%u road=\"%s\" steps=%d lanes=%d",
                          g->maneuver_type, hud_nav16_maneuver_name(g->maneuver_type),
                          hud_nav16_glyph(g), g->road, g->n_steps, g->n_lanes);
    for (int i = 0; i < g->n_lanes && o < cap; ++i)
        o += snprintf(buf + o, cap - o, " [L%d pres=0x%03x hi=0x%03x]",
                           i, g->lanes[i].present_mask, g->lanes[i].highlight_mask);
    // Echo the roundabout inputs so a log shows which branch produced the glyph
    // above: a sent angle, or the exit-number estimate standing in for one.
    if ((g->have_exit_number || g->have_exit_angle) && o > 0 && o < cap) {
        char ex[16], an[16];
        if (g->have_exit_number) snprintf(ex, sizeof ex, "%d", g->roundabout_exit_number);
        else                     snprintf(ex, sizeof ex, "none");
        if (g->have_exit_angle)  snprintf(an, sizeof an, "%d", g->roundabout_exit_angle);
        else                     snprintf(an, sizeof an, "none");
        o += snprintf(buf + o, cap - o, " ra[exit=%s angle=%s]", ex, an);
    }
    return o;
}

int hud_nav16_format_position(const AaPosition *p, char *buf, int cap)
{
    if (!p || !buf || cap <= 0) return 0;
    return snprintf(buf, cap,
        "nav16 POS: step=%dm \"%s\" u%u | dest=%dm \"%s\" u%u eta=\"%s\"",
        p->step_meters, p->step_display, p->step_units,
        p->dest_meters, p->dest_display, p->dest_units, p->eta);
}

// === Push API =================================================================

// Registered callbacks. Set on the lifecycle thread before the rx thread starts
// and cleared after it stops (see nav16_rx.cpp), so hud_nav16_feed() — which only
// runs on the rx thread — never races them.
static HudNav16GuidanceFn g_on_guidance = nullptr;
static HudNav16PositionFn g_on_position = nullptr;
static HudNav16StatusFn   g_on_status   = nullptr;

void hud_nav16_set_sink(HudNav16GuidanceFn on_guidance,
                        HudNav16PositionFn on_position,
                        HudNav16StatusFn   on_status)
{
    g_on_guidance = on_guidance;
    g_on_position = on_position;
    g_on_status   = on_status;
}

void hud_nav16_feed(const uint8_t *frame, int len)
{
    if (!frame || len < 2) return;

    const uint32_t id = ((uint32_t)frame[0] << 8) | frame[1];   // big-endian msgId
    switch (id) {
    case AA_NAV16_MSG_NAV_STATE: {          // 0x8006 — maneuver + road + lanes
        if (!g_on_guidance) break;
        AaGuidance g;
        hud_nav16_on_frame(frame, len, &g, nullptr);
        g_on_guidance(&g);
        break;
    }
    case AA_NAV16_MSG_POSITION: {           // 0x8007 — distance to next maneuver
        if (!g_on_position) break;
        AaPosition p;
        hud_nav16_on_frame(frame, len, nullptr, &p);
        g_on_position(&p);
        break;
    }
    case AA_NAV16_MSG_STATUS: {             // 0x8003 — NavigationStatus lifecycle
        if (!g_on_status) break;
        AaStatus s;
        s.nav_status   = -1;
        s.cluster_stop = false;
        hud_nav16_read_status(frame, len, &s.nav_status);
        g_on_status(&s);
        break;
    }
    case AA_NAV16_MSG_CLUSTER_STOP: {       // 0x8002 — blank the HUD
        if (!g_on_status) break;
        AaStatus s;
        s.nav_status   = -1;
        s.cluster_stop = true;
        g_on_status(&s);
        break;
    }
    default:
        // CLUSTER_START (0x8001) / NEXT_TURN (0x8004) / NEXT_TURN_DIST (0x8005):
        // nothing to render.
        break;
    }
}
