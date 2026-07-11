// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Producer-side interface for the svcjcinavi HUD transport.
//
// This is the active HUD output transport: hud.cpp calls these
// svcnavi_tx_* functions directly. (A direct com.jci.vbs.navi transport
// also exists in vbs_tx.{h,cpp}.)
//
// === What this transport does ================================
//
// Instead of marshalling the OEM com.jci.vbs.navi HUD frame
// ourselves, it hands our guidance to the OEM navi service
// (svcjcinavi) by emitting the very D-Bus signal that service
// already subscribes to for HUD updates:
//
//   bus:    service bus ($JCI_SERVICE_BUS)
//   type:   signal              (broadcast; svcjcinavi's match has no sender=)
//   path:   /com/NNG/Api/Server
//   iface:  com.NNG.Api.Server.Guidance
//   member: GuidanceChangedForHUD
//   sig:    i i i s i i i i i i i i i i      (14 args)
//           dirIcon, maneuverDist, distUnit, streetName,
//           speedLimit, speedUnit, lane0..lane7
//
// svcjcinavi's NNGAPISERVERGUIDANCE_GuidanceChangedForHUD_Handler
// receives it, runs it through NAVI_SendHUDGuidaceDataToVBS, and
// forwards it to the HUD ECU — giving us its sync-bit rotation and
// street-strip pairing for free, and (crucially) making svcjcinavi
// the single writer of the HUD frame so our updates no longer race
// the OEM nav engine's.
//
// The speedLimit argument is set to a reserved sentinel
// (kAapSpeedSentinel) so an in-process svcjcinavi-side splice hook
// (a separate, future component) can tell our frames apart from the
// OEM nav engine's and merge the two streams. A stock, un-hooked
// svcjcinavi will simply render the sentinel as a speed value, which
// is exactly the behaviour the round-trip bring-up test relies on.
//
// This transport only does anything useful when svcjcinavi is
// running, which on this platform requires the navigation SD card to
// be present. With no card, svcjcinavi is absent, the signal goes
// nowhere, and nothing is shown — so a build that selects this
// transport is for card-equipped vehicles. The direct transport
// (vbs_tx) remains the cardless path.

#ifndef LIBPATCH_BLMJCIAAPA_HUD_SVCNAVI_TX_H
#define LIBPATCH_BLMJCIAAPA_HUD_SVCNAVI_TX_H

#include <stdint.h>
#include "hud_nav16.h"   // AaLane, aa_nav16_lane_codes

// HUD output lifecycle. svcnavi_tx_start opens the OEM service-bus
// connection (libjcidbus, exit-on-disconnect disabled) on a
// dedicated sender thread and starts forwarding per-event updates
// from hud.cpp as GuidanceChangedForHUD signals. svcnavi_tx_stop
// tears the thread down and releases the connection. Both idempotent.
void svcnavi_tx_start(void);
void svcnavi_tx_stop(void);

// 0x500 NAVMessagesStatus. `status` per hu.proto: 1=START, 2=STOP.
// Anything else is treated as STOP (defensive). On STOP we emit a
// blanking signal so svcjcinavi clears the HUD.
void svcnavi_tx_status(uint32_t status);

// 0x501 NAVTurnMessage. Identical contract to vbs_tx_next_turn:
//   turn_side  - 1=LEFT, 2=RIGHT, 3=UNSPECIFIED (proto TURN_SIDE)
//   turn_event - sparse 0..19  (proto TURN_EVENT)
//   turn_angle - degrees, signed
//   turn_number - maneuver / exit number
// road_name is NOT held past return (snapshot-copied inline).
void svcnavi_tx_next_turn(const char *road_name,
                          uint32_t    turn_side,
                          uint32_t    turn_event,
                          int32_t     turn_angle,
                          int32_t     turn_number);

// 0x502 NAVDistanceMessage. Identical contract to vbs_tx_distance:
//   display_distance - "raw unit * 1000" int32 from the SDK header;
//                      converted to the HUD's "raw unit * 10" here.
//   display_distance_unit - proto DISPLAY_DISTANCE_UNIT, 1..6.
void svcnavi_tx_distance(int32_t  distance_meters,
                         int32_t  time_until_seconds,
                         int32_t  display_distance,
                         uint32_t display_distance_unit);

// Android Auto GAL 1.6 path (fed from the aap_service shim via blmjciaapa's
// nav16 receiver). The maneuver glyph is ALREADY the Mazda HUD code, distance
// is already value*10 in the Mazda unit, and lanes are carried through to
// lane0..7 of GuidanceChangedForHUD. One call sets the whole frame.
//   glyph     - Mazda HUD maneuver code (MazdaIcon; 0=blank)
//   road      - street name (already latin-folded by the caller)
//   dist_dec  - display distance * 10
//   dist_unit - Mazda HUD unit (1=m,2=mi,3=km,4=yd,5=ft; 0=none)
//   lanes/n   - up to 8 decoded lanes (may be null / 0)
void svcnavi_tx_v16(uint32_t glyph, const char *road,
                    int32_t dist_dec, uint8_t dist_unit,
                    const AaLane *lanes, uint8_t n_lanes);

#endif // LIBPATCH_BLMJCIAAPA_HUD_SVCNAVI_TX_H
