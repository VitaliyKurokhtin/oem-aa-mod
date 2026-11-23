// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Shared runtime configuration read from a `<key>=<value>` file named
// `libpatch.conf`, located in the same on-device folder as the
// libpatch-<name>.so that reads it. The folder is discovered at runtime
// via dladdr() on one of our own functions — it is wherever the dynamic
// linker loaded our shim from (e.g. /data_persist/oem-aa-mod), NOT the
// OEM library we patch. So the config simply travels next to the
// deployed .so, with no path hardcoded anywhere.
//
// One schema, split across TWO deployed files (both live in the same
// on-device folder as the .so libraries):
//   * libpatch.conf         — the Android Auto family: blmjciaapa, svcjcinavi,
//                             aap_service, plus svcjcimmui (Audio Off fallback).
//   * libpatch-carplay.conf — blmjcicarplay (the CarPlay -> HUD bridge).
// Each library passes the filename it wants to load() (the AA family uses the
// default). AA and CarPlay run in DIFFERENT processes, so each reads only its
// own file into its own copy of the settings. This struct still defines the
// FULL schema for the family; a library acts on the keys it cares about and
// ignores the rest (e.g. svcjcinavi has no HUD transport, so it never reads
// hud_transport). Both files, and every key, are optional; anything unset
// keeps its default.
//
// Recognised keys:
//   touch         = true|false        enable the AA touch-input shim   (default true)
//   hud           = true|false        enable HUD guidance forwarding   (default true)
//   hud_transport = svcnavi|vbs       which HUD backend to use          (default svcnavi)
//   force_street_name = true|false    rewrite the HUD street strip with the AAP
//                                     street even where the OEM blanks it (default false)
//   hud_fold_latin = true|false       fold HUD-unrenderable precomposed Latin
//                                     street-name letters to their base forms (default true)
//   use_protocol_v1_6 = true|false    advertise Android Auto GAL 1.6 so the phone sends the
//                                     1.6 navigation protocol (maneuver / lanes / distance)
//                                     instead of the 1.5 turn events; read by aap_service
//                                     (default false = stock 1.5)
//   aa_audio_low_latency = true|false lower Android Auto playback's ALSA start threshold
//                                     from the full buffer to one negotiated period;
//                                     read by aap_service (default false)
//   mute_pauses_phone = true|false    on a user mute, send Android Auto a media PAUSE (and a
//                                     media PLAY on unmute) so the phone stops streaming while
//                                     muted instead of only silencing the amp (default true)
//   block_headunit_media_play = true|false
//                                     block head-unit-generated Android Auto MEDIA_PLAY events
//                                     and startup audio restoration (default false)
//   fallback_audio_off = true|false   when CarPlay/AA/USB disconnect (or reconnect timer
//                                     expires) would fall back to FM/AM/SiriusXM, select
//                                     stock Audio Off (NONE) instead; read by svcjcimmui
//                                     (default false)
//   carplay_vn_normalize = true|false fold Vietnamese 3-byte UTF-8 street-name letters to the
//                                     2-byte/ASCII forms the HUD font can render; read by
//                                     blmjcicarplay (default true)
//   carplay_speed_limit  = true|false read + display the posted speed limit on the HUD; read by
//                                     blmjcicarplay (default false = nav-only)
//   carplay_udpd_launch  = true|false auto-launch the shared splim_udpd listener; read by
//                                     blmjcicarplay, only acts when carplay_speed_limit = true
//                                     (default true)
//   carplay_seriald_launch = true|false auto-launch splim_seriald (ESP32 USB CDC → splim);
//                                     read by blmjcicarplay, only acts when
//                                     carplay_speed_limit = true (default false). Prefer a
//                                     single writer of /data_persist/splim — set
//                                     carplay_udpd_launch=false when enabling this.
//   carplay_nav_diag     = true|false dump every decoded nav maneuver/guidance payload to
//                                     /data_persist/nav_diag.log (RE aid; LOGS LOCATION DATA —
//                                     read by blmjcicarplay, default false)
//   carplay_msgtype_diag = true|false dump every devmgr message (type/size + small-msg peek) to
//                                     /data_persist/msgtype_diag.log (RE aid; LOGS LOCATION DATA —
//                                     read by blmjcicarplay, default false)
//   carplay_hud_debug    = true|false overlay raw turn codes on the HUD street strip when no
//                                     glyph maps (developer aid; read by blmjcicarplay,
//                                     default false)
//
// Booleans are lenient (true/1/yes/on, false/0/no/off). hud_transport
// also accepts "svcjcinavi" as an alias for "svcnavi".
//
// Header-only on purpose: the common/ tree carries no .cpp and the
// Makefile compiles only patches/<name>/*.cpp, so shared code lives in
// inline functions here (same convention as preload.h / preload_guard.h).
// State is held in a function-local static (see settings()).
//
// Logging: requires the including patch's own log.h (which sets
// LIBPATCH_NAME) to have been included first, exactly like
// preload_guard.h. _GNU_SOURCE must be defined before the first system
// header in the including TU for dladdr().

