// SPDX-License-Identifier: AGPL-3.0-or-later
//
// OEM head-unit lane-guidance codes, and the Android Auto -> OEM code mapping.
//
// When Android Auto shows lane guidance (the row of little arrows before a
// junction), we forward it to the car by sending one numeric "lane code" per lane
// to the head unit's navigation service. That service owns the actual cluster
// artwork: it turns each code into a glyph for whichever instrument cluster is
// fitted (a basic cluster draws a coarser set of arrows, an extended one draws
// more distinct arrows). That choice is made by the head unit, downstream of us,
// so this module's only job is to pick the right code for a lane — it never deals
// in glyphs.
//
// How the 0..70 code space is laid out:
//   - Every lane shape has an "arrow" number:
//       0        empty (no lane)
//       1        straight
//       2..7     the six turn arrows (slight/normal/sharp, right and left)
//       8..19    multi-direction lanes (a lane you may take several ways)
//       20..22   HOV / bus / taxi lanes
//   - A RECOMMENDED lane (the one Android Auto highlights as yours) is sent as the
//     arrow number itself, 1..48.
//   - A NON-recommended lane (a neighbouring lane, drawn greyed) is sent as
//     arrow + 48, i.e. 49..70.
//   - An empty lane is 0.
//   - Codes 23..48 are a finer recommended form: a multi-direction lane in which
//     exactly one of its directions is the highlighted (recommended) one.
//
// This lives in its own header, separate from the version-specific Android Auto
// nav decoder, because the code space is the head unit's and does not depend on
// the Android Auto protocol version that produced the lanes.
//
// Header-only: plain enums and pure inline functions, no state and no I/O.

#ifndef LIBPATCH_BLMJCIAAPA_HUD_LANE_H
#define LIBPATCH_BLMJCIAAPA_HUD_LANE_H

#include <stdint.h>

// === Android Auto lane-direction shapes =======================================
// The shape of one arrow within an Android Auto lane (its LaneDirection.shape).
// A single lane may carry several of these at once (a multi-direction lane).
enum AaLaneShape : uint8_t {
    AA_SHAPE_UNKNOWN      = 0,
    AA_SHAPE_STRAIGHT     = 1,
    AA_SHAPE_SLIGHT_LEFT  = 2,
    AA_SHAPE_SLIGHT_RIGHT = 3,
    AA_SHAPE_NORMAL_LEFT  = 4,
    AA_SHAPE_NORMAL_RIGHT = 5,
    AA_SHAPE_SHARP_LEFT   = 6,
    AA_SHAPE_SHARP_RIGHT  = 7,
    AA_SHAPE_U_TURN_LEFT  = 8,
    AA_SHAPE_U_TURN_RIGHT = 9,
};

// === OEM lane codes (0..70) ===================================================
// The value sent to the head unit for one lane. Codes 1..22 are the recommended
// (your) lane; 23..48 are the recommended lane with one direction singled out;
// 49..70 are the non-recommended (greyed neighbour) lanes, and are simply the
// matching 1..22 shape plus 48.
enum OemLaneCode : uint8_t {
    OEM_LANE_NONE                      =  0, // empty / no lane

    // -- recommended lane, one arrow --
    OEM_LANE_STRAIGHT                  =  1, // straight
    OEM_LANE_SLIGHT_RIGHT              =  2, // slight right
    OEM_LANE_RIGHT                     =  3, // right
    OEM_LANE_SHARP_RIGHT               =  4, // sharp right
    OEM_LANE_SLIGHT_LEFT               =  5, // slight left
    OEM_LANE_LEFT                      =  6, // left
    OEM_LANE_SHARP_LEFT                =  7, // sharp left

    // -- recommended lane, multiple arrows (you may take it several ways) --
    OEM_LANE_COMBO_STRAIGHT_SLIGHT_R   =  8, // straight or slight-right
    OEM_LANE_COMBO_STRAIGHT_SLIGHT_L   =  9, // straight or slight-left
    OEM_LANE_COMBO_SLIGHT_L_S_SLIGHT_R = 10, // slight-left, straight or slight-right
    OEM_LANE_COMBO_STRAIGHT_RIGHT      = 11, // straight or right
    OEM_LANE_COMBO_STRAIGHT_LEFT       = 12, // straight or left
    OEM_LANE_COMBO_LEFT_STRAIGHT_RIGHT = 13, // left, straight or right
    OEM_LANE_COMBO_SLIGHT_R_SHARP_R    = 14, // slight-right or sharp-right
    OEM_LANE_COMBO_SLIGHT_L_SHARP_L    = 15, // slight-left or sharp-left
    OEM_LANE_COMBO_FORK                = 16, // fork: slight-left or slight-right
    OEM_LANE_COMBO_17_ALIAS_11         = 17, // same as 11 (straight or right)
    OEM_LANE_COMBO_18_ALIAS_12         = 18, // same as 12 (straight or left)
    OEM_LANE_COMBO_LEFT_RIGHT_T        = 19, // left or right (T-junction)

