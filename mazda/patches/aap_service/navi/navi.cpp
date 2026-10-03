// SPDX-License-Identifier: AGPL-3.0-or-later
// GAL 1.6 navigation negotiation and raw-frame relay for aap_service.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define LOG_TAG "NAVI"
#include "../log.h"
#include "navi.h"
#include "common/aa_nav16_msg.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cerrno>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// Firmware 74.00.324A /usr/bin/aap_service is a non-PIE executable.
constexpr uintptr_t kVersionInsnAddr  = 0x00173eb4;
constexpr uint32_t  kVersionInsnOld   = 0xe3a02005;
constexpr uint32_t  kVersionInsnNew   = 0xe3a02006;
constexpr uintptr_t kVtableSlotAddr   = 0x001f7d2c;
constexpr uintptr_t kRouteMessageAddr = 0x0017be10;
constexpr uintptr_t kSharedPtrDeref   = 0x0016f810;
constexpr uintptr_t kIoBufferRaw      = 0x0016f2f8;
constexpr uintptr_t kIoBufferSize     = 0x0016f330;

typedef int      (*route_fn)(void *, uint32_t, uint32_t, const void *);
typedef void *   (*sp_deref_fn)(const void *);
typedef uint8_t *(*iob_raw_fn)(void *);
typedef int      (*iob_size_fn)(void *);

bool set_prot(uintptr_t addr, size_t len, int prot)
{
    const uintptr_t ps = (uintptr_t)sysconf(_SC_PAGESIZE);
    const uintptr_t page = addr & ~(ps - 1);
    return mprotect(reinterpret_cast<void *>(page), (addr + len) - page, prot) == 0;
}

const uint8_t *frame_bytes(const void *iobuf_ref, int *size_out)
{
    *size_out = 0;
    if (!iobuf_ref) return nullptr;
    void *iob = reinterpret_cast<sp_deref_fn>(kSharedPtrDeref)(iobuf_ref);
    if (!iob) return nullptr;
    const uint8_t *raw = reinterpret_cast<iob_raw_fn>(kIoBufferRaw)(iob);
    int size = reinterpret_cast<iob_size_fn>(kIoBufferSize)(iob);
    if (!raw || size <= 0) return nullptr;
    *size_out = size;
    return raw;
}

int g_tx_fd = -1;

void nav16_send_raw(const void *buf, size_t len)
{
    if (g_tx_fd < 0) {
        g_tx_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (g_tx_fd < 0) {
            LOGE("nav16: socket() failed: %s", strerror(errno));
            return;
        }
    }
    struct sockaddr_un addr;
    socklen_t addrlen;
    aa_nav16_fill_addr(addr, addrlen);
    if (sendto(g_tx_fd, buf, len, MSG_DONTWAIT,
               reinterpret_cast<struct sockaddr *>(&addr), addrlen) < 0)
        LOGV("nav16: sendto dropped: %s", strerror(errno));
}

void relay_frame(const void *iobuf_ref)
{
    int size = 0;
    const uint8_t *raw = frame_bytes(iobuf_ref, &size);
    if (!raw || size < 2) {
        LOGV("nav16: relay skipped - no/short frame (size=%d)", size);
        return;
    }
    if (size > AA_NAV16_MAX_FRAME) size = AA_NAV16_MAX_FRAME;

    char buf[sizeof(AaNav16Hdr) + AA_NAV16_MAX_FRAME];
    AaNav16Hdr hdr = {AA_NAV16_MAGIC, AA_NAV16_VERSION, 0, (uint16_t)size};
    std::memcpy(buf, &hdr, sizeof(hdr));
    std::memcpy(buf + sizeof(hdr), raw, (size_t)size);
    nav16_send_raw(buf, sizeof(hdr) + (size_t)size);
}

int hook_routeMessage(void *self, uint32_t chan, uint32_t msgId,
                      const void *iobuf_ref)
{
    const uint32_t id = msgId & 0xffff;
    if (id >= AA_NAV16_MSG_SWALLOW_FIRST && id <= AA_NAV16_MSG_SWALLOW_LAST) {
        LOGV("nav16: swallow msgId=0x%04x chan=%u -> relay", id, chan);
        relay_frame(iobuf_ref);
        return 0;
    }
    return reinterpret_cast<route_fn>(kRouteMessageAddr)(self, chan, msgId,
                                                          iobuf_ref);
}

