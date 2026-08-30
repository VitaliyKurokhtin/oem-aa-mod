// OEM symbols inside blmjciaapa.so (anchor-and-offset bindings).
//
// This header reads like a normal vendor header: just the touch
// wire-format types and prototypes of the OEM functions/methods we
// call. There is NO init step and NO resolver to invoke — each
// prototype is backed by a self-resolving thunk in blmjciaapa.cpp.
//
// The shipped blmjciaapa.so (FW 74.00.324A NA) exports exactly one
// symbol in its dynamic symbol table: GetServiceInterfaces. Every
// function below is LOCAL in the static symbol table and not
// reachable via dlsym, so each thunk resolves its target by:
//
//   base = (uintptr_t)dlsym(blmjciaapa.so, "GetServiceInterfaces")
//          - <anchor file offset>
//   addr = base + <target file offset>
//
// on first call, caches it, and forwards. If blmjciaapa.so isn't
// mapped (we're not in the {L_jciAAPA} PID) the thunk returns a
// benign failure value (nullptr / 0 / -1). All offsets were harvested
// with the cross-toolchain nm against the shipped
// /jci/aapa/blmjciaapa.so; re-run the harvest after any OEM firmware
// update before trusting them.

#ifndef LIBPATCH_BLMJCIAAPA_BLMJCIAAPA_H
#define LIBPATCH_BLMJCIAAPA_BLMJCIAAPA_H

#include <stdint.h>

// Maximum multi-touch fingers, fixed by AAP_TouchEvent's array sizes
// and the AAP service's `< 10` validation on the receiving end.
constexpr int kMaxFingers = 10;

// Wire-format AAP_TouchEvent (0x88 / 136 bytes). Decoded by tracing
// AAPInputSource::onTouchEvent and InputSource::populateTouchEvent.
struct AAP_TouchEvent {
    uint32_t device_type;            // +0x00, 1 = touchscreen
    uint32_t finger_count;           // +0x04, 1..10
    uint32_t x[kMaxFingers];         // +0x08, display pixels
    uint32_t y[kMaxFingers];         // +0x30, display pixels
    uint32_t pointer_id[kMaxFingers];// +0x58, evdev tracking IDs
    uint32_t action;                 // +0x80, 1..5
    uint32_t action_index;           // +0x84, compacted slot index
};
static_assert(sizeof(AAP_TouchEvent) == 0x88, "AAP_TouchEvent must be 0x88 bytes");

// Wire-format `action` values, mapped to Android MotionEvent
// semantics by the AAP service.
enum AAPTouchAction : uint32_t {
    kAAPTouchActionDown        = 1,  // first finger landed (n: 0 -> 1)
    kAAPTouchActionUp          = 2,  // last finger lifted (n: 1 -> 0)
    kAAPTouchActionMove        = 3,  // any finger moved
    kAAPTouchActionPointerDown = 4,  // additional finger landed (n: k -> k+1, k>=1)
    kAAPTouchActionPointerUp   = 5,  // one of several lifted (n: k -> k-1, k>=2)
};

// Wire-format AAP_KeyEvent (12 bytes) for RaceAap::SendKeyInput. Mirrors the
// three consecutive stack words HMIEventHandler::InputKey builds before every
// SendKeyInput call: an event type (the OEM always uses 0xe00 for key input),
// an Android keycode, and a down/up flag. A key "press" is two calls — bDown
// 1 (press) then bDown 0 (release) — exactly as the OEM does.
struct AAP_KeyEvent {
    uint32_t eType;      // +0x00  event type; OEM key input uses kAAPKeyEventType
    uint32_t eKeyCode;   // +0x04  Android KEYCODE_* value
    uint32_t bDown;      // +0x08  1 = key down (press), 0 = key up (release)
};
static_assert(sizeof(AAP_KeyEvent) == 0xc, "AAP_KeyEvent must be 12 bytes");

// eType the OEM stamps on every key event it feeds to SendKeyInput.
constexpr uint32_t kAAPKeyEventType = 0xe00;

// Android media keycodes we inject. KEYCODE_MEDIA_PLAY is the very code the
// OEM's own InputKey case 0xe emits; KEYCODE_MEDIA_PAUSE has no OEM AA path
// (the projection key map has no pause), which the play/pause bridge fills.
constexpr uint32_t kAAPKeyMediaPlay  = 0x7e;  // 126 KEYCODE_MEDIA_PLAY
constexpr uint32_t kAAPKeyMediaPause = 0x7f;  // 127 KEYCODE_MEDIA_PAUSE

