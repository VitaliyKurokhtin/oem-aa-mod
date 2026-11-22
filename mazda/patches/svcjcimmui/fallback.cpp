// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Redirect CMU audio-source fallback away from FM/AM/SiriusXM.
//
// Stock libjciuiasystem.so (FW 74.00.324A) calls the local function
// activatePrevious() when CarPlay / Android Auto / USB disconnects or
// when the reconnect timer expires. That helper prefers the previous
// source, then lastBroadcast — which is almost always FM. Selecting FM
// with no station yields the harsh static users hear at the next ACC ON.
//
// activatePrevious is LOCAL (not a PLT import), so we install an 8-byte
// ARM absolute trampoline at its known file offset once
// libjciuiasystem.so is mapped. When the candidate source is a
// broadcast radio app (stock UIA_SYSTEM_AudioStack_isBroadcastSource),
// we inject the stock SelectEntertainmentNoAudio event instead — the
// same path the Entertainment → Audio Off menu uses — so last-mode
// becomes NONE and CarPlay/AA media can still reclaim audio later via
// PrePlaying / audio-focus.
//
// Offsets are for cmu150_NA_74.00.324A (vaddr-0 PIE). The prologue check
// aborts the hook if the binary does not match.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define LOG_TAG "FALLBACK"
#include "log.h"
#include "fallback.h"
#include "common/config.h"
#include "common/thread_util.h"

#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

// --- FW 74.00.324A libjciuiasystem.so layout ----------------------------
constexpr const char *kUiaSystemSo = "/jci/mmui/uia/libjciuiasystem.so";
constexpr const char *kAnchorSym   = "UiApp_InjectEvent";
constexpr uintptr_t   kOffAnchor   = 0x0005c220; // UiApp_InjectEvent (exported)

constexpr uintptr_t kOffActivatePrevious   = 0x0003b37c;
constexpr uintptr_t kOffGetPrevious        = 0x00039e3c;
constexpr uintptr_t kOffGetLastBroadcast   = 0x00039e50;
constexpr uintptr_t kOffIsAvailable        = 0x00038290;
constexpr uintptr_t kOffIsBroadcastSource  = 0x00039d1c;

// Expected first two instructions of activatePrevious (push + sub sp).
constexpr uint32_t kPrologue0 = 0xe92d4070; // push {r4,r5,r6,lr}
constexpr uint32_t kPrologue1 = 0xe24dd018; // sub  sp, sp, #24

// Absolute jump: ldr pc, [pc, #-4] ; .word target
constexpr uint32_t kLdrPcM4   = 0xe51ff004;

using ActivatePreviousFn = int (*)(int);
using GetAppFn           = void *(*)();
using IsAvailableFn      = int (*)(void *);
using IsBroadcastFn      = int (*)(void *);
using InjectToNameFn     = void (*)(const char *, const char *, void *, int);

uintptr_t          g_base            = 0;
ActivatePreviousFn g_real_activate   = nullptr;
GetAppFn           g_get_previous    = nullptr;
GetAppFn           g_get_last_bcast  = nullptr;
IsAvailableFn      g_is_available    = nullptr;
IsBroadcastFn      g_is_broadcast    = nullptr;
InjectToNameFn     g_inject_to_name  = nullptr;

uint32_t           g_saved_prologue[2] = {0, 0};
uint32_t          *g_trampoline        = nullptr; // RWX page, 4 ARM words
bool               g_installed         = false;

bool set_prot(uintptr_t addr, size_t len, int prot)
{
    const uintptr_t ps   = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t page = addr & ~(ps - 1);
    const uintptr_t end  = addr + len;
    return mprotect(reinterpret_cast<void *>(page), end - page, prot) == 0;
}

void *at(uintptr_t off)
{
    return reinterpret_cast<void *>(g_base + off);
}

int select_audio_off()
{
    if (!g_inject_to_name) {
        LOGE("fallback: UiApp_InjectEventToUiaName unresolved");
        return 0;
    }
    // Stock Entertainment → Audio Off path. Null payload matches the
    // empty `{}` the GUI sends; the event handler tolerates a NULL JSON
    // object and still delivers SelectEntertainmentNoAudio.
    LOGD("fallback: injecting SelectEntertainmentNoAudio (Audio Off)");
    g_inject_to_name("system", "SelectEntertainmentNoAudio", nullptr, 0);
    return 1;
}

// Our activatePrevious replacement. Signature matches stock (one int
// flag; callers pass 0 or 1).
int hook_activatePrevious(int flag)
{
    if (!libpatch_config::fallback_audio_off() || !g_real_activate) {
        return g_real_activate ? g_real_activate(flag) : 0;
    }

    void *cand = nullptr;
    if (g_get_previous && g_is_available) {
        void *prev = g_get_previous();
        if (prev && g_is_available(prev) == 1) {
            cand = prev;
        }
    }
    if (!cand && g_get_last_bcast && g_is_available) {
        void *lb = g_get_last_bcast();
        if (lb && g_is_available(lb) == 1) {
            cand = lb;
        }
    }

    if (cand && g_is_broadcast && g_is_broadcast(cand) == 1) {
        LOGD("fallback: broadcast candidate — Audio Off instead of radio");
        return select_audio_off();
    }

    return g_real_activate(flag);
}

