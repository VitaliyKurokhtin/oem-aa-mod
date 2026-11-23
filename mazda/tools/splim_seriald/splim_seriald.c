// SPDX-License-Identifier: AGPL-3.0-or-later
//
// splim_seriald — USB serial → /data_persist/splim for Mazda Connect (CMU150).
// Copyright (C) 2026 KID MIXER-MODER.
//
// Part of an AGPL-3.0 work — see NOTICE.md for full attribution.
//
// Reads line-oriented "SPLIM <km/h>" from an ESP32 USB CDC port (ttyACM*/ttyUSB*)
// and atomically writes "/data_persist/splim" as "<limit> <unix_ts>" — the same
// format libpatch-blmjcicarplay::read_splim() already consumes.
//
// EXCLUSIVITY: a lock file under /data_persist keeps a second instance from
// opening the same writer path. Prefer a single writer of /data_persist/splim
// (do not run OCR / a competing UDP writer at the same time).

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SPLIM_PATH     "/data_persist/splim"
#define SPLIM_TMP      "/data_persist/splim.tmp"
#define LOCK_PATH      "/data_persist/splim_seriald.lock"
#define DEFAULT_BAUD   B115200
#define SPLIM_LINE_MAX 128   // not LINE_MAX: that name collides with <limits.h>
// The HUD treats splim as stale after 8 s and the ESP32 re-sends every 2 s, so an
// unchanged limit only needs a fresh timestamp this often. Cuts flash writes.
#define REFRESH_SEC    3

static volatile sig_atomic_t g_stop = 0;
static int g_lock_fd = -1;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int acquire_lock(void)
{
    g_lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT, 0644);
    if (g_lock_fd < 0) {
        fprintf(stderr, "splim_seriald: open lock: %s\n", strerror(errno));
        return -1;
    }
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "splim_seriald: already running (lock busy)\n");
        close(g_lock_fd);
        g_lock_fd = -1;
        return -1;
    }
    return 0;
}

static void release_lock(void)
{
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
}

static int write_splim(int limit)
{
    if (limit < 5 || limit > 200)
        return -1;

    FILE *f = fopen(SPLIM_TMP, "w");
    if (!f) {
        fprintf(stderr, "splim_seriald: open tmp: %s\n", strerror(errno));
        return -1;
    }
    // No fsync: the value is refreshed every few seconds and rename() keeps the
    // swap atomic, so a power cut only loses a sample. fsync per write wears flash.
    int ok = fprintf(f, "%d %ld\n", limit, (long)time(NULL)) > 0;
    if (fclose(f) != 0)
        ok = 0;
    if (!ok) {
        fprintf(stderr, "splim_seriald: write tmp: %s\n", strerror(errno));
        unlink(SPLIM_TMP);
        return -1;
    }
    if (rename(SPLIM_TMP, SPLIM_PATH) != 0) {
        fprintf(stderr, "splim_seriald: rename: %s\n", strerror(errno));
        unlink(SPLIM_TMP);
        return -1;
    }
    return 0;
}

static int configure_tty(int fd)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0)
        return -1;
    cfmakeraw(&tio);
    cfsetispeed(&tio, DEFAULT_BAUD);
    cfsetospeed(&tio, DEFAULT_BAUD);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 5;   // 0.5 s read timeout → wake for signals / reopen
    return tcsetattr(fd, TCSANOW, &tio);
}

static int open_device(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    // Drop O_NONBLOCK after open so VTIME-based reads work.
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    if (configure_tty(fd) != 0) {
        close(fd);
        return -1;
    }
    // Discard stale boot chatter.
    tcflush(fd, TCIFLUSH);
    return fd;
}

// Prefer explicit path; otherwise pick the first /dev/ttyACM* then ttyUSB*.
static int open_auto(char *chosen, size_t chosen_len)
{
    static const char *const prefixes[] = { "ttyACM", "ttyUSB", NULL };
    DIR *d = opendir("/dev");
    if (!d)
        return -1;

    for (int p = 0; prefixes[p]; p++) {
        struct dirent *ent;
        rewinddir(d);
        while ((ent = readdir(d)) != NULL) {
            if (strncmp(ent->d_name, prefixes[p], strlen(prefixes[p])) != 0)
                continue;
            char path[64];
            snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
            int fd = open_device(path);
            if (fd >= 0) {
                snprintf(chosen, chosen_len, "%s", path);
                closedir(d);
                return fd;
            }
        }
    }
    closedir(d);
    return -1;
}

