// Self-resolving thunks for the OEM functions inside blmjciaapa.so.
// See blmjciaapa.h for the rationale and the anchor-and-offset model.
//
// Each public function is a thin trampoline that, on first call,
// computes its target address (load bias of blmjciaapa.so + the
// target's file offset), caches it, and forwards. The load bias comes
// from the one dynamically-exported anchor symbol, GetServiceInterfaces,
// via dlsym. No init step, no shared global state, no ordering rule.
// An unresolved target (blmjciaapa.so not mapped — i.e. not the
// {L_jciAAPA} PID) degrades to a benign failure value.

#define LOG_TAG "BLMJCIAAPA"
#include "../log.h"

#include "blmjciaapa.h"

#include <dlfcn.h>

namespace {

// File offsets within blmjciaapa.so (FW 74.00.324A NA), harvested
// with the cross-toolchain nm. Re-harvest after any OEM update.
namespace off {
//   GetServiceInterfaces (the only dynamically-exported symbol; anchor)
constexpr uintptr_t GetServiceInterfaces          = 0x00061768;
//   _ZN9SingletonI7AapProcE11GetInstanceEv
constexpr uintptr_t Singleton_AapProc_GetInstance = 0x00062670;
//   _ZN7AapProc15GetVideoManagerEv
constexpr uintptr_t AapProc_GetVideoManager       = 0x0007f664;
//   _ZN7AapProc10GetRaceAapEv
constexpr uintptr_t AapProc_GetRaceAap            = 0x0007f614;
//   _ZN7AapProc15GetAudioManagerEv
constexpr uintptr_t AapProc_GetAudioManager       = 0x0007f63c;
//   _ZN7AapProc22GetAapConnectionManagerEv
constexpr uintptr_t AapProc_GetAapConnectionManager = 0x000872fc;
//   _ZN12VideoManager16IsAAVideoInFocusEv
constexpr uintptr_t VideoManager_IsAAVideoInFocus = 0x000b3af4;
//   _ZN12AudioManager16IsAAMediaInFocusEv
constexpr uintptr_t AudioManager_IsAAMediaInFocus = 0x000ac3b0;
//   _ZN12AudioManager18IsAAMediaInPlayingEv
constexpr uintptr_t AudioManager_IsAAMediaInPlaying = 0x000ac3f8;
//   _ZN7RaceAap14SendTouchInputEP14AAP_TouchEvent
constexpr uintptr_t RaceAap_SendTouchInput        = 0x0008e3a8;
//   _ZN7RaceAap12SendKeyInputEP12AAP_KeyEvent
constexpr uintptr_t RaceAap_SendKeyInput          = 0x0008e534;
//   _ZN12AudioManager21SendReplyRequestFocusE14AAP_StreamType17AAP_StreamRequest
constexpr uintptr_t AudioManager_SendReplyRequestFocus = 0x000a6f40;
//   _ZN20AapConnectionManager21NotifyBtPairingResultE12true_false_t
constexpr uintptr_t AapConnectionManager_NotifyBtPairingResult = 0x0007c820;
//   _ZN20AapConnectionManager18ActivateAapSessionEv
constexpr uintptr_t AapConnectionManager_ActivateAapSession    = 0x00078db4;

// AapConnectionManager instance layout (DATA field offsets, not
// function offsets — no OEM getter exists for these). Both offsets are
// EMPIRICAL — the Ghidra decompile's this-relative offsets do NOT map
// uniformly (Ghidra splits the class into base subobjects, so different
// method groups render `this` at different bases). These were pinned by
// dumping the whole 0x274-byte object out of /proc/<jciAAPA>/mem in
// three states and diffing (2026-07-06):
//
//   DevName (+0x54): "" idle / "Pixel 9 Pro XL" wired / "AAWireless" dongle.
//     The USB-layer device name from StoreDeviceInfo; distinguishes the
//     AAWireless dongle from a wired phone. (Ghidra "this+0x4c".)
//   ConnectMode (+0xdc): 0 idle / 2 pending-BT-pair / 3 activated.
//     0 idle -> 2 the moment the AOA session connects (phone parks here
//     over the dongle at GAL 1.6, waiting for the BT pairing that never
//     comes) -> 3 once ActivateAapSession runs. This is the field the
//     decompile calls "this+0x254"; it is NOT at +0x25c at runtime (that
//     tail of the object is dead space). Re-verify after any OEM update.
constexpr uintptr_t AapConnectionManager_DevName     = 0x54;   // char[]  USB device name
constexpr uintptr_t AapConnectionManager_ConnectMode = 0xdc;   // int     0=idle 2=pending-pair 3=activated
} // namespace off

// Load bias of the already-mapped blmjciaapa.so: address of the
// exported anchor minus its file offset. 0 until resolved (or if
// blmjciaapa.so isn't mapped). Retries until it succeeds — the BLM's
// AapProc singleton may not be up on the earliest calls, but the
// library itself is mapped for the whole {L_jciAAPA} PID lifetime, so
// once non-zero the bias never changes.
uintptr_t blm_base()
{
	static uintptr_t base = 0;
	if (base == 0) {
		void *handle = dlopen("/jci/aapa/blmjciaapa.so",
		                      RTLD_NOW | RTLD_NOLOAD);
		void *sym = handle ? dlsym(handle, "GetServiceInterfaces")
		                   : nullptr;
		if (sym) {
			base = reinterpret_cast<uintptr_t>(sym)
			       - off::GetServiceInterfaces;
		} else {
			// Log once — repeated misses (e.g. we're not the
			// {L_jciAAPA} PID, so blmjciaapa.so is never mapped)
			// would otherwise spam the per-frame touch path.
			static bool logged = false;
			if (!logged) {
				logged = true;
				LOGC("oem: blmjciaapa.so anchor GetServiceInterfaces "
				     "unresolved (not the L_jciAAPA PID?)");
			}
		}
	}
	return base;
}

// Resolve one offset-based target to an absolute address, or nullptr
// if the load bias isn't available yet.
void *blm_addr(uintptr_t target_offset)
{
	uintptr_t base = blm_base();
	return base ? reinterpret_cast<void *>(base + target_offset) : nullptr;
}

} // namespace

