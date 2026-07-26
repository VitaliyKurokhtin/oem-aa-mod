// SPDX-License-Identifier: AGPL-3.0-or-later
//
// common/audio/drain_event.h — cross-process "the AA audio tail has finished draining"
// event, shared by the two halves of the audio END-cutoff fix:
//
//   PRODUCER  aap_service / sem_clockfix.cpp   — posts one token the instant the
//             EOS-drain wait returns success, i.e. the exact moment the buffered
//             tail has played out.
//   CONSUMER  blmjciaapa / audio_stopdelay.cpp — waits on that token to release the
//             amp mix (re-arm the OEM stop timer) precisely when the tail is gone,
//             instead of guessing a fixed hold duration.
//
// The two live in DIFFERENT processes, so the event is a System V counting semaphore
// keyed by a fixed IPC key. SysV (not a POSIX named semaphore) on purpose: the platform
// already relies on SysV IPC for ALSA dmix, so it is known-good here, and it needs no
// /dev/shm. Counting semantics give us lossless edge delivery — a token posted before
// the consumer waits is still there when it does.
//
// Header-only, matching the common/ convention (preload.h / config.h). Requires
// _GNU_SOURCE to be defined before inclusion (for semtimedop, declared under
// __USE_GNU in <sys/sem.h>) — every including TU defines it at the top, as they do
// for dladdr()/RTLD_NEXT.

#ifndef LIBPATCH_COMMON_AUDIO_DRAIN_EVENT_H
#define LIBPATCH_COMMON_AUDIO_DRAIN_EVENT_H

#include "common/log.h"

#include <errno.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <time.h>

namespace drain_event {

// Fixed IPC key shared by producer and consumer. "amdr" = AudioManager DRain.
// Far from the ALSA dmix ipc_key (10000 = 0x2710); no other SysV sem user on the
// unit is known to collide.
static const key_t kKey = 0x616D6472;

// glibc no longer defines `union semun`; POSIX leaves it to the caller. We use our
// own named union (never clashes with a system definition). Its first member is an
// int, so passing it by value to the variadic semctl() for SETVAL is layout-correct.
union semun_t {
    int                 val;
    struct semid_ds    *buf;
    unsigned short     *array;
};

// Open (creating if absent) the shared 1-slot semaphore. Both processes call this
// with IPC_CREAT; whoever runs first creates it, the other attaches. 0666 so it works
// even if the two services run as different users. Returns the semid, or -1 on error.
inline int open_sem()
{
    int id = semget(kKey, 1, IPC_CREAT | 0666);
    if (id < 0) {
        LOGE("drain_event: semget(0x%lx) failed: errno=%d", (unsigned long)kKey, errno);
    }
    return id;
}

// PRODUCER: signal that one drain completed (V / +1). Best-effort — a failure (e.g.
// the value saturating at SEMVMX because the consumer wasn't clearing) is logged and
// ignored; the consumer's fail-safe cap still bounds the worst case.
inline void post(int id)
{
    if (id < 0) return;
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op  = 1;
    op.sem_flg = 0;
    if (semop(id, &op, 1) != 0) {
        LOGW("drain_event: post failed: errno=%d", errno);
    }
}

// PRODUCER: clear any stale tokens so the consumer's next wait() blocks for a FRESH drain.
// Call this at drain-START (in the producer, just before beginning the EOS-drain wait) — NOT
// at the consumer's arm. The stop reaches aap_service's drain before it reaches blmjciaapa's
// arm, so a short prompt can finish draining and post its token before the consumer even
// arms; resetting on the producer at drain-start clears leftovers without ever eating the
// current drain's own token, whichever side wins the arm-vs-drain race.
inline void reset(int id)
{
    if (id < 0) return;
    union semun_t su;
    su.val = 0;
    if (semctl(id, 0, SETVAL, su) != 0) {
        LOGW("drain_event: reset failed: errno=%d", errno);
    }
}

// CONSUMER: wait up to timeout_sec for one drain-done token (P / -1). EINTR is
// retried. Returns 0 if a token arrived, -1 on timeout or error (caller then relies
// on its fail-safe cap).
inline int wait(int id, long timeout_sec)
{
    if (id < 0) return -1;
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op  = -1;
    op.sem_flg = 0;
    struct timespec to;
    to.tv_sec  = timeout_sec;
    to.tv_nsec = 0;
    int r;
    do {
        r = semtimedop(id, &op, 1, &to);
    } while (r != 0 && errno == EINTR);
    return r == 0 ? 0 : -1;
}

} // namespace drain_event

#endif // LIBPATCH_COMMON_AUDIO_DRAIN_EVENT_H
