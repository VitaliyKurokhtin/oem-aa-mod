// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Wireless GAL-1.6 Bluetooth-pairing bypass.
//
// Over an AAWireless dongle at GAL protocol >= 1.6 the phone
// DELIBERATELY skips the in-band BluetoothPairingRequest — gearhead's
// ptf.java literally logs "Bypassing the initial pairing request on
// purpose" when the connection is wireless AND the head-unit protocol
// minor >= 6. A real wireless 1.6 head unit pairs Bluetooth earlier, in
// the WPP (WiFi Projection Protocol / RFCOMM) bootstrap, so the in-band
// step is redundant there. Mazda's blmjciaapa has no such bootstrap: it
// still hard-gates session activation on a successful BT-pairing result
// (AapConnectionManager::ActivateAapSession is only reached from
// NotifyBtPairingResult(success) while connect mode == 2). So over the
// dongle the AAP session connects (SessionStatusCb CONNECTED,
// ProjectionStatusCb AAP_PROJECTION_SETUP) but never ACTIVATES: no
// "AA connected" SBN notification, no video focus, and the dongle
// detaches after ~5 s.
//
// This module closes that gap. When GAL 1.6 is enabled AND the connected
// USB device name matches a known dongle (or the all-devices config override
// is enabled), it synthesizes the missing activation by calling the OEM's
// AapConnectionManager::ActivateAapSession(cm)
// directly, once the session reaches the pending-pairing state
// (connect mode == 2, read live from AapConnectionManager+0xdc). That is
// the exact activation a real BT-pairing success would perform
// (StartResourcesControl, ResumeLastMode, NotifyBTConnectionComplete,
// mode:=3), so it activates the session with no double-activation risk.
// The trigger is a transparent wrapper around cb_list slot 3
// (ProjectionStatusCb): it always chains to the OEM callback, and only
// arms the bypass on the AAP_PROJECTION_SETUP event for the
// wireless-1.6 case. Confirmed working on-device (FW 74.00.324A NA).
//
// Wired connections and non-1.6 configs are unaffected: the wrapper
// passes through and the device-name / GAL 1.6 gates simply don't match.

#ifndef LIBPATCH_BLMJCIAAPA_BT16PAIR_BT16PAIR_H
#define LIBPATCH_BLMJCIAAPA_BT16PAIR_BT16PAIR_H

// Install the ProjectionStatusCb (cb_list slot 3) wrapper. Call from the
// aap_create_session shim, alongside hud_pre_aap_create_session, BEFORE
// chaining to the real SDK entry point (the SDK memcpy's the table into
// the session handle inside aap_create_session). Safe with cb_list==NULL
// (no-op) and idempotent across aap_create_session retries.
void bt16pair_pre_aap_create_session(void *cb_list);

// Disarm the deferred activator. Call from the aap_destroy_session shim
// BEFORE chaining to the real SDK teardown. Note the SDK session lives for
// the whole jciAAPA process, so this covers service shutdown only; device
// disconnects are detected by the activator itself. Idempotent.
void bt16pair_pre_aap_destroy_session(void);

#endif // LIBPATCH_BLMJCIAAPA_BT16PAIR_BT16PAIR_H