    // -- recommended lane, special lane types --
    OEM_LANE_HOV                       = 20, // HOV / carpool lane
    OEM_LANE_BUS                       = 21, // bus lane
    OEM_LANE_TAXI                      = 22, // taxi lane

    // -- recommended multi-direction lane, one direction singled out --
    OEM_LANE_REC_C8_STRAIGHT           = 23, // straight-or-slight-right lane, take straight
    OEM_LANE_REC_C8_SLIGHT_R           = 24, // straight-or-slight-right lane, take slight-right
    OEM_LANE_REC_C9_STRAIGHT           = 25, // straight-or-slight-left lane, take straight
    OEM_LANE_REC_C9_SLIGHT_L           = 26, // straight-or-slight-left lane, take slight-left
    OEM_LANE_REC_C10_STRAIGHT          = 27, // slight-left/straight/slight-right lane, take straight
    OEM_LANE_REC_C10_SLIGHT_L          = 28, // slight-left/straight/slight-right lane, take slight-left
    OEM_LANE_REC_C10_SLIGHT_R          = 29, // slight-left/straight/slight-right lane, take slight-right
    OEM_LANE_REC_C11_STRAIGHT          = 30, // straight-or-right lane, take straight
    OEM_LANE_REC_C11_RIGHT             = 31, // straight-or-right lane, take right
    OEM_LANE_REC_C12_STRAIGHT          = 32, // straight-or-left lane, take straight
    OEM_LANE_REC_C12_LEFT              = 33, // straight-or-left lane, take left
    OEM_LANE_REC_C13_STRAIGHT          = 34, // left/straight/right lane, take straight
    OEM_LANE_REC_C13_LEFT              = 35, // left/straight/right lane, take left
    OEM_LANE_REC_C13_RIGHT             = 36, // left/straight/right lane, take right
    OEM_LANE_REC_C14_SLIGHT_R          = 37, // slight-right-or-sharp-right lane, take slight-right
    OEM_LANE_REC_C14_SHARP_R           = 38, // slight-right-or-sharp-right lane, take sharp-right
    OEM_LANE_REC_C15_SLIGHT_L          = 39, // slight-left-or-sharp-left lane, take slight-left
    OEM_LANE_REC_C15_SHARP_L           = 40, // slight-left-or-sharp-left lane, take sharp-left
    OEM_LANE_REC_C16_SLIGHT_L          = 41, // fork lane, take slight-left
    OEM_LANE_REC_C16_SLIGHT_R          = 42, // fork lane, take slight-right
    OEM_LANE_REC_43_ALIAS_30           = 43, // same as 30
    OEM_LANE_REC_44_ALIAS_31           = 44, // same as 31
    OEM_LANE_REC_45_ALIAS_32           = 45, // same as 32
    OEM_LANE_REC_46_ALIAS_33           = 46, // same as 33
    OEM_LANE_REC_C19_LEFT              = 47, // left-or-right lane, take left
    OEM_LANE_REC_C19_RIGHT             = 48, // left-or-right lane, take right