bool bump_version()
{
    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(kVersionInsnAddr);
    if (*p != kVersionInsnOld) {
        LOGE("version: ABORT - 0x%08x at 0x%08lx, expected 0x%08x",
             *p, (unsigned long)kVersionInsnAddr, kVersionInsnOld);
        return false;
    }
    if (!set_prot(kVersionInsnAddr, 4, PROT_READ | PROT_WRITE)) {
        LOGE("version: ABORT - mprotect RW failed");
        return false;
    }
    *p = kVersionInsnNew;
    if (!set_prot(kVersionInsnAddr, 4, PROT_READ | PROT_EXEC))
        LOGW("version: mprotect RO+EX restore failed (version IS bumped; continuing)");

    __builtin___clear_cache(reinterpret_cast<char *>(kVersionInsnAddr),
                            reinterpret_cast<char *>(kVersionInsnAddr + 4));
    LOGD("version: advertising GAL 1.6 (mov r2,#5 -> #6 @0x%08lx)",
         (unsigned long)kVersionInsnAddr);
    return true;
}

bool install_nav_hook()
{
    volatile uintptr_t *slot = reinterpret_cast<volatile uintptr_t *>(kVtableSlotAddr);
    if (*slot != kRouteMessageAddr) {
        LOGE("hook: ABORT - slot7 @0x%08lx = 0x%08lx, expected 0x%08lx",
             (unsigned long)kVtableSlotAddr, (unsigned long)*slot,
             (unsigned long)kRouteMessageAddr);
        return false;
    }
    if (!set_prot(kVtableSlotAddr, sizeof(uintptr_t), PROT_READ | PROT_WRITE)) {
        LOGE("hook: ABORT - mprotect RW failed on vtable");
        return false;
    }
    *slot = reinterpret_cast<uintptr_t>(&hook_routeMessage);
    LOGD("hook: nav routeMessage installed (slot7 @0x%08lx)",
         (unsigned long)kVtableSlotAddr);
    if (!set_prot(kVtableSlotAddr, sizeof(uintptr_t), PROT_READ))
        LOGW("hook: mprotect RO restore failed (hook IS installed; continuing)");
    return true;
}

bool restore_nav_hook()
{
    volatile uintptr_t *slot = reinterpret_cast<volatile uintptr_t *>(kVtableSlotAddr);
    if (*slot != reinterpret_cast<uintptr_t>(&hook_routeMessage)) {
        LOGW("restore_nav_hook: slot7 already restored or never hooked (0x%08lx)",
             (unsigned long)*slot);
        return false;
    }
    if (!set_prot(kVtableSlotAddr, sizeof(uintptr_t), PROT_READ | PROT_WRITE)) {
        LOGE("restore_nav_hook: ABORT - mprotect RW failed; hook remains installed");
        return false;
    }
    *slot = kRouteMessageAddr;
    if (!set_prot(kVtableSlotAddr, sizeof(uintptr_t), PROT_READ)) {
        LOGW("restore_nav_hook: mprotect RO restore failed (hook IS removed; vtable writable)");
        return true; // hook is removed; partial success is not the same as full failure
    }
    return true;
}

} // namespace

namespace aap_service_navi {

void init()
{
    // Install the hook first: it swallows the 1.5 navigation messages and relays
    // them to the GAL 1.6 socket. Only advertise 1.6 after that patch is ready.
    if (!install_nav_hook()) {
        LOGE("GAL 1.6 navigation patch DISABLED - hook failed; staying stock GAL 1.5");
        return;
    }

    if (!bump_version()) {
        if (!restore_nav_hook())
            LOGE("GAL 1.6 navigation patch DISABLED - failed to roll back navigation hook");
        else
            LOGE("GAL 1.6 navigation patch ROLLED BACK; staying stock GAL 1.5");
        return;
    }

    LOGD("GAL 1.6 navigation patch ACTIVATED");
}

} // namespace aap_service_navi
