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

#include "hud_lane.h"   // shared OEM lane-code space + AA->OEM mapping (version-agnostic)

enum { HUD_NAV16_MAX_LANES = 8 };

// One physical lane: the arrows it shows (Shape 0..9) as bitmasks, plus which
// are highlighted (recommended). bit s set iff a LaneDirection with shape==s.
struct AaLane {
    uint16_t present_mask;
    uint16_t highlight_mask;
};

// Lane guidance is produced as OEM lane CODES (see hud_lane.h / oem_lane_code_for_aa).
// Two transports consume them:
//   * svcnavi path: emits the CODES and lets svcjcinavi do code->glyph + A/B
//     (aa_nav16_lane_codes below).
//   * vbs path: bypasses svcjcinavi, so it takes those same CODES and applies the
//     code->glyph + A/B map itself (hud_lane.h::oem_lane_glyph) before sending.

// Fill the 8 lane slots with OEM lane CODES (both transports call this). The full
// AA-shape -> OEM-code mapping (single arrows, combos, and the recommended-direction
// detail codes) lives in the version-agnostic hud_lane.h; here we only fill the slots
// and hide empties. `hidden` is the CALLER's per-slot "no lane" sentinel: svcnavi
// passes 0 (svcjcinavi validates each arg to 0..0x46); the vbs path passes its own
// 0xFF empty-slot marker.
inline void aa_nav16_lane_codes(const AaLane *lanes, uint8_t n,
                                uint8_t out[8], uint8_t hidden)
{
    for (int i = 0; i < 8; ++i) {
        if (!lanes || i >= (int)n) {
            out[i] = hidden;
            continue;
        }
        const OemLaneCode c = oem_lane_code_for_aa(lanes[i].present_mask,
                                                   lanes[i].highlight_mask);
        out[i] = (c == OEM_LANE_NONE) ? hidden : (uint8_t)c;
    }
}

// Decoded NavigationState (the active/first step — the next maneuver).
struct AaGuidance {
    bool     have_maneuver;
    uint32_t maneuver_type;            // NavigationManeuver.NavigationType, 0..42
    int32_t  roundabout_exit_number;   // field 2 — meaningful for ROUNDABOUT_*
    int32_t  roundabout_exit_angle;    // field 3 — degrees; selects the roundabout glyph
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

// Decode the protobuf BODY (AFTER the 2-byte big-endian msgId). *out is zeroed
// first; returns true on a clean walk (partial/unknown fields tolerated). Fully
// bounds-checked — safe on truncated/malformed input (runs in a reset_board PID).
bool hud_nav16_decode_navstate(const uint8_t *proto, int len, AaGuidance *out);
bool hud_nav16_decode_position (const uint8_t *proto, int len, AaPosition *out);

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

// Name tables for logging (NULL-safe bounds): type 0..42 / shape 0..9.
const char *hud_nav16_maneuver_name(uint32_t type);
const char *hud_nav16_shape_name(int shape);

// Read NavigationStatus (0x8003) field 1 (status enum varint) from a FULL frame
// (leading 2-byte big-endian msgId). Returns true and sets *status_out if found.
// Fully bounds-checked (same Pb/rd_tag/rd_varint/skip reader as the other decoders).
// *status_out is set to -1 (sentinel) on entry; unchanged if the field is absent or
// the frame is malformed. Safe on any phone-originated input.
bool hud_nav16_read_status(const uint8_t *raw, int size, int *status_out);

#endif  // LIBPATCH_BLMJCIAAPA_HUD_HUD_NAV16_H