    // -- non-recommended (greyed neighbour) lanes: the 1..22 shapes, plus 48 --
    OEM_LANE_NR_STRAIGHT               = 49, // straight
    OEM_LANE_NR_SLIGHT_RIGHT           = 50, // slight right
    OEM_LANE_NR_RIGHT                  = 51, // right
    OEM_LANE_NR_SHARP_RIGHT            = 52, // sharp right
    OEM_LANE_NR_SLIGHT_LEFT            = 53, // slight left
    OEM_LANE_NR_LEFT                   = 54, // left
    OEM_LANE_NR_SHARP_LEFT             = 55, // sharp left
    OEM_LANE_NR_COMBO_STRAIGHT_SLIGHT_R= 56, // straight or slight-right
    OEM_LANE_NR_COMBO_STRAIGHT_SLIGHT_L= 57, // straight or slight-left
    OEM_LANE_NR_COMBO_SLIGHT_L_S_SLIGHT_R=58,// slight-left, straight or slight-right
    OEM_LANE_NR_COMBO_STRAIGHT_RIGHT   = 59, // straight or right
    OEM_LANE_NR_COMBO_STRAIGHT_LEFT    = 60, // straight or left
    OEM_LANE_NR_COMBO_LEFT_STRAIGHT_RIGHT=61,// left, straight or right
    OEM_LANE_NR_COMBO_SLIGHT_R_SHARP_R = 62, // slight-right or sharp-right
    OEM_LANE_NR_COMBO_SLIGHT_L_SHARP_L = 63, // slight-left or sharp-left
    OEM_LANE_NR_COMBO_FORK             = 64, // fork: slight-left or slight-right
    OEM_LANE_NR_COMBO_17_ALIAS_11      = 65, // same as 59 (straight or right)
    OEM_LANE_NR_COMBO_18_ALIAS_12      = 66, // same as 60 (straight or left)
    OEM_LANE_NR_COMBO_LEFT_RIGHT_T     = 67, // left or right (T-junction)
    OEM_LANE_NR_HOV                    = 68, // HOV / carpool lane
    OEM_LANE_NR_BUS                    = 69, // bus lane
    OEM_LANE_NR_TAXI                   = 70, // taxi lane
};

// The "no lane in this slot" value depends on the transport that carries the
// codes: the guidance-signal path treats 0 as empty, while the direct lane-request
// path uses 0xFF. This module works in the code domain where empty is 0; the
// caller substitutes its own sentinel for empty slots if it needs a different one.

// === Android Auto lane -> OEM code ============================================

// A single turn direction, as a bit, so a multi-direction lane can be described
// as a set of directions and matched against the known combinations below.
enum {
    OD_STRAIGHT = 1u << 0,
    OD_SLIGHT_R = 1u << 1,
    OD_RIGHT    = 1u << 2,
    OD_SHARP_R  = 1u << 3,
    OD_SLIGHT_L = 1u << 4,
    OD_LEFT     = 1u << 5,
    OD_SHARP_L  = 1u << 6,
};

// Android Auto shape -> its direction bit (see OD_* above).
inline unsigned aa_shape_to_oem_dir(uint8_t shape)
{
    switch (shape) {
    case AA_SHAPE_STRAIGHT:     return OD_STRAIGHT;
    case AA_SHAPE_SLIGHT_RIGHT: return OD_SLIGHT_R;
    case AA_SHAPE_NORMAL_RIGHT: return OD_RIGHT;
    case AA_SHAPE_SHARP_RIGHT:  return OD_SHARP_R;
    case AA_SHAPE_U_TURN_RIGHT: return OD_SHARP_R;  // U-turn -> sharp right
    case AA_SHAPE_SLIGHT_LEFT:  return OD_SLIGHT_L;
    case AA_SHAPE_NORMAL_LEFT:  return OD_LEFT;
    case AA_SHAPE_SHARP_LEFT:   return OD_SHARP_L;
    case AA_SHAPE_U_TURN_LEFT:  return OD_SHARP_L;  // U-turn -> sharp left
    default:                    return OD_STRAIGHT; // unknown
    }
}

// Non-recommended (greyed neighbour) lane codes are the recommended code + this.
static constexpr uint8_t OEM_LANE_NONREC_OFFSET = 48;

// A set of direction bits -> the recommended OEM code for that lane shape.
// One direction -> a single-arrow code; a recognised combination -> a combo code;
// anything not recognised -> OEM_LANE_NONE (the caller then picks a single arrow).
inline OemLaneCode oem_base_code_for_dirs(unsigned d)
{
    switch (d) {
    case OD_STRAIGHT:                             return OEM_LANE_STRAIGHT;
    case OD_SLIGHT_R:                             return OEM_LANE_SLIGHT_RIGHT;
    case OD_RIGHT:                                return OEM_LANE_RIGHT;
    case OD_SHARP_R:                              return OEM_LANE_SHARP_RIGHT;
    case OD_SLIGHT_L:                             return OEM_LANE_SLIGHT_LEFT;
    case OD_LEFT:                                 return OEM_LANE_LEFT;
    case OD_SHARP_L:                              return OEM_LANE_SHARP_LEFT;
    case OD_STRAIGHT | OD_SLIGHT_R:               return OEM_LANE_COMBO_STRAIGHT_SLIGHT_R;
    case OD_STRAIGHT | OD_SLIGHT_L:               return OEM_LANE_COMBO_STRAIGHT_SLIGHT_L;
    case OD_SLIGHT_L | OD_STRAIGHT | OD_SLIGHT_R: return OEM_LANE_COMBO_SLIGHT_L_S_SLIGHT_R;
    case OD_STRAIGHT | OD_RIGHT:                  return OEM_LANE_COMBO_STRAIGHT_RIGHT;
    case OD_STRAIGHT | OD_LEFT:                   return OEM_LANE_COMBO_STRAIGHT_LEFT;
    case OD_LEFT | OD_STRAIGHT | OD_RIGHT:        return OEM_LANE_COMBO_LEFT_STRAIGHT_RIGHT;
    case OD_SLIGHT_R | OD_SHARP_R:                return OEM_LANE_COMBO_SLIGHT_R_SHARP_R;
    case OD_SLIGHT_L | OD_SHARP_L:                return OEM_LANE_COMBO_SLIGHT_L_SHARP_L;
    case OD_SLIGHT_L | OD_SLIGHT_R:               return OEM_LANE_COMBO_FORK;
    case OD_LEFT | OD_RIGHT:                      return OEM_LANE_COMBO_LEFT_RIGHT_T;
    default:                                      return OEM_LANE_NONE;
    }
}

