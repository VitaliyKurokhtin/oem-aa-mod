// SPDX-License-Identifier: AGPL-3.0-or-later
//
// audio_preopen.cpp — open the amp's guidance (NAVI) mix channel early so the head of an
// Android Auto guidance prompt is not clipped.
//
// The amp opens its NAVI channel only once guidance audio focus is requested, and the OEM
// requests it at stream start — a couple hundred ms after it has already granted the guidance
// duck, with the request -> amp-NAVI-up round-trip adding more. So the channel opens well after
// the first PCM sample has already reached the ALSA sink; that head is written while the amp is
// still routed to media, so it is lost acoustically, downstream of snd_pcm_writei — not in the
// digital sink.
//
// Fix: issue the OEM's own guidance focus request earlier — at the moment the OEM grants the
// guidance duck, which happens during focus negotiation, well before its own (late) stream-start
// request. It is idempotent with that later request (a duplicate grant is benign), adds no onset
// latency, injects no audio, and only ducks media slightly early. The amp NAVI channel then opens
// at or before PCM onset, not after.
//
// Trigger: the OEM's grant runs inside AudioManager::SendReplyRequestFocus, and it grants the
// guidance duck on exactly one argument pair — stream type 0xa01, request 0x802. We reproduce that
// exact gate and act only then, so the pre-open fires precisely when (and only when) the OEM's own
// duck-grant path runs — never on the mic-only listening reply or any other request.
//
// Mechanism: an entry detour on SendReplyRequestFocus. The OEM's callers reach it by a direct,
// link-time-bound branch inside blmjciaapa.so, so symbol interposition cannot catch it; a one-time
// code patch is required. The detour observes the arguments, fires the early focus request when the
// gate matches, and then falls straight through into the unmodified OEM function, which runs
// normally. The live AudioManager is the detour's own `this` (the object the OEM is about to use),
// so no separate lookup and no cached pointer.
//
// Objective validation (the pre-open provably "took hold"): the install verifies the OEM prologue
// byte-for-byte before patching and ABORTs on any mismatch (never a silent no-op), then reads the
// patch back. A status file at STATUS_PATH is written on install and refreshed on every grant, so
// `cat` on the unit confirms the detour is installed and shows live counters (grants seen / fires /
// last result) independent of build or log level.
//
// Gated under the libpatch.conf key `aa_audio_low_latency` (shared with the tail-side hold).

#define LOG_TAG "PREOPEN"
#include "../log.h"

#include "common/preload.h"
#include "common/config.h"
#include "../oem/blmjciaapa.h"   // AudioManager_SendReplyRequestFocus_addr()

#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace {

// --- OEM ABI: AUDIO_AMSERVICE_requestAudioFocus ----------------------------
// Exported by libjciamclient.so; requests guidance audio focus.
struct AMResult {
    int   code;
    char *msg1;
    char *msg2;
};
typedef void (*requestAudioFocus_fn)(void *serviceDbus, unsigned sessionId,
                                     int flags, AMResult *result);

// --- OEM gate reproduced from AudioManager::SendReplyRequestFocus -----------
// The OEM does nothing unless the stream type is 0xa01, then grants the guidance
// duck only for request 0x802 (the 0x800/0x801 requests are other focus states).
// We fire the pre-open on exactly that pair and nothing else.
const unsigned kTypeGuidance = 0xa01;   // AAP_StreamType
const unsigned kReqDuckGrant = 0x802;   // AAP_StreamRequest -> guidance duck

// --- AudioManager instance field offsets ------------------------------------
const size_t kOffServiceDbus = 0x04;    // service D-Bus handle
const size_t kOffSessionId1  = 0x24;    // guidance mix session id (-> amp NAVI)

// Dedup window: collapses a rapid re-grant of the same duck into one request
// (idempotent anyway; this just avoids a redundant call). NOT an added delay.
// CLOCK_MONOTONIC nanoseconds overflow a 32-bit long after ~2.1 s of uptime on
// this ILP32 ARM target, so all time math MUST be int64_t.
const int64_t kDedupNs = 250LL * 1000000LL;   // 250 ms

// Objective-validation status file. Written on install, refreshed on each grant.
// `cat` it on the unit to confirm the detour is live and see the counters.
const char STATUS_PATH[] = "/tmp/aa_preopen.status";

// --- ARM detour encodings (verified against the OEM prologue) ----------------
// SendReplyRequestFocus starts:
//     a6f40:  e92d4800  push {fp, lr}
//     a6f44:  e28db004  add  fp, sp, #4
//     a6f48:  e24dd020  sub  sp, sp, #32   <- resume point (entry + 8)
// We overwrite the first two words (8 bytes) with an absolute branch to our
// stub and relocate those two words into the stub's tail. Neither is
// PC-relative, so relocating them is safe.
const uint32_t kOrigWord0 = 0xe92d4800;   // push {fp, lr}
const uint32_t kOrigWord1 = 0xe28db004;   // add  fp, sp, #4
const uint32_t kLdrPcMinus4 = 0xe51ff004;  // ldr pc, [pc, #-4]  (branch via next word)