#ifndef LIBPATCH_COMMON_CONFIG_H
#define LIBPATCH_COMMON_CONFIG_H

// The including patch already supplies the LOGx family (via its own log.h /
// patch.h). Only pull in the shared logger as a fallback when none is defined
// yet — so a patch that logs through a DIFFERENT sink (e.g. blmjcicarplay,
// whose patch.h routes LOGx to a /tmp file) is not clobbered by a second,
// conflicting LOG_EMIT/LOG_DROP definition.
#ifndef LOGD
#include "log.h"
#endif
#include <dlfcn.h>
#include <limits.h>    // PATH_MAX
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp

// Pin the subsystem tag for the inline logging below to "CONFIG",
// independent of whatever LOG_TAG the including TU is using, so the
// inline function bodies are identical across translation units
// (ODR-safe). Restored at end of header.
#pragma push_macro("LOG_TAG")
#undef LOG_TAG
#define LOG_TAG "CONFIG"

namespace libpatch_config {

constexpr const char *kConfigFile        = "libpatch.conf";          // Android Auto family
constexpr const char *kCarplayConfigFile = "libpatch-carplay.conf";  // blmjcicarplay

// HUD output transport. Both backends are compiled into blmjciaapa; the
// active one is chosen from `hud_transport`. svcnavi routes through the
// OEM svcjcinavi service (needs the nav SD card); vbs writes the HUD
// frame directly to com.jci.vbs.navi (works cardless).
enum HudTransport {
    HUD_TRANSPORT_VBS     = 0,
    HUD_TRANSPORT_SVCNAVI = 1,
};

// === File plumbing ============================================

// Build the path of a file that sits in the SAME directory as the
// shared object containing `sym_in_self`. Pass the address of any
// function defined in your own library, so dladdr resolves to your
// .so's deployed location (not a library you merely link or dlopen).
// Writes "<dir-of-so>/<filename>" into `out`. Returns false if the
// owning object can't be resolved or the result won't fit.
inline bool find_sibling_file(const void *sym_in_self, const char *filename,
                              char *out, size_t out_sz)
{
    Dl_info info;
    if (dladdr(sym_in_self, &info) == 0 || info.dli_fname == nullptr) {
        return false;
    }

    const char *path  = info.dli_fname;
    const char *slash = strrchr(path, '/');
    size_t dir_len    = slash ? static_cast<size_t>(slash - path) : 0;

    // "<dir>" + "/" + filename + NUL
    if (dir_len + 1 + strlen(filename) + 1 > out_sz) {
        return false;
    }

    if (dir_len > 0) {
        memcpy(out, path, dir_len);
        out[dir_len] = '/';
        strcpy(out + dir_len + 1, filename);
    } else {
        // Loaded by bare name with no directory component — fall back
        // to a cwd-relative lookup.
        strcpy(out, filename);
    }
    return true;
}

// Parse a `<key>=<value>` file. Rules:
//   * lines whose first non-blank char is '#' are comments (ignored)
//   * blank / whitespace-only lines are ignored
//   * leading/trailing whitespace around key and value is trimmed
//   * a line with no '=' is ignored
// For each valid pair `cb(key, val, ud)` is invoked. Returns false if
// the file could not be opened (caller should keep its defaults).
inline bool parse_file(const char *path,
                       void (*cb)(const char *key, const char *val, void *ud),
                       void *ud)
{
    FILE *f = fopen(path, "re");   // 'e' == O_CLOEXEC (glibc)
    if (f == nullptr) {
        return false;
    }

    char line[256];
    while (fgets(line, sizeof(line), f) != nullptr) {
        // Strip trailing newline / carriage return.
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }

        // Skip leading whitespace; ignore blanks and comments.
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '\0' || *p == '#') {
            continue;
        }