// OEM functions/methods inside blmjciaapa.so. The OEM declares the
// methods as non-static C++ members; ARM EABI passes the implicit
// `this` in r0 like a normal first argument, so they are modelled as
// free functions taking `void *self`.
//   Singleton<AapProc>::GetInstance()      -> AapProc *
//   AapProc::GetVideoManager()             -> VideoManager *
//   AapProc::GetRaceAap()                  -> RaceAap *
//   AapProc::GetAudioManager()             -> AudioManager *
//   VideoManager::IsAAVideoInFocus()       -> int (bool)
//   AudioManager::IsAAMediaInFocus()       -> int (bool)  AA media is the active/focused source
//   AudioManager::IsAAMediaInPlaying()     -> int (bool)  AA media is actively PLAYING (not paused)
//   RaceAap::SendTouchInput(AAP_TouchEvent*) -> int (0 = ok)
//   RaceAap::SendKeyInput(AAP_KeyEvent*)     -> int (0 = ok)
void *Singleton_AapProc_GetInstance(void);
void *AapProc_GetVideoManager(void *self);
void *AapProc_GetRaceAap(void *self);
void *AapProc_GetAudioManager(void *self);
int   VideoManager_IsAAVideoInFocus(void *self);
int   AudioManager_IsAAMediaInFocus(void *self);
int   AudioManager_IsAAMediaInPlaying(void *self);
int   RaceAap_SendTouchInput(void *self, AAP_TouchEvent *evt);
int   RaceAap_SendKeyInput(void *self, AAP_KeyEvent *evt);

// Runtime address of AudioManager::SendReplyRequestFocus inside the mapped
// blmjciaapa.so — the OEM method that grants the guidance audio-focus reply.
// Unlike the thunks above this returns the ADDRESS (the caller installs an
// entry detour on it), not a callable forward. nullptr if blmjciaapa.so isn't
// mapped (wrong PID) or the anchor isn't resolvable yet. Used by the audio
// head pre-open (audio_preopen.cpp).
void *AudioManager_SendReplyRequestFocus_addr(void);

// AapConnectionManager access, used by the wireless GAL-1.6 BT-pairing
// bypass (bt16pair/). The AapConnectionManager is a subobject of the
// AapProc singleton at AapProc+0x98 (= AapProc::GetAapConnectionManager,
// which just returns this+0x98). These helpers resolve/read it; all
// degrade to nullptr / -1 when blmjciaapa.so isn't mapped or the
// singleton isn't up yet.
//   AapConnectionManager_instance()          -> AapConnectionManager* / NULL
//   AapConnectionManager_dev_name(cm)        -> USB device name (cm+0x54):
//                                               "AAWireless" for the dongle,
//                                               the phone model when wired.
//   AapConnectionManager_connect_mode(cm)    -> connect mode (cm+0xdc):
//                                               0 = idle, 2 = pending BT pair
//                                               (AOA session up, dongle parks
//                                               here at GAL 1.6), 3 = activated.
//                                               Offset EMPIRICALLY pinned by a
//                                               3-state /proc/mem object diff
//                                               (idle/wired/dongle) — NOT the
//                                               decompile's "0x254".
//   AapConnectionManager_NotifyBtPairingResult(cm, result)
//       -> OEM guarded pairing-result entry; result==1 while mode==2
//          triggers ActivateAapSession. Its guard reads the real mode field
//          (cm+0xdc), so it works when mode==2 is observed.
//   AapConnectionManager_ActivateAapSession(cm)
//       -> the OEM "AA connected" activation itself (StartResourcesControl,
//          ResumeLastMode, NotifyBTConnectionComplete, mode:=3). Takes NO
//          mode precondition, so it can be invoked directly to force
//          activation when the in-band BT pairing is skipped (dongle GAL 1.6).
void       *AapConnectionManager_instance(void);
const char *AapConnectionManager_dev_name(void *cm);
int         AapConnectionManager_connect_mode(void *cm);
void        AapConnectionManager_NotifyBtPairingResult(void *cm, int result);
void        AapConnectionManager_ActivateAapSession(void *cm);

#endif  // LIBPATCH_BLMJCIAAPA_BLMJCIAAPA_H
