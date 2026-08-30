// SPDX-License-Identifier: AGPL-3.0-or-later
// Set the ALSA auto-start threshold on Android Auto playback sinks only. The
// guidance sink starts on a multi-period cushion, the others on one period;
// the shared dmix geometry and every non-AA PCM handle stay untouched.

#define LOG_TAG "AUDIO"
#include "../log.h"
#include "audio.h"
#include "common/preload.h"

#include <alsa/asoundlib.h>
#include <alloca.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>

namespace {

// Guidance sink (androidautoAltAudio, 16 kHz mono) auto-start threshold, in periods.
// The threshold is how much audio the sink buffers before it starts playing — the cushion that
// absorbs delivery jitter. One period underruns on the first late frame, so the prompt garbles or
// drops; four periods (128 ms) is the whole 2048-frame buffer, the ceiling ALSA allows (a start
// threshold cannot exceed the buffer). The other AA sinks start on one period.
const snd_pcm_uframes_t kGuidanceStartPeriods = 4;   // 128 ms; buffer ceiling

typedef int (*pcm_open_fn)(snd_pcm_t **, const char *, snd_pcm_stream_t, int);
typedef int (*pcm_close_fn)(snd_pcm_t *);
typedef int (*set_start_threshold_fn)(snd_pcm_t *, snd_pcm_sw_params_t *,
                                      snd_pcm_uframes_t);
typedef int (*get_params_fn)(snd_pcm_t *, snd_pcm_uframes_t *,
                             snd_pcm_uframes_t *);
typedef size_t (*hw_params_sizeof_fn)(void);
typedef int (*hw_params_current_fn)(snd_pcm_t *, snd_pcm_hw_params_t *);
typedef int (*hw_params_get_rate_fn)(const snd_pcm_hw_params_t *, unsigned int *, int *);

pcm_open_fn            g_real_open = nullptr;
pcm_close_fn           g_real_close = nullptr;
set_start_threshold_fn g_real_set_start_threshold = nullptr;
get_params_fn          g_real_get_params = nullptr;
hw_params_sizeof_fn    g_real_hw_params_sizeof = nullptr;
hw_params_current_fn   g_real_hw_params_current = nullptr;
hw_params_get_rate_fn  g_real_hw_params_get_rate = nullptr;
#if LOG_LEVEL <= LOG_LEVEL_VERBOSE
// VERBOSE only: the guidance sink's real write path and fill query, used solely by the
// diagnostic probe below. A release build has no snd_pcm_writei override, so neither is
// resolved there.
typedef snd_pcm_sframes_t (*pcm_writei_fn)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
pcm_writei_fn          g_real_writei = nullptr;
typedef snd_pcm_sframes_t (*pcm_avail_update_fn)(snd_pcm_t *);
pcm_avail_update_fn    g_real_avail_update = nullptr;
#endif
pthread_once_t         g_resolve_once = PTHREAD_ONCE_INIT;
bool                   g_enabled = false;

struct TrackedPcm {
    snd_pcm_t *pcm;
    const char *name;
    snd_pcm_uframes_t buffer_size;   // captured at start_threshold (0 until known)
    unsigned rate;                   // Hz, captured at start_threshold (0 if unavailable)
    unsigned writes;                 // guidance writei count since open (head-window throttle)
};

TrackedPcm g_tracked[8] = {};
pthread_mutex_t g_tracked_mu = PTHREAD_MUTEX_INITIALIZER;

void resolve_real_functions()
{
    g_real_open = reinterpret_cast<pcm_open_fn>(dlsym(RTLD_NEXT, "snd_pcm_open"));
    g_real_close = reinterpret_cast<pcm_close_fn>(dlsym(RTLD_NEXT, "snd_pcm_close"));
    g_real_set_start_threshold = reinterpret_cast<set_start_threshold_fn>(
        dlsym(RTLD_NEXT, "snd_pcm_sw_params_set_start_threshold"));
    g_real_get_params = reinterpret_cast<get_params_fn>(
        dlsym(RTLD_NEXT, "snd_pcm_get_params"));
    // Optional sample-rate lookup, used only to print geometry in ms; never affects the threshold.
    g_real_hw_params_sizeof = reinterpret_cast<hw_params_sizeof_fn>(
        dlsym(RTLD_NEXT, "snd_pcm_hw_params_sizeof"));
    g_real_hw_params_current = reinterpret_cast<hw_params_current_fn>(
        dlsym(RTLD_NEXT, "snd_pcm_hw_params_current"));
    g_real_hw_params_get_rate = reinterpret_cast<hw_params_get_rate_fn>(
        dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_rate"));
#if LOG_LEVEL <= LOG_LEVEL_VERBOSE
    g_real_writei = reinterpret_cast<pcm_writei_fn>(
        dlsym(RTLD_NEXT, "snd_pcm_writei"));
    g_real_avail_update = reinterpret_cast<pcm_avail_update_fn>(
        dlsym(RTLD_NEXT, "snd_pcm_avail_update"));
#endif
}

// Best-effort read of the PCM's negotiated sample rate (Hz), 0 if unavailable. hw params are
// fully negotiated by the time the OEM sets the start threshold, so current() reflects the real
// geometry. Pure measurement — never affects the threshold decision.
unsigned query_rate(snd_pcm_t *pcm)
{
    if (!g_real_hw_params_sizeof || !g_real_hw_params_current || !g_real_hw_params_get_rate)
        return 0;
    size_t sz = g_real_hw_params_sizeof();
    if (!sz) return 0;
    snd_pcm_hw_params_t *hw = static_cast<snd_pcm_hw_params_t *>(alloca(sz));
    std::memset(hw, 0, sz);
    if (g_real_hw_params_current(pcm, hw) < 0) return 0;
    unsigned rate = 0;
    int dir = 0;
    if (g_real_hw_params_get_rate(hw, &rate, &dir) < 0) return 0;
    return rate;
}


const char *canonical_aa_name(const char *name)
{
    if (!name) return nullptr;
    if (std::strcmp(name, "androidautoMainAudio") == 0)
        return "androidautoMainAudio";
    if (std::strcmp(name, "androidautoMainAudioVR") == 0)
        return "androidautoMainAudioVR";
    if (std::strcmp(name, "androidautoAltAudio") == 0)
        return "androidautoAltAudio";
    return nullptr;
}

void track_pcm(snd_pcm_t *pcm, const char *name)
{
    pthread_mutex_lock(&g_tracked_mu);
    for (size_t i = 0; i < sizeof(g_tracked) / sizeof(g_tracked[0]); ++i) {
        if (!g_tracked[i].pcm) {
            g_tracked[i].pcm = pcm;
            g_tracked[i].name = name;
            g_tracked[i].buffer_size = 0;   // filled in at start_threshold
            g_tracked[i].rate = 0;
            g_tracked[i].writes = 0;        // fresh head-window counter per prompt
            pthread_mutex_unlock(&g_tracked_mu);
            LOGD("tracking playback PCM %s handle=%p", name, (void *)pcm);
            return;
        }
    }
    pthread_mutex_unlock(&g_tracked_mu);
    LOGE("cannot track playback PCM %s handle=%p: table full", name, (void *)pcm);
}

const char *tracked_name(snd_pcm_t *pcm)
{
    const char *name = nullptr;
    pthread_mutex_lock(&g_tracked_mu);
    for (size_t i = 0; i < sizeof(g_tracked) / sizeof(g_tracked[0]); ++i) {
        if (g_tracked[i].pcm == pcm) {
            name = g_tracked[i].name;
            break;
        }
    }
    pthread_mutex_unlock(&g_tracked_mu);
    return name;
}

void untrack_pcm(snd_pcm_t *pcm)
{
    pthread_mutex_lock(&g_tracked_mu);
    for (size_t i = 0; i < sizeof(g_tracked) / sizeof(g_tracked[0]); ++i) {
        if (g_tracked[i].pcm == pcm) {
            g_tracked[i] = TrackedPcm();
            break;
        }
    }
    pthread_mutex_unlock(&g_tracked_mu);
}

// Record the negotiated geometry on the tracked entry so the writei probe can turn ALSA's
// available-frame count into a fill level in ms. Cheap; runs for every AA sink (release too),
// but only the guidance probe reads it back.
void set_tracked_geometry(snd_pcm_t *pcm, snd_pcm_uframes_t buffer_size, unsigned rate)
{
    pthread_mutex_lock(&g_tracked_mu);
    for (size_t i = 0; i < sizeof(g_tracked) / sizeof(g_tracked[0]); ++i) {
        if (g_tracked[i].pcm == pcm) {
            g_tracked[i].buffer_size = buffer_size;
            g_tracked[i].rate = rate;
            break;
        }
    }
    pthread_mutex_unlock(&g_tracked_mu);
}

#if LOG_LEVEL <= LOG_LEVEL_VERBOSE
// How many writes at the start of each prompt to log in full (the "head" — the window where an
// onset underrun would garble the voice). ~1 s of audio; xruns are logged beyond it regardless.
const unsigned kHeadWrites = 24;

// Under the table lock: is this a sink we probe? If so, hand back its short tag, geometry, and the
// 0-based index of THIS write (post-increments the stored counter). Two sinks are probed:
//   "gd" androidautoAltAudio      — nav guidance (16 kHz mono); the head-cut / defect-2 sink.
//   "vr" androidautoMainAudioVR   — voice/assistant/message-readout TTS; the defect-1 (AAVR mute
//                                   window) sink. Un-instrumented before this diagnostic build, so
//                                   the readout PCM onset vs the amp SetMute->SetUnMute window was
//                                   invisible; now its writei#0 timestamp + head amplitude are
//                                   logged so the clip can be measured directly.
// The music sink (androidautoMainAudio) and everything untracked => nullptr, so the writei
// interpose stays a zero-work pass-through there (no per-frame music spam).
const char *probe_write_slot(snd_pcm_t *pcm, snd_pcm_uframes_t *buf_out,
                             unsigned *rate_out, unsigned *idx_out)
{
    const char *tag = nullptr;
    pthread_mutex_lock(&g_tracked_mu);
    for (size_t i = 0; i < sizeof(g_tracked) / sizeof(g_tracked[0]); ++i) {
        if (g_tracked[i].pcm == pcm) {
            if (g_tracked[i].name) {
                if (std::strcmp(g_tracked[i].name, "androidautoAltAudio") == 0)
                    tag = "gd";
                else if (std::strcmp(g_tracked[i].name, "androidautoMainAudioVR") == 0)
                    tag = "vr";
            }
            if (tag) {
                *buf_out   = g_tracked[i].buffer_size;
                *rate_out  = g_tracked[i].rate;
                *idx_out   = g_tracked[i].writes++;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_tracked_mu);
    return tag;
}

#endif

} // namespace

namespace aap_service_audio {

void init()
{
    g_enabled = true;
    LOGD("AA playback start threshold override active "
         "(guidance %lu periods, others one period)",
         (unsigned long)kGuidanceStartPeriods);
}

} // namespace aap_service_audio

extern "C" PRELOAD_EXPORT
int snd_pcm_open(snd_pcm_t **pcm, const char *name,
                 snd_pcm_stream_t stream, int mode)
{
    pthread_once(&g_resolve_once, resolve_real_functions);
    if (!g_real_open) return -ENOSYS;

    int rc = g_real_open(pcm, name, stream, mode);
    const char *aa_name = canonical_aa_name(name);
    if (rc == 0 && g_enabled && pcm && *pcm &&
        stream == SND_PCM_STREAM_PLAYBACK && aa_name)
        track_pcm(*pcm, aa_name);
    return rc;
}

extern "C" PRELOAD_EXPORT
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm,
                                          snd_pcm_sw_params_t *params,
                                          snd_pcm_uframes_t threshold)
{
    pthread_once(&g_resolve_once, resolve_real_functions);
    if (!g_real_set_start_threshold) return -ENOSYS;

    const char *name = g_enabled ? tracked_name(pcm) : nullptr;
    if (!name || !g_real_get_params)
        return g_real_set_start_threshold(pcm, params, threshold);

    snd_pcm_uframes_t buffer_size = 0;
    snd_pcm_uframes_t period_size = 0;
    int rc = g_real_get_params(pcm, &buffer_size, &period_size);
    if (rc < 0 || period_size == 0) {
        LOGW("%s handle=%p: get_params rc=%d; keeping threshold=%lu",
             name, (void *)pcm, rc, (unsigned long)threshold);
        return g_real_set_start_threshold(pcm, params, threshold);
    }

    // Guidance starts on a few periods (a real cushion below the pre-roll); the others on one.
    const bool is_guidance = std::strcmp(name, "androidautoAltAudio") == 0;
    const snd_pcm_uframes_t periods = is_guidance ? kGuidanceStartPeriods : 1;
    snd_pcm_uframes_t want = period_size * periods;
    if (want > buffer_size) want = buffer_size;                 // never exceed the ring
    const snd_pcm_uframes_t replacement = want < threshold ? want : threshold;

    const unsigned rate = query_rate(pcm);   // measurement-only: 0 if unavailable
    set_tracked_geometry(pcm, buffer_size, rate);   // for the writei fill probe (VERBOSE)
    if (rate)
        LOGD("%s handle=%p rate=%uHz: buffer=%lu(%lums) period=%lu(%lums) "
             "start_threshold %lu -> %lu (%lums, %lu periods)",
             name, (void *)pcm, rate,
             (unsigned long)buffer_size, (unsigned long)(buffer_size * 1000 / rate),
             (unsigned long)period_size, (unsigned long)(period_size * 1000 / rate),
             (unsigned long)threshold, (unsigned long)replacement,
             (unsigned long)(replacement * 1000 / rate), (unsigned long)periods);
    else
        LOGD("%s handle=%p: buffer=%lu period=%lu start_threshold %lu -> %lu (%lu periods)",
             name, (void *)pcm, (unsigned long)buffer_size,
             (unsigned long)period_size, (unsigned long)threshold,
             (unsigned long)replacement, (unsigned long)periods);
    return g_real_set_start_threshold(pcm, params, replacement);
}

extern "C" PRELOAD_EXPORT
int snd_pcm_close(snd_pcm_t *pcm)
{
    pthread_once(&g_resolve_once, resolve_real_functions);
    if (!g_real_close) return -ENOSYS;
#if LOG_LEVEL <= LOG_LEVEL_VERBOSE
    // Probed-sink close marker (VERBOSE): bounds the prompt's sink lifetime and reports how many
    // head writes it took, so the timeline parser can pair open->close per prompt without guessing.
    // Same two sinks as the writei probe ("gd" guidance / "vr" voice).
    if (g_enabled) {
        pthread_mutex_lock(&g_tracked_mu);
        for (size_t i = 0; i < sizeof(g_tracked) / sizeof(g_tracked[0]); ++i) {
            if (g_tracked[i].pcm == pcm && g_tracked[i].name) {
                const char *tag = nullptr;
                if (std::strcmp(g_tracked[i].name, "androidautoAltAudio") == 0)
                    tag = "gd";
                else if (std::strcmp(g_tracked[i].name, "androidautoMainAudioVR") == 0)
                    tag = "vr";
                if (tag)
                    LOGV("%s CLOSE handle=%p writes=%u", tag, (void *)pcm, g_tracked[i].writes);
                break;
            }
        }
        pthread_mutex_unlock(&g_tracked_mu);
    }
#endif
    if (g_enabled) untrack_pcm(pcm);
    return g_real_close(pcm);
}

#if LOG_LEVEL <= LOG_LEVEL_VERBOSE
// snd_pcm_writei interpose — VERBOSE only, compiled out of release (release has no writei
// override, so the media path is untouched). For a probed sink ("gd" guidance / "vr" voice) it
// logs the ring fill and the write's return code before each head write:
//   avail = frames free for writing => fill = buffer_size - avail (how full the ring was)
//   fill trending to 0 across the head window = the sink draining faster than it refills
//   rc < 0 (e.g. -EPIPE) = an underrun the sink then recovers from
// The owning AAL playback thread also calls writei, so reading avail_update here is race-free.
extern "C" PRELOAD_EXPORT
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
    pthread_once(&g_resolve_once, resolve_real_functions);
    if (!g_real_writei) return -ENOSYS;
    if (!g_enabled) return g_real_writei(pcm, buffer, size);

    snd_pcm_uframes_t buf = 0;
    unsigned rate = 0, idx = 0;
    const char *tag = probe_write_slot(pcm, &buf, &rate, &idx);
    if (!tag)
        return g_real_writei(pcm, buffer, size);   // music / untracked: straight through

    const snd_pcm_sframes_t avail = g_real_avail_update ? g_real_avail_update(pcm) : -1;

    // Head-frame amplitude probe: peak and mean-abs of this frame's leading S16 samples, read from
    // the caller's buffer before the write. Low then rising over the first frames = a silence-led
    // head; high on frame #0 = an already-clipped head. Integer-only, over the head window only.
    // Bounded by `size` (one int16 per frame) so it stays in-bounds whether the sink is mono
    // (gd, exact) or stereo (vr reads the leading half — still a valid onset-amplitude proxy).
    long pk = -1, mabs = -1;
    if (idx < kHeadWrites && buffer && size) {
        const int16_t *s = static_cast<const int16_t *>(buffer);
        int32_t peak = 0; uint64_t sum_abs = 0;
        for (snd_pcm_uframes_t i = 0; i < size; ++i) {
            int32_t v = s[i]; if (v < 0) v = -v;
            if (v > peak) peak = v;
            sum_abs += static_cast<uint32_t>(v);
        }
        pk   = peak;
        mabs = static_cast<long>(sum_abs / size);
    }

    // On the first frame, re-read the rate here: the stream is fully prepared by now, so it reports
    // the real ALSA-layer rate even when the sw_params-time query returned 0.
    if (idx == 0) {
        const unsigned r = query_rate(pcm);
        LOGV("%s RATE handle=%p alsa_negotiated=%uHz (0=unavailable)", tag, (void *)pcm, r);
    }

    const snd_pcm_sframes_t rc    = g_real_writei(pcm, buffer, size);

    long fill_fr = -1, fill_ms = -1;
    if (avail >= 0 && buf > 0) {
        fill_fr = static_cast<long>(buf) - static_cast<long>(avail);
        if (fill_fr < 0) fill_fr = 0;
        fill_ms = fill_fr * 1000L / static_cast<long>(rate ? rate : 16000);
    }
    if (rc < 0)
        LOGW("%s writei #%u req=%lu avail=%ld fill=%ldfr/%ldms pk=%ld mabs=%ld rc=%ld XRUN",
             tag, idx, (unsigned long)size, (long)avail, fill_fr, fill_ms, pk, mabs, (long)rc);
    else if (idx < kHeadWrites)
        LOGV("%s writei #%u req=%lu avail=%ld fill=%ldfr/%ldms pk=%ld mabs=%ld rc=%ld",
             tag, idx, (unsigned long)size, (long)avail, fill_fr, fill_ms, pk, mabs, (long)rc);

    return rc;
}
#endif