// A recommended combo lane in which exactly one direction is highlighted -> the
// finer "take this direction" code. OEM_LANE_NONE if `base` is not a combo, or has
// no code for that highlighted direction.
inline OemLaneCode oem_detail_code(OemLaneCode base, unsigned hl)
{
    switch (base) {
    case OEM_LANE_COMBO_STRAIGHT_SLIGHT_R:
        return hl == OD_STRAIGHT ? OEM_LANE_REC_C8_STRAIGHT
             : hl == OD_SLIGHT_R ? OEM_LANE_REC_C8_SLIGHT_R  : OEM_LANE_NONE;
    case OEM_LANE_COMBO_STRAIGHT_SLIGHT_L:
        return hl == OD_STRAIGHT ? OEM_LANE_REC_C9_STRAIGHT
             : hl == OD_SLIGHT_L ? OEM_LANE_REC_C9_SLIGHT_L  : OEM_LANE_NONE;
    case OEM_LANE_COMBO_SLIGHT_L_S_SLIGHT_R:
        return hl == OD_STRAIGHT ? OEM_LANE_REC_C10_STRAIGHT
             : hl == OD_SLIGHT_L ? OEM_LANE_REC_C10_SLIGHT_L
             : hl == OD_SLIGHT_R ? OEM_LANE_REC_C10_SLIGHT_R : OEM_LANE_NONE;
    case OEM_LANE_COMBO_STRAIGHT_RIGHT:
        return hl == OD_STRAIGHT ? OEM_LANE_REC_C11_STRAIGHT
             : hl == OD_RIGHT    ? OEM_LANE_REC_C11_RIGHT    : OEM_LANE_NONE;
    case OEM_LANE_COMBO_STRAIGHT_LEFT:
        return hl == OD_STRAIGHT ? OEM_LANE_REC_C12_STRAIGHT
             : hl == OD_LEFT     ? OEM_LANE_REC_C12_LEFT     : OEM_LANE_NONE;
    case OEM_LANE_COMBO_LEFT_STRAIGHT_RIGHT:
        return hl == OD_STRAIGHT ? OEM_LANE_REC_C13_STRAIGHT
             : hl == OD_LEFT     ? OEM_LANE_REC_C13_LEFT
             : hl == OD_RIGHT    ? OEM_LANE_REC_C13_RIGHT    : OEM_LANE_NONE;
    case OEM_LANE_COMBO_SLIGHT_R_SHARP_R:
        return hl == OD_SLIGHT_R ? OEM_LANE_REC_C14_SLIGHT_R
             : hl == OD_SHARP_R  ? OEM_LANE_REC_C14_SHARP_R  : OEM_LANE_NONE;
    case OEM_LANE_COMBO_SLIGHT_L_SHARP_L:
        return hl == OD_SLIGHT_L ? OEM_LANE_REC_C15_SLIGHT_L
             : hl == OD_SHARP_L  ? OEM_LANE_REC_C15_SHARP_L  : OEM_LANE_NONE;
    case OEM_LANE_COMBO_FORK:
        return hl == OD_SLIGHT_L ? OEM_LANE_REC_C16_SLIGHT_L
             : hl == OD_SLIGHT_R ? OEM_LANE_REC_C16_SLIGHT_R : OEM_LANE_NONE;
    case OEM_LANE_COMBO_LEFT_RIGHT_T:
        return hl == OD_LEFT     ? OEM_LANE_REC_C19_LEFT
             : hl == OD_RIGHT    ? OEM_LANE_REC_C19_RIGHT    : OEM_LANE_NONE;
    default:
        return OEM_LANE_NONE;
    }
}