// --- lazily resolved handles / config --------------------------------------
requestAudioFocus_fn g_req_focus      = nullptr;
void                *g_amclient_handle = nullptr;

pthread_mutex_t g_lock         = PTHREAD_MUTEX_INITIALIZER;
int64_t         g_last_fire_ns = 0;      // CLOCK_MONOTONIC of last fire
__thread int    g_in_preopen   = 0;      // re-entrancy guard

// Validation counters (updated under g_lock).
unsigned g_stat_seen  = 0;   // SendReplyRequestFocus duck-grants observed
unsigned g_stat_fired = 0;   // pre-opens actually issued
int      g_stat_code  = -1;  // last requestAudioFocus result code
void    *g_detour_addr = nullptr;   // patched OEM address (for the status file)

int64_t now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Rewrite the status file. Cheap and infrequent (once per prompt at most).
// Best-effort: a failed write never affects the audio path.
void write_status(bool installed)
{
    char buf[256];
    int n = snprintf(buf, sizeof buf,
        "detour=%s addr=%p prologue=%08x,%08x\n"
        "seen=%u fired=%u last_code=%d\n",
        installed ? "installed" : "not-installed",
        g_detour_addr, kOrigWord0, kOrigWord1,
        g_stat_seen, g_stat_fired, g_stat_code);
    if (n <= 0)
        return;
    int fd = open(STATUS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    ssize_t w = write(fd, buf, (size_t)n);
    (void)w;
    close(fd);
}

// Page-align + mprotect helper (same idiom as aap_service/navi/navi.cpp).
bool set_prot(uintptr_t addr, size_t len, int prot)
{
    const uintptr_t ps   = (uintptr_t)sysconf(_SC_PAGESIZE);
    const uintptr_t page = addr & ~(ps - 1);
    return mprotect(reinterpret_cast<void *>(page), (addr + len) - page, prot) == 0;
}

// Fire the OEM's own guidance focus request early, so the amp opens its NAVI
// channel by PCM onset. `am` is the live AudioManager (the detour's `this`).
void fire_preopen(void *am)
{
    if (g_in_preopen)
        return;                     // don't re-enter from requestAudioFocus's own path

    // Dedup: collapse a rapid re-grant of the same duck into one request.
    int64_t t = now_ns();
    pthread_mutex_lock(&g_lock);
    int64_t since = t - g_last_fire_ns;
    if (g_last_fire_ns != 0 && since < kDedupNs) {
        pthread_mutex_unlock(&g_lock);
        LOGV("skip: within dedup window (%lld ms)", (long long)(since / 1000000LL));
        return;
    }
    g_last_fire_ns = t;
    pthread_mutex_unlock(&g_lock);

    if (!am) { LOGD("skip: null AudioManager"); return; }

    void    *serviceDbus = *reinterpret_cast<void **>(
                               reinterpret_cast<char *>(am) + kOffServiceDbus);
    unsigned session     = *reinterpret_cast<unsigned *>(
                               reinterpret_cast<char *>(am) + kOffSessionId1);
    if (!serviceDbus || session == 0) {
        LOGD("skip: serviceDbus=%p session=0x%x not ready", serviceDbus, session);
        return;
    }

    if (!g_req_focus) {
        g_req_focus = reinterpret_cast<requestAudioFocus_fn>(resolve_real_symbol(
            "AUDIO_AMSERVICE_requestAudioFocus", "libjciamclient.so",
            "/jci/lib/libjciamclient.so", nullptr, &g_amclient_handle));
    }
    if (!g_req_focus) { LOGW("skip: requestAudioFocus unresolved"); return; }

    // The OEM's own audio-focus request, fired early.
    AMResult r;
    r.code = 0; r.msg1 = nullptr; r.msg2 = nullptr;
    g_in_preopen = 1;
    g_req_focus(serviceDbus, session, 0, &r);
    g_in_preopen = 0;

    pthread_mutex_lock(&g_lock);
    g_stat_fired++;
    g_stat_code = r.code;
    pthread_mutex_unlock(&g_lock);

    LOGV("pre-opened amp NAVI: requestAudioFocus(dbus=%p sess=0x%x) code=%d",
         serviceDbus, session, r.code);
}

// The detour's callee. Runs at SendReplyRequestFocus entry with the OEM's own
// arguments (this, AAP_StreamType, AAP_StreamRequest), fires the pre-open on the
// exact duck-grant gate, then returns so the original OEM function runs unchanged.
// Only its address is used (the stub calls it via blx), so plain linkage is fine.
void preopen_hook(void *self, unsigned type, unsigned req)
{
    if (type != kTypeGuidance || req != kReqDuckGrant)
        return;
    pthread_mutex_lock(&g_lock);
    g_stat_seen++;
    pthread_mutex_unlock(&g_lock);
    fire_preopen(self);
    write_status(true);
}

// Runtime-built trampoline stub (9 words). Entered from the patched OEM prologue
// in ARM state; saves the arg/scratch registers, calls preopen_hook, restores
// them, runs the relocated original prologue, and jumps back to entry+8.
//
//   [0] e92d500f  push {r0-r3, r12, lr}   ; preserve args + scratch + lr
//   [1] e59f3014  ldr  r3, [pc, #0x14]    ; r3 = [8] = &preopen_hook
//   [2] e12fff33  blx  r3                 ; preopen_hook(this, type, req)
//   [3] e8bd500f  pop  {r0-r3, r12, lr}   ; restore exactly as the OEM saw them
//   [4] <orig w0> push {fp, lr}           ; relocated OEM prologue word 0
//   [5] <orig w1> add  fp, sp, #4         ; relocated OEM prologue word 1
//   [6] e51ff004  ldr  pc, [pc, #-4]      ; branch to [7]
//   [7] <resume>  entry + 8               ; a6f48 at runtime
//   [8] <&hook>   &preopen_hook           ; (thumb bit preserved -> blx interworks)
uint32_t *build_stub(uintptr_t resume)
{
    const uintptr_t ps = (uintptr_t)sysconf(_SC_PAGESIZE);
    void *mem = mmap(nullptr, ps, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        LOGC("install: ABORT - mmap for detour stub failed");
        return nullptr;
    }
    uint32_t *s = reinterpret_cast<uint32_t *>(mem);
    s[0] = 0xe92d500f;
    s[1] = 0xe59f3014;
    s[2] = 0xe12fff33;
    s[3] = 0xe8bd500f;
    s[4] = kOrigWord0;
    s[5] = kOrigWord1;
    s[6] = kLdrPcMinus4;
    s[7] = (uint32_t)resume;
    s[8] = (uint32_t)reinterpret_cast<uintptr_t>(&preopen_hook);

    if (mprotect(mem, ps, PROT_READ | PROT_EXEC) != 0) {
        LOGC("install: ABORT - mprotect RX on detour stub failed");
        return nullptr;
    }
    __builtin___clear_cache(reinterpret_cast<char *>(s),
                            reinterpret_cast<char *>(s + 9));
    return s;
}

// Install the entry detour on AudioManager::SendReplyRequestFocus. Once per
// process; re-calls are no-ops. Verifies the OEM prologue byte-for-byte and
// ABORTs (never silently no-ops) on any mismatch or missing library.
void install_detour()
{
    static bool done = false;
    if (done)
        return;

    void *target = AudioManager_SendReplyRequestFocus_addr();
    if (!target) {
        // blmjciaapa.so not mapped yet / wrong PID. Not fatal — retry on the
        // next session bring-up; the caller invokes us again then.
        LOGD("install: SendReplyRequestFocus unresolved (blmjciaapa.so not mapped?) — will retry");
        return;
    }

    volatile uint32_t *code = reinterpret_cast<volatile uint32_t *>(target);
    if (code[0] == kLdrPcMinus4) {          // already patched (defensive)
        done = true;
        LOGD("install: detour already present at %p", target);
        return;
    }
    if (code[0] != kOrigWord0 || code[1] != kOrigWord1) {
        // Wrong offset / firmware drift. Refuse to patch and stay inert — a
        // wrong write here would corrupt the OEM. Loud, and survives release.
        LOGC("install: ABORT - prologue mismatch at %p: got %08x,%08x expected %08x,%08x",
             target, code[0], code[1], kOrigWord0, kOrigWord1);
        done = true;                        // don't hammer a bad target every session
        return;
    }

    uintptr_t t = reinterpret_cast<uintptr_t>(target);
    uint32_t *stub = build_stub(t + 8);
    if (!stub)                              // build_stub already logged the abort
        return;

    if (!set_prot(t, 8, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        LOGC("install: ABORT - mprotect RW on %p failed", target);
        return;
    }
    code[0] = kLdrPcMinus4;                 // ldr pc, [pc, #-4]
    code[1] = (uint32_t)reinterpret_cast<uintptr_t>(stub);
    set_prot(t, 8, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char *>(t),
                            reinterpret_cast<char *>(t + 8));

    // Read back and confirm the patch physically landed — objective proof the
    // approach took hold, independent of whether it ever fires.
    if (code[0] != kLdrPcMinus4 ||
        code[1] != (uint32_t)reinterpret_cast<uintptr_t>(stub)) {
        LOGC("install: ABORT - read-back mismatch at %p (%08x,%08x)",
             target, code[0], code[1]);
        done = true;
        return;
    }

    done = true;
    g_detour_addr = target;
    write_status(true);
    LOGD("install: detour LIVE on SendReplyRequestFocus @%p -> stub %p (verified); "
         "status at %s", target, (void *)stub, STATUS_PATH);
}

} // namespace

// --- lifecycle entry -------------------------------------------------------
void audio_preopen_post_aap_create_session(void)
{
    install_detour();
}