        char *eq = strchr(p, '=');
        if (eq == nullptr) {
            continue;   // not a key=value line
        }
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        // Trim trailing whitespace off the key.
        char *ke = eq;
        while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) {
            --ke;
        }
        *ke = '\0';

        // Trim surrounding whitespace off the value.
        while (*val == ' ' || *val == '\t') {
            ++val;
        }
        char *ve = val + strlen(val);
        while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t')) {
            --ve;
        }
        *ve = '\0';

        if (*key == '\0') {
            continue;
        }
        cb(key, val, ud);
    }

    fclose(f);
    return true;
}

// Lenient boolean parse. Case-insensitive:
//   true  / 1 / yes / on  -> true
//   false / 0 / no  / off -> false
//   anything else         -> deflt
inline bool parse_bool(const char *val, bool deflt)
{
    if (val == nullptr) {
        return deflt;
    }
    if (strcasecmp(val, "true") == 0 || strcasecmp(val, "1") == 0 ||
        strcasecmp(val, "yes")  == 0 || strcasecmp(val, "on") == 0) {
        return true;
    }
    if (strcasecmp(val, "false") == 0 || strcasecmp(val, "0") == 0 ||
        strcasecmp(val, "no")    == 0 || strcasecmp(val, "off") == 0) {
        return false;
    }
    return deflt;
}

// === Schema + state ===========================================

struct Settings {
    bool         touch                 = true;
    bool         hud                   = true;
    HudTransport hud_transport         = HUD_TRANSPORT_SVCNAVI;
    bool         force_street_name     = false;
    bool         hud_fold_latin        = true;
    bool         use_protocol_v1_6     = false;
    bool         aa_audio_low_latency  = false;
    bool         mute_pauses_phone     = true;
    bool         block_headunit_media_play = false;
    bool         fallback_audio_off = false;    // svcjcimmui: Audio Off instead of FM on disconnect
    bool         carplay_vn_normalize = true;   // blmjcicarplay: fold VN 3-byte street names for the HUD font
    bool         carplay_speed_limit  = false;  // blmjcicarplay: read + show the posted speed limit
    bool         carplay_udpd_launch  = true;   // blmjcicarplay: auto-launch splim_udpd (only when speed_limit)
    bool         carplay_seriald_launch = false; // blmjcicarplay: auto-launch splim_seriald (ESP32 CDC)
    bool         carplay_nav_diag     = false;  // blmjcicarplay: dump nav payloads to disk (RE aid; location data)
    bool         carplay_msgtype_diag = false;  // blmjcicarplay: dump devmgr msg types to disk (RE aid; location data)
    bool         carplay_hud_debug    = false;  // blmjcicarplay: overlay raw turn codes on the HUD street strip
    bool         loaded               = false;
};