// Accept "SPLIM 60", optional CR, leading log noise on the same line ignored
// only when the line starts with SPLIM (ESP32 also prints [BOOT]/ debug).
static int parse_line(const char *line, int *out_limit)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, "SPLIM", 5) != 0)
        return -1;
    p += 5;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p < '0' || *p > '9')
        return -1;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p)
        return -1;
    // Only trailing blanks allowed. A line cut short by an unplug/reset and glued to
    // the next one ("SPLIM 6SPLIM 60") must be rejected, not read as limit 6.
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')
        return -1;
    if (v < 5 || v > 200)
        return -1;
    *out_limit = (int)v;
    return 0;
}

static void feed_byte(char *buf, size_t *len, unsigned char c, int *last_limit)
{
    static time_t last_write = 0;
    if (c == '\r')
        return;
    if (c == '\n') {
        buf[*len] = '\0';
        int limit = 0;
        if (*len > 0 && parse_line(buf, &limit) == 0) {
            time_t now = time(NULL);
            // Rewrite on change, or to refresh the timestamp. now < last_write
            // means the wall clock stepped back, so refresh immediately.
            if (limit != *last_limit || now < last_write ||
                now - last_write >= REFRESH_SEC) {
                if (write_splim(limit) == 0) {
                    last_write = now;
                    if (limit != *last_limit) {
                        fprintf(stderr, "splim_seriald: splim=%d\n", limit);
                        *last_limit = limit;
                    }
                }
            }
        }
        *len = 0;
        return;
    }
    if (*len + 1 < SPLIM_LINE_MAX)
        buf[(*len)++] = (char)c;
    else
        *len = 0;   // overflow → resync on next newline
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    // Launched via system() from inside jciCARPLAY, so every non-CLOEXEC fd of that
    // process (D-Bus sockets, device nodes, logs) leaked into us. Hold none of them:
    // a daemon pinning them would outlive jciCARPLAY and keep them open.
    for (int fd = 3; fd < 256; fd++)
        close(fd);

    if (acquire_lock() != 0)
        return 1;

    const char *want = NULL;
    if (argc >= 2 && argv[1][0])
        want = argv[1];
    else if (getenv("SPLIM_SERIAL_DEV") && getenv("SPLIM_SERIAL_DEV")[0])
        want = getenv("SPLIM_SERIAL_DEV");

    char path[64] = "";
    int fd = -1;
    int last_limit = -1;
    char line[SPLIM_LINE_MAX];
    size_t llen = 0;

    fprintf(stderr, "splim_seriald: starting (want=%s)\n",
            want ? want : "auto ttyACM*/ttyUSB*");

    while (!g_stop) {
        if (fd < 0) {
            if (want) {
                snprintf(path, sizeof(path), "%s", want);
                fd = open_device(want);
            } else {
                fd = open_auto(path, sizeof(path));
            }
            if (fd < 0) {
                sleep(2);
                continue;
            }
            fprintf(stderr, "splim_seriald: opened %s\n", path);
            llen = 0;
        }

        // Wait with poll(), not a bare read(): once the ESP32 is unplugged the tty is
        // hung up and read() returns 0 forever, which would spin the CPU and never
        // reopen. poll() times out when idle (lets us notice g_stop) and reports a
        // hangup explicitly.
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 1000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "splim_seriald: poll: %s — reopening\n", strerror(errno));
            close(fd);
            fd = -1;
            sleep(1);
            continue;
        }
        if (pr == 0)
            continue;   // idle

        unsigned char chunk[64];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        // n == 0 after poll() said readable is EOF (hangup); POLLHUP/ERR/NVAL with no
        // data pending is the same thing. Either way the port is gone — reopen.
        if (n <= 0 || (pfd.revents & (POLLERR | POLLNVAL))) {
            fprintf(stderr, "splim_seriald: port lost (%s) — reopening\n",
                    n < 0 ? strerror(errno) : "hangup");
            close(fd);
            fd = -1;
            sleep(1);
            continue;
        }
        for (ssize_t i = 0; i < n; i++)
            feed_byte(line, &llen, chunk[i], &last_limit);
    }

    if (fd >= 0)
        close(fd);
    release_lock();
    return 0;
}
