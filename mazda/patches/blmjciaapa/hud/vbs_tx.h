// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Adapted from headunit (https://github.com/Trevelopment/headunit),
// licensed under GNU AGPL v3.
// See NOTICE.md at the repo root for the full attribution.
//
// Producer-side interface for the VBS (com.jci.vbs.navi) HUD sender.
// HUD output module — push interface used by hud.cpp.
//
// Three small non-blocking functions the AAP nav callback in
// `hud.cpp` calls once per phone-side event. Each updates a
// seqlock-protected snapshot inside `vbs_tx.cpp` and wakes the
// dedicated sender thread that owns the OEM D-Bus connections.
//
// This transport writes the HUD frame DIRECTLY to com.jci.vbs.navi
// (the libjcimod_navigation.so handler inside the jciVBS process),
// so it works with no navigation SD card. The alternative svcnavi
// transport (svcnavi_send.{h,cpp}) routes through svcjcinavi instead.
//
// Lifecycle (start/stop) is declared here as the sender-facing
// half of the HUD subsystem; hud_pre_aap_create_session lives in hud.h.
//
// Implementation reference: `mazda/hud/{hud.h,hud.cpp}` in the
// upstream headunit project (see the SPDX header at the top of
// vbs_tx.cpp for the URL). We mirror the proxy classes and the
// turn-icon lookup table from that implementation but adapted to
// live inside an LD_PRELOAD library rather than the headunit's
// own daemon.

#ifndef LIBPATCH_BLMJCIAAPA_VBS_TX_H
#define LIBPATCH_BLMJCIAAPA_VBS_TX_H

#include <stdint.h>
#include "hud_nav16.h"   // AaLane (vbs_tx_v16 lane-strip param)

// Defined in vbs_tx.cpp. HUD output lifecycle. vbs_tx_start opens
// the OEM HMI + Service D-Bus connections, spawns a dispatcher
// thread and a sender thread, and starts forwarding per-event
// updates from hud.cpp. vbs_tx_stop tears the threads down and
// releases the D-Bus connections. Both are idempotent.
void vbs_tx_start(void);
void vbs_tx_stop(void);

// 0x500 NAVMessagesStatus. `status` per hu.proto: 1=START, 2=STOP.
// Anything else is treated as STOP (defensive).
void vbs_tx_status(uint32_t status);

// 0x501 NAVTurnMessage. `road_name` may be NULL or empty.
//   turn_side  - 1=LEFT, 2=RIGHT, 3=UNSPECIFIED (proto TURN_SIDE)
//   turn_event - sparse 0..19  (proto TURN_EVENT)
//   turn_angle - degrees, signed
//   turn_number - maneuver / exit number
//
// road_name lifetime: NOT held past return. The pointer is
// snapshot-copied into a fixed-size buffer inline.
void vbs_tx_next_turn(const char *road_name,
                      uint32_t    turn_side,
                      uint32_t    turn_event,
                      int32_t     turn_angle,
                      int32_t     turn_number);

// 0x502 NAVDistanceMessage.
//   distance_meters  - raw meters (proto `distance`)
//   time_until_seconds - ETA seconds (proto `time_until`)
//   display_distance - the "raw unit * 1000" int32 from the SDK
//                      header (truncated from proto uint64). The
//                      HUD wire format wants "raw unit * 10", so
//                      we divide by 100 on the way in.
//   display_distance_unit - proto DISPLAY_DISTANCE_UNIT, 1..6.
//                           Mapped to the HUD's own unit enum
//                           inside vbs_tx.cpp.
void vbs_tx_distance(int32_t  distance_meters,
                     int32_t  time_until_seconds,
                     int32_t  display_distance,
                     uint32_t display_distance_unit);

// Android Auto GAL 1.6 path. The maneuver glyph is ALREADY the Mazda HUD code
// and distance is already value*10 in the Mazda unit. Lanes are carried through
// so the direct path is lane-ready; see vbs_tx.cpp for the emit-side OEM-ABI
// note (the basic VBS_NAVI_SetHUDDisplayMsgReq frame has no lane field). One
// call sets the frame.
void vbs_tx_v16(uint32_t glyph, const char *road,
                int32_t dist_dec, uint8_t dist_unit,
                const AaLane *lanes, uint8_t n_lanes);

#endif // LIBPATCH_BLMJCIAAPA_VBS_TX_H