// The single parsed-config instance for this library. Function-local
// static: lazily constructed on first use, no .cpp needed.
inline Settings &settings()
{
    static Settings s;
    return s;
}

inline const char *transport_name(HudTransport t)
{
    return t == HUD_TRANSPORT_SVCNAVI ? "svcnavi" : "vbs";
}

// Per-key handler invoked by parse_file. `ud` is the Settings being
// populated.
inline void apply_kv(const char *key, const char *val, void *ud)
{
    Settings &s = *static_cast<Settings *>(ud);

    if (strcasecmp(key, "touch") == 0) {
        s.touch = parse_bool(val, s.touch);
    } else if (strcasecmp(key, "hud") == 0) {
        s.hud = parse_bool(val, s.hud);
    } else if (strcasecmp(key, "hud_transport") == 0) {
        if (strcasecmp(val, "svcnavi") == 0 ||
            strcasecmp(val, "svcjcinavi") == 0) {
            s.hud_transport = HUD_TRANSPORT_SVCNAVI;
        } else if (strcasecmp(val, "vbs") == 0) {
            s.hud_transport = HUD_TRANSPORT_VBS;
        } else {
            LOGW("config: unknown hud_transport=\"%s\" — keeping %s",
                 val, transport_name(s.hud_transport));
        }
    } else if (strcasecmp(key, "force_street_name") == 0) {
        s.force_street_name = parse_bool(val, s.force_street_name);
    } else if (strcasecmp(key, "hud_fold_latin") == 0) {
        s.hud_fold_latin = parse_bool(val, s.hud_fold_latin);
    } else if (strcasecmp(key, "use_protocol_v1_6") == 0) {
        s.use_protocol_v1_6 = parse_bool(val, s.use_protocol_v1_6);
    } else if (strcasecmp(key, "aa_audio_low_latency") == 0) {
        s.aa_audio_low_latency = parse_bool(val, s.aa_audio_low_latency);
    } else if (strcasecmp(key, "mute_pauses_phone") == 0) {
        s.mute_pauses_phone = parse_bool(val, s.mute_pauses_phone);
    } else if (strcasecmp(key, "block_headunit_media_play") == 0) {
        s.block_headunit_media_play =
            parse_bool(val, s.block_headunit_media_play);
    } else if (strcasecmp(key, "fallback_audio_off") == 0) {
        s.fallback_audio_off = parse_bool(val, s.fallback_audio_off);
    } else if (strcasecmp(key, "carplay_vn_normalize") == 0) {
        s.carplay_vn_normalize = parse_bool(val, s.carplay_vn_normalize);
    } else if (strcasecmp(key, "carplay_speed_limit") == 0) {
        s.carplay_speed_limit = parse_bool(val, s.carplay_speed_limit);
    } else if (strcasecmp(key, "carplay_udpd_launch") == 0) {
        s.carplay_udpd_launch = parse_bool(val, s.carplay_udpd_launch);
    } else if (strcasecmp(key, "carplay_seriald_launch") == 0) {
        s.carplay_seriald_launch = parse_bool(val, s.carplay_seriald_launch);
    } else if (strcasecmp(key, "carplay_nav_diag") == 0) {
        s.carplay_nav_diag = parse_bool(val, s.carplay_nav_diag);
    } else if (strcasecmp(key, "carplay_msgtype_diag") == 0) {
        s.carplay_msgtype_diag = parse_bool(val, s.carplay_msgtype_diag);
    } else if (strcasecmp(key, "carplay_hud_debug") == 0) {
        s.carplay_hud_debug = parse_bool(val, s.carplay_hud_debug);
    } else {
        // Common schema: a key this library doesn't act on is not an
        // error, just informational.
        LOGD("config: key \"%s\" not used by this library", key);
    }
}

