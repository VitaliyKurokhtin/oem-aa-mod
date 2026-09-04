// SPDX-License-Identifier: AGPL-3.0-or-later
//
// hud_nav16 — Android Auto GAL 1.6 navigation decoder + Mazda HUD glyph/lane map.
// Handles NavigationState (msgId 0x8006) and NavigationCurrentPosition (0x8007).
//
// This is the single place that understands the Android-Auto 1.6 nav protocol:
// the aap_service shim relays raw frames over the IPC socket and blmjciaapa
// decodes + maps them here. Pure protobuf walk + constant lookup tables; no I/O,
// no logging. The Mazda glyph IDs come from MazdaIcon (hud_nav.h) — one enum,
// shared with the 1.5 path.

#ifndef LIBPATCH_BLMJCIAAPA_HUD_HUD_NAV16_H
#define LIBPATCH_BLMJCIAAPA_HUD_HUD_NAV16_H

#include <stdint.h>

enum { HUD_NAV16_MAX_LANES = 8 };

// One physical lane: the arrows it shows (Shape 0..9) as bitmasks, plus which
// are highlighted (recommended). bit s set iff a LaneDirection with shape==s.
struct AaLane {
    uint16_t present_mask;
    uint16_t highlight_mask;
};

// Decoded NavigationState (the active/first step — the next maneuver).
struct AaGuidance {
    bool     have_maneuver;
    uint32_t maneuver_type;            // NavigationManeuver.NavigationType, 0..42
    int32_t  roundabout_exit_number;   // field 2 — 1-based, in circulation order;
                                       // 0 when absent (see have_exit_number)
    bool     have_exit_number;         // field 2 was on the wire
    int32_t  roundabout_exit_angle;    // field 3 — degrees from the entry, increasing in
                                       // the driving direction; 0 when absent
    bool     have_exit_angle;          // field 3 was on the wire. Needed because an
                                       // absent angle and a real 0 are otherwise
                                       // indistinguishable, and they mean different
                                       // things: the protocol's angle range is
                                       // (0, 360], so 0 only ever means "not sent"
    char     road[64];                 // NavigationRoad.name (UTF-8)
    int      n_steps;                  // total steps present (diagnostic)
    int      n_lanes;                  // lanes on step[0] (0..8)
    AaLane   lanes[HUD_NAV16_MAX_LANES];
};

// Decoded NavigationCurrentPosition.
struct AaPosition {
    bool     have_step;                // distance to the next maneuver
    int32_t  step_meters;
    char     step_display[16];         // e.g. "350" / "1,3"
    uint32_t step_units;               // NavigationDistance.DistanceUnits 0..7
    bool     have_dest;
    int32_t  dest_meters;
    char     dest_display[16];
    uint32_t dest_units;
    char     eta[16];                  // estimated_time_at_arrival, e.g. "21:54"
};

// Dispatch on a FULL frame (leading 2-byte big-endian msgId): 0x8006 -> *g,
// 0x8007 -> *p. Returns the msgId (0 if too short). Pass NULL for the unwanted one.
uint32_t hud_nav16_on_frame(const uint8_t *raw, int size, AaGuidance *g, AaPosition *p);

// The maneuver-glyph map: decoded guidance -> Mazda HUD glyph (MazdaIcon, and
// 37..60 for roundabouts by exit angle). The single source of truth for the
// AA -> HUD maneuver pairing.
uint8_t hud_nav16_glyph(const AaGuidance *g);

// AA NavigationDistance.DistanceUnits (0..7) -> Mazda HUD unit (1=m,2=mi,3=km,
// 4=yd,5=ft; 0=none). The 1.6 unit map (distinct from the 1.5 NAVDistanceMessage
// map_distance_unit in hud_nav.h — different source enum).
uint8_t aa_to_mazda_unit(uint32_t units);

// Parse an AA display value ("350","1,3","1.3") to the HUD's value*10 form
// (one decimal): "350"->3500, "1,3"->13. 0 on garbage; overflow-capped.
int32_t parse_dist_x10(const char *s);

// Pure formatters (snprintf into caller buffer; no I/O) for one-line logging.
int hud_nav16_format_guidance(const AaGuidance *g, char *buf, int cap);
int hud_nav16_format_position(const AaPosition *p, char *buf, int cap);

// Read NavigationStatus (0x8003) field 1 (status enum varint) from a FULL frame
// (leading 2-byte big-endian msgId). Returns true and sets *status_out if found.
// Fully bounds-checked (same Pb/rd_tag/rd_varint/skip reader as the other decoders).
// *status_out is set to -1 (sentinel) on entry; unchanged if the field is absent or
// the frame is malformed. Safe on any phone-originated input.
bool hud_nav16_read_status(const uint8_t *raw, int size, int *status_out);

// === Push API: decode a raw frame and deliver to a registered sink ==========
//
// hud_nav16 owns ALL Android-Auto msgId dispatch + protobuf decode; the HUD
// consumer registers callbacks and receives the decoded structs, then does the
// HUD-domain mapping (glyph / units / change-gate / blank) itself. Keeps the AA
// protocol knowledge in exactly one translation unit. (The decoder entry points
// above stay public for the host self-test.)

// NavigationStatus (0x8003) + cluster lifecycle (0x8002 STOP), reported so the
// consumer can decide blank-vs-keep. hud_nav16 applies no HUD policy — it only
// reports what arrived.
struct AaStatus {
    int  nav_status;    // 0x8003 NavigationStatus enum: 0=UNAVAILABLE, 1=ACTIVE,
                        // 2=INACTIVE, 3=REROUTING; -1 if absent/malformed, or on
                        // a cluster STOP (see cluster_stop).
    bool cluster_stop;  // true iff this was a 0x8002 InstrumentClusterStop.
};

// Callbacks the consumer registers to receive decoded 1.6 nav. Each fires on the
// caller's (rx) thread. Any may be NULL to ignore that stream.
typedef void (*HudNav16GuidanceFn)(const AaGuidance *g);   // 0x8006 NavigationState
typedef void (*HudNav16PositionFn)(const AaPosition *p);   // 0x8007 CurrentPosition
typedef void (*HudNav16StatusFn)  (const AaStatus   *s);   // 0x8003 status / 0x8002 stop

// Register the callbacks hud_nav16_feed() delivers to. Call before the rx thread
// starts (feed is single-threaded w.r.t. this). Pass NULL for any/all to detach.
void hud_nav16_set_sink(HudNav16GuidanceFn on_guidance,
                        HudNav16PositionFn on_position,
                        HudNav16StatusFn   on_status);

// Decode one RAW frame (leading 2-byte big-endian msgId) and invoke the
// registered sink: 0x8006->on_guidance, 0x8007->on_position, 0x8003/0x8002->
// on_status. Other ids (cluster START, legacy turn/dist) are ignored. Runs on
// the rx thread; safe on truncated/malformed input.
void hud_nav16_feed(const uint8_t *frame, int len);

#endif  // LIBPATCH_BLMJCIAAPA_HUD_HUD_NAV16_H