// Define a self-resolving thunk that forwards to the OEM target at
// off::<name>. `params` / `args` are parenthesised lists.
#define BLM_THUNK(ret, name, params, args, failval) \
	ret name params \
	{ \
		using Fn = ret (*) params; \
		static Fn fn = nullptr; \
		if (!fn) fn = reinterpret_cast<Fn>(blm_addr(off::name)); \
		return fn ? fn args : (failval); \
	}

// Void-returning variant of BLM_THUNK (the value-returning macro can't
// express a void forward). Same self-resolving-and-caching model.
#define BLM_THUNK_VOID(name, params, args) \
	void name params \
	{ \
		using Fn = void (*) params; \
		static Fn fn = nullptr; \
		if (!fn) fn = reinterpret_cast<Fn>(blm_addr(off::name)); \
		if (fn) fn args; \
	}

// Read a scalar field at a fixed byte offset within an OEM instance the
// caller already holds. Pure pointer arithmetic — unlike BLM_THUNK it
// needs no load-bias resolution, since the instance pointer is supplied.
// A null instance degrades to `failval`.
#define BLM_FIELD(ret, name, self_off, failval) \
	ret name(void *self) \
	{ \
		return self \
		    ? *reinterpret_cast<ret *>( \
		          reinterpret_cast<char *>(self) + (self_off)) \
		    : (failval); \
	}

// Variant returning a pointer INTO the instance, for an inline buffer /
// char array that is addressed rather than dereferenced. Null -> nullptr.
#define BLM_FIELD_PTR(elem, name, self_off) \
	const elem *name(void *self) \
	{ \
		return self \
		    ? reinterpret_cast<const elem *>( \
		          reinterpret_cast<char *>(self) + (self_off)) \
		    : nullptr; \
	}

BLM_THUNK(void *, Singleton_AapProc_GetInstance, (void), (), nullptr)
BLM_THUNK(void *, AapProc_GetVideoManager, (void *self), (self), nullptr)
BLM_THUNK(void *, AapProc_GetRaceAap, (void *self), (self), nullptr)
BLM_THUNK(void *, AapProc_GetAudioManager, (void *self), (self), nullptr)
BLM_THUNK(void *, AapProc_GetAapConnectionManager, (void *self), (self), nullptr)
BLM_THUNK(int,    VideoManager_IsAAVideoInFocus, (void *self), (self), 0)
BLM_THUNK(int,    AudioManager_IsAAMediaInFocus, (void *self), (self), 0)
BLM_THUNK(int,    AudioManager_IsAAMediaInPlaying, (void *self), (self), 0)
BLM_THUNK(int,    RaceAap_SendTouchInput,
          (void *self, AAP_TouchEvent *evt), (self, evt), -1)
BLM_THUNK(int,    RaceAap_SendKeyInput,
          (void *self, AAP_KeyEvent *evt), (self, evt), -1)
BLM_THUNK_VOID(AapConnectionManager_NotifyBtPairingResult,
               (void *self, int result), (self, result))
BLM_THUNK_VOID(AapConnectionManager_ActivateAapSession,
               (void *self), (self))

BLM_FIELD_PTR(char, AapConnectionManager_dev_name,
              off::AapConnectionManager_DevName)
BLM_FIELD(int, AapConnectionManager_connect_mode,
          off::AapConnectionManager_ConnectMode, -1)

// Address (not a forward) of AudioManager::SendReplyRequestFocus, for the
// audio head pre-open's entry detour. Same anchor-and-offset resolution as
// the thunks; nullptr until blmjciaapa.so is mapped.
void *AudioManager_SendReplyRequestFocus_addr(void)
{
	return blm_addr(off::AudioManager_SendReplyRequestFocus);
}

		  
// AapConnectionManager instance accessor. GetInstance() may return
// nullptr before the AapProc singleton is up (or when we're in the
// wrong PID); callers must tolerate the nullptr degradation. Forwards
// through the OEM AapProc::GetAapConnectionManager getter (thunk above).
void *AapConnectionManager_instance(void)
{
	void *aap = Singleton_AapProc_GetInstance();
	return aap ? AapProc_GetAapConnectionManager(aap) : nullptr;
}