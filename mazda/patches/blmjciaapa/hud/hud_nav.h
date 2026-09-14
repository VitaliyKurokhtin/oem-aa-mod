// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Adapted from headunit (https://github.com/Trevelopment/headunit),
// licensed under GNU AGPL v3.
// See NOTICE.md at the repo root for the full attribution.
//
// Shared HUD navigation mapping — the transport-agnostic translation
// between Android Auto nav fields and the Mazda HUD ECU's own
// encodings. Both HUD transports (vbs_tx and svcnavi_tx) need exactly
// the same glyph table and unit mapping (svcjcinavi forwards dirIcon
// and distUnit straight through as the HUD frame's nextManeuverInfo
// and distanceUnit, so the codes are identical on both paths), so it
// lives here instead of being duplicated.
//
// Header-only: everything is constexpr data or pure inline functions
// with no transport coupling and no global state, so there is no
// matching .cpp and no link-time concern.

#ifndef LIBPATCH_BLMJCIAAPA_HUD_NAV_H
#define LIBPATCH_BLMJCIAAPA_HUD_NAV_H

#include <stdint.h>

// === Mazda HUD turn-icon enum =================================
//
// Mirrors `NaviTurns` in reference hud.h. These are the integer
// codes the HUD ECU interprets — the OEM glyph atlas is baked
// into the ECU firmware and is out of our scope. Same numbering
// as the reference, no remapping.
enum MazdaIcon : uint8_t {
    HUD_BLANK              = 0,    // HUD draws nothing (implicit blank; named for the 1.6 map)
    HUD_STRAIGHT            = 1,
    HUD_LEFT                = 2,
    HUD_RIGHT               = 3,
    HUD_SLIGHT_LEFT         = 4,
    HUD_SLIGHT_RIGHT        = 5,
    HUD_UNDER_BRIDGE        = 6,
    HUD_OFF_RAMP_RIGHT      = 7,
    HUD_DESTINATION         = 8,
    HUD_SHARP_RIGHT         = 9,
    HUD_U_TURN_RIGHT        = 10,
    HUD_SHARP_LEFT          = 11,
    HUD_FLAG                = 12,
    HUD_U_TURN_LEFT         = 13,
    HUD_FORK_RIGHT          = 14,
    HUD_FORK_LEFT           = 15,
    HUD_MERGE_LEFT          = 16,
    HUD_MERGE_RIGHT         = 17,
    HUD_EMPTY               = 18,
    // 19 is empty
    HUD_CROSS_RIGHT         = 20,
    HUD_CROSS_LEFT          = 21,
    HUD_MEDIAN_U_TURN_LEFT  = 22,
    HUD_MEDIAN_U_TURN_RIGHT = 23,
    HUD_CAR                 = 24,
    HUD_NO_CAR              = 25,
    // 26..29 are empty
    HUD_OFF_RAMP_LEFT       = 30,
    HUD_T_LEFT              = 31,
    HUD_T_RIGHT             = 32,
    HUD_DESTINATION_LEFT    = 33,
    HUD_DESTINATION_RIGHT   = 34,
    HUD_FLAG_LEFT           = 35,
    HUD_FLAG_RIGHT          = 36,
    // Roundabout exit-glyph bases (12 directional glyphs per traffic side); the
    // exit angle is added in roundabout_icon()/the 1.6 roundabout map. Named here
    // so the 1.6 glyph map has a single source for them (1.5 roundabout_icon below
    // uses the same 37/49 offsets).
    HUD_ROUNDABOUT_CCW_BASE = 37,   // right-hand traffic, +round(angle/30) -> 37..48
    HUD_ROUNDABOUT_CW_BASE  = 49,   // left-hand  traffic, +round(angle/30) -> 49..60
};

// === Android turn_event → Mazda icon ==========================
//
// Lookup: kTurnIcons[android_turn_event][side_index]
//   side_index: 0=LEFT, 1=RIGHT, 2=UNSPECIFIED/STRAIGHT
//
// Indexed by hu.proto NAVTurnMessage.TURN_EVENT (0..19, sparse at
// 15 and 18). A `0` entry means "no glyph" — the HUD draws blank.
// ROUNDABOUT_ENTER_AND_EXIT (13) is handled separately by
// roundabout_icon() because the icon depends on exit angle.
//
// This table is copied verbatim from reference hud.cpp's turns[][]
// (just renamed). The reference table has been validated on real
// cars; do not "improve" it without a road test.
constexpr uint8_t kTurnIcons[20][3] = {
    /*  0 TURN_UNKNOWN                  */ {0, 0, 0},
    /*  1 TURN_DEPART                   */ {HUD_FLAG_LEFT, HUD_FLAG_RIGHT, HUD_FLAG},
    /*  2 TURN_NAME_CHANGE              */ {HUD_STRAIGHT, HUD_STRAIGHT, HUD_STRAIGHT},
    /*  3 TURN_SLIGHT_TURN              */ {HUD_SLIGHT_LEFT, HUD_SLIGHT_RIGHT, HUD_STRAIGHT},
    /*  4 TURN_TURN                     */ {HUD_LEFT, HUD_RIGHT, HUD_STRAIGHT},
    /*  5 TURN_SHARP_TURN               */ {HUD_SHARP_LEFT, HUD_SHARP_RIGHT, HUD_STRAIGHT},
    /*  6 TURN_U_TURN                   */ {HUD_U_TURN_LEFT, HUD_U_TURN_RIGHT, HUD_STRAIGHT},
    /*  7 TURN_ON_RAMP                  */ {HUD_LEFT, HUD_RIGHT, HUD_STRAIGHT},
    /*  8 TURN_OFF_RAMP                 */ {HUD_OFF_RAMP_LEFT, HUD_OFF_RAMP_RIGHT, HUD_STRAIGHT},
    /*  9 TURN_FORK                     */ {HUD_FORK_LEFT, HUD_FORK_RIGHT, HUD_STRAIGHT},
    /* 10 TURN_MERGE                    */ {HUD_MERGE_LEFT, HUD_MERGE_RIGHT, HUD_STRAIGHT},
    /* 11 TURN_ROUNDABOUT_ENTER         */ {0, 0, 0},
    /* 12 TURN_ROUNDABOUT_EXIT          */ {0, 0, 0},
    /* 13 TURN_ROUNDABOUT_ENTER_AND_EXIT*/ {0, 0, 0},  // handled by roundabout_icon()
    /* 14 TURN_STRAIGHT                 */ {HUD_STRAIGHT, HUD_STRAIGHT, HUD_STRAIGHT},
    /* 15 unassigned in proto           */ {0, 0, 0},
    /* 16 TURN_FERRY_BOAT               */ {0, 0, 0},
    /* 17 TURN_FERRY_TRAIN              */ {0, 0, 0},
    /* 18 unassigned in proto           */ {0, 0, 0},
    /* 19 TURN_DESTINATION              */ {HUD_DESTINATION_LEFT, HUD_DESTINATION_RIGHT, HUD_DESTINATION},
};

// Roundabout exit icon — 12 directional roundabout glyphs per
// side, indexed by exit angle (rounded to nearest 30°). Offsets
// 37 (right-hand traffic) and 49 (left-hand traffic) come from
// the reference implementation; the HUD ECU has roundabout
// glyphs at IDs 37..48 and 49..60.
inline uint8_t roundabout_icon(int32_t degrees, int32_t side_index_lr)
{
    uint8_t nearest = static_cast<uint8_t>(((degrees + 15) / 30) % 12);
    uint8_t offset  = (side_index_lr == 0) ? 49 : 37;
    return static_cast<uint8_t>(nearest + offset);
}

// === Distance-unit enum translation ===========================
//
// hu.proto DISPLAY_DISTANCE_UNIT:
//   1=METERS, 2=KILOMETERS10, 3=KILOMETERS, 4=MILES10,
//   5=MILES,  6=FEET
//
// Mazda HUD (HudDistanceUnit in reference hud.h):
//   1=METERS, 2=MILES, 3=KILOMETERS, 4=YARDS, 5=FEET
//
// The Mazda HUD doesn't distinguish the "rounded > 10" sub-units
// — it always shows one decimal and lets the magnitude speak for
// itself. So KILOMETERS10 and KILOMETERS both map to Mazda
// KILOMETERS, etc.
inline uint8_t map_distance_unit(uint32_t android_unit)
{
    switch (android_unit) {
    case 1: return 1;  // METERS       -> METERS
    case 2: return 3;  // KILOMETERS10 -> KILOMETERS
    case 3: return 3;  // KILOMETERS   -> KILOMETERS
    case 4: return 2;  // MILES10      -> MILES
    case 5: return 2;  // MILES        -> MILES
    case 6: return 5;  // FEET         -> FEET
    default: return 0; // Unknown — HUD will render nothing.
    }
}

// === Turn-icon resolution =====================================
//
// Resolve the HUD glyph from the proto turn fields. Pure function of
// its inputs (the three nav fields it reads), so it is independent of
// either transport's snapshot struct.
//   turn_event - proto TURN_EVENT (0..19 sparse)
//   turn_side  - proto TURN_SIDE  (1=L, 2=R, 3=U)
//   turn_angle - degrees (only used for roundabout exit angle)
inline uint32_t compute_turn_icon(uint32_t turn_event, uint32_t turn_side,
                                  int32_t turn_angle)
{
    if (turn_event == 13 /*TURN_ROUNDABOUT_ENTER_AND_EXIT*/) {
        // side_index: 0=left-hand traffic, 1=right-hand. Convert
        // proto TURN_SIDE (1=L, 2=R, 3=U) to that binary —
        // UNSPECIFIED falls back to right-hand, matching the
        // reference's `side - 1`.
        int32_t side_lr = (turn_side == 1) ? 0 : 1;
        return roundabout_icon(turn_angle, side_lr);
    }
    if (turn_event < 20) {
        int32_t side_idx = static_cast<int32_t>(turn_side) - 1;
        if (side_idx < 0 || side_idx > 2) side_idx = 2;
        return kTurnIcons[turn_event][side_idx];
    }
    return 0;
}

#endif // LIBPATCH_BLMJCIAAPA_HUD_NAV_H