// Turn one Android Auto lane into its OEM code.
//
// Inputs are bitmasks over AaLaneShape: bit s is set if a direction with shape s
// is present / highlighted in this lane. A lane counts as recommended (yours) if
// any of its directions is highlighted. Result:
//   - no directions                                -> OEM_LANE_NONE
//   - recommended combo, one direction highlighted -> a detail code (23..48)
//   - recommended                                  -> the base code (1..22)
//   - not recommended                              -> base + 48 (49..70)
inline OemLaneCode oem_lane_code_for_aa(uint16_t present_shapes, uint16_t highlight_shapes)
{
    unsigned present = 0, hl = 0;
    for (uint8_t s = 0; s <= 9; ++s) {
        const uint16_t bit = (uint16_t)(1u << s);
        if (present_shapes   & bit) present |= aa_shape_to_oem_dir(s);
        if (highlight_shapes & bit) hl      |= aa_shape_to_oem_dir(s);
    }
    if (!present) return OEM_LANE_NONE;

    const bool  rec  = (hl != 0);
    OemLaneCode base = oem_base_code_for_dirs(present);

    // A recommended combo lane with exactly one highlighted direction gets the
    // finer "take this direction" code.
    const bool one_hl = hl && ((hl & (hl - 1)) == 0);
    if (rec && one_hl) {
        const OemLaneCode d = oem_detail_code(base, hl);
        if (d != OEM_LANE_NONE) return d;
    }

    // A direction set with no combined code: fall back to a single arrow — the
    // highlighted direction if there is one, otherwise the first present.
    if (base == OEM_LANE_NONE) {
        const unsigned pick = one_hl ? hl : (present & (0u - present));
        base = oem_base_code_for_dirs(pick);
        if (base == OEM_LANE_NONE) base = OEM_LANE_STRAIGHT;  // last resort
    }

    return rec ? base : (OemLaneCode)(base + OEM_LANE_NONREC_OFFSET);
}

// === OEM code -> HUD glyph (svcjcinavi's g_arrLaneInfoMapping) ================
//
// svcjcinavi::NAVI_SendHUDGuidaceDataToVBS turns each OEM lane code (0..70) into a
// cluster glyph via g_arrLaneInfoMapping, then ships the glyph bytes on
// VBS_NAVI_SetRecommLaneReq. Table A = basic cluster (glyphs 0..35, collapses
// detail); table B = extended cluster (glyphs 0..62). Which is live is chosen by
// CMU-control sub-id 0x18 (meter ECU / as-built).
//
// The svcnavi transport hands svcjcinavi the CODE and lets it do this. The DIRECT
// VBS transport bypasses svcjcinavi, so it must apply the same map itself before
// SetRecommLaneReq — that's what oem_lane_glyph() is for.
enum OemLaneTable { OEM_LANE_TABLE_A = 0, OEM_LANE_TABLE_B = 1 };

// code (0..70) -> cluster glyph for the chosen table; out-of-range -> 0 (empty).
// Tables are byte-exact from svcjcinavi.so .data (A @ vaddr 0xaa058, B @ 0xaa174;
// 71 entries each, all values <= 62). Function-local so a single shared copy.
inline uint8_t oem_lane_glyph(OemLaneCode code, OemLaneTable table)
{
    static const uint8_t A[71] = {
         0, 1, 2, 2, 3, 4, 4, 5, 1, 1, 1, 1,
         1, 1, 1, 1, 1, 1, 1, 1,19,20,21, 6,
         7, 8, 9,10,11,12, 6, 7, 8, 9,10,15,
        13,13,14,15,16,17,18, 6, 7, 8, 9, 9,
         7,22,23,23,24,25,25,26,27,28,29,27,
        28,29,30,31,32,30,31,32,33,34,35
    };
    static const uint8_t B[71] = {
         0, 1, 2,46, 3, 4,47, 5,40,43,36,41,
        44,37,42,45,38,41,44,39,19,20,21, 6,
         7, 8, 9,10,11,12,48,49,51,52,54,56,
        55,13,14,15,16,17,18,48,49,51,52,53,
        50,22,23,57,24,25,58,26,27,28,29,59,
        60,61,30,31,32,59,60,62,33,34,35
    };
    const unsigned c = (unsigned)code;
    if (c > 70) return 0;
    return table == OEM_LANE_TABLE_B ? B[c] : A[c];
}

#endif // LIBPATCH_BLMJCIAAPA_HUD_LANE_H