inline void log_effective(const char *prefix)
{
    const Settings &s = settings();
    LOGD("config: %s touch=%s hud=%s hud_transport=%s force_street_name=%s "
         "hud_fold_latin=%s use_protocol_v1_6=%s aa_audio_low_latency=%s "
         "mute_pauses_phone=%s block_headunit_media_play=%s "
         "fallback_audio_off=%s "
         "carplay_vn_normalize=%s carplay_speed_limit=%s carplay_udpd_launch=%s "
         "carplay_seriald_launch=%s "
         "carplay_nav_diag=%s carplay_msgtype_diag=%s carplay_hud_debug=%s",
         prefix,
         s.touch ? "true" : "false",
         s.hud   ? "true" : "false",
         transport_name(s.hud_transport),
         s.force_street_name ? "true" : "false",
         s.hud_fold_latin ? "true" : "false",
         s.use_protocol_v1_6 ? "true" : "false",
         s.aa_audio_low_latency ? "true" : "false",
         s.mute_pauses_phone ? "true" : "false",
         s.block_headunit_media_play ? "true" : "false",
         s.fallback_audio_off ? "true" : "false",
         s.carplay_vn_normalize ? "true" : "false",
         s.carplay_speed_limit ? "true" : "false",
         s.carplay_udpd_launch ? "true" : "false",
         s.carplay_seriald_launch ? "true" : "false",
         s.carplay_nav_diag ? "true" : "false",
         s.carplay_msgtype_diag ? "true" : "false",
         s.carplay_hud_debug ? "true" : "false");
}

// === Public API ===============================================

// Load the config file `filename` from the directory of the library that owns
// `sym_in_self` (pass the address of any function in your own .so). The AA
// family uses the default (libpatch.conf); blmjcicarplay passes
// kCarplayConfigFile. Idempotent: only the first call reads the file. Missing
// file or keys fall back to the defaults above.
inline void load(const void *sym_in_self, const char *filename = kConfigFile)
{
    Settings &s = settings();
    if (s.loaded) {
        return;
    }
    s.loaded = true;

    char path[PATH_MAX];
    if (!find_sibling_file(sym_in_self, filename, path, sizeof(path))) {
        LOGW("config: could not resolve own .so directory — using defaults");
        log_effective("defaults:");
        return;
    }

    if (!parse_file(path, &apply_kv, &s)) {
        LOGD("config: %s not present — using defaults", path);
        log_effective("defaults:");
        return;
    }

    LOGD("config: loaded %s", path);
    log_effective("effective:");
}

// Accessors — valid any time; return the compiled-in defaults before
// load() has run, the file's values after.
inline bool         touch_enabled()  { return settings().touch; }
inline bool         hud_enabled()    { return settings().hud; }
inline HudTransport hud_transport()  { return settings().hud_transport; }
inline bool         force_street_name() { return settings().force_street_name; }
inline bool         hud_fold_latin() { return settings().hud_fold_latin; }
inline bool         use_protocol_v1_6() { return settings().use_protocol_v1_6; }
inline bool         aa_audio_low_latency() { return settings().aa_audio_low_latency; }
inline bool         mute_pauses_phone() { return settings().mute_pauses_phone; }
inline bool         block_headunit_media_play() { return settings().block_headunit_media_play; }
inline bool         fallback_audio_off() { return settings().fallback_audio_off; }
inline bool         carplay_vn_normalize() { return settings().carplay_vn_normalize; }
inline bool         carplay_speed_limit()  { return settings().carplay_speed_limit; }
inline bool         carplay_udpd_launch()  { return settings().carplay_udpd_launch; }
inline bool         carplay_seriald_launch() { return settings().carplay_seriald_launch; }
inline bool         carplay_nav_diag()     { return settings().carplay_nav_diag; }
inline bool         carplay_msgtype_diag() { return settings().carplay_msgtype_diag; }
inline bool         carplay_hud_debug()    { return settings().carplay_hud_debug; }

} // namespace libpatch_config

#pragma pop_macro("LOG_TAG")

#endif  // LIBPATCH_COMMON_CONFIG_H