bool install_hook()
{
    void *h = dlopen(kUiaSystemSo, RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        return false;
    }

    void *anchor = dlsym(h, kAnchorSym);
    if (!anchor) {
        LOGE("fallback: dlsym(%s) failed", kAnchorSym);
        return false;
    }

    g_base = reinterpret_cast<uintptr_t>(anchor) - kOffAnchor;
    LOGD("fallback: libjciuiasystem base=0x%08lx",
         static_cast<unsigned long>(g_base));

    uint32_t *target = reinterpret_cast<uint32_t *>(at(kOffActivatePrevious));
    if (target[0] != kPrologue0 || target[1] != kPrologue1) {
        LOGE("fallback: ABORT — activatePrevious prologue "
             "%08x %08x (expected %08x %08x); wrong FW?",
             target[0], target[1], kPrologue0, kPrologue1);
        return false;
    }

    g_get_previous   = reinterpret_cast<GetAppFn>(at(kOffGetPrevious));
    g_get_last_bcast = reinterpret_cast<GetAppFn>(at(kOffGetLastBroadcast));
    g_is_available   = reinterpret_cast<IsAvailableFn>(at(kOffIsAvailable));
    g_is_broadcast   = reinterpret_cast<IsBroadcastFn>(at(kOffIsBroadcastSource));
    g_inject_to_name = reinterpret_cast<InjectToNameFn>(
        dlsym(h, "UiApp_InjectEventToUiaName"));
    if (!g_inject_to_name) {
        LOGE("fallback: UiApp_InjectEventToUiaName missing");
        return false;
    }

    // Build a callable trampoline on an executable page: original 2
    // insns + jump to target+8. Cannot live in .data (NX).
    g_saved_prologue[0] = target[0];
    g_saved_prologue[1] = target[1];
    void *rx = mmap(nullptr, 16, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rx == MAP_FAILED) {
        LOGE("fallback: mmap trampoline failed: %s", strerror(errno));
        return false;
    }
    g_trampoline = reinterpret_cast<uint32_t *>(rx);
    g_trampoline[0] = g_saved_prologue[0];
    g_trampoline[1] = g_saved_prologue[1];
    g_trampoline[2] = kLdrPcM4;
    g_trampoline[3] = reinterpret_cast<uint32_t>(target) + 8;
    __builtin___clear_cache(reinterpret_cast<char *>(g_trampoline),
                            reinterpret_cast<char *>(g_trampoline + 4));
    g_real_activate = reinterpret_cast<ActivatePreviousFn>(g_trampoline);

    // Match aap_service's insn-patch pattern: RW -> write -> RX.
    if (!set_prot(reinterpret_cast<uintptr_t>(target), 8,
                  PROT_READ | PROT_WRITE)) {
        LOGE("fallback: mprotect RW on activatePrevious failed");
        return false;
    }
    target[0] = kLdrPcM4;
    target[1] = reinterpret_cast<uint32_t>(&hook_activatePrevious);
    __builtin___clear_cache(reinterpret_cast<char *>(target),
                            reinterpret_cast<char *>(target + 2));
    if (!set_prot(reinterpret_cast<uintptr_t>(target), 8,
                  PROT_READ | PROT_EXEC)) {
        LOGW("fallback: mprotect RX restore failed (hook is live)");
    }

    g_installed = true;
    LOGD("fallback: activatePrevious hooked @%p -> %p (trampoline %p)",
         reinterpret_cast<void *>(target),
         reinterpret_cast<void *>(&hook_activatePrevious),
         reinterpret_cast<void *>(g_trampoline));
    return true;
}

void *waiter_main(void *)
{
    // mmuiLoadUias() runs after our constructor; poll briefly for the
    // system UIA library, then give up quietly if absent (wrong PID).
    for (int i = 0; i < 100; ++i) {
        if (install_hook()) {
            return nullptr;
        }
        usleep(100 * 1000);
    }
    LOGW("fallback: libjciuiasystem.so never mapped — hook not installed");
    return nullptr;
}

} // namespace

namespace svcjcimmui_fallback {

void start()
{
    if (!libpatch_config::fallback_audio_off()) {
        LOGD("fallback: disabled by config");
        return;
    }
    pthread_t tid;
    if (preload_thread_create(&tid, waiter_main, nullptr) != 0) {
        LOGE("fallback: failed to start waiter thread");
        return;
    }
    pthread_detach(tid);
}

} // namespace svcjcimmui_fallback
