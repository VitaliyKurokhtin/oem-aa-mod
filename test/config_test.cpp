// SPDX-License-Identifier: AGPL-3.0-or-later
//
// config_test — host self-test for the config.h inline parser functions.
// Exercises parse_nonnegative_meters with the full range of inputs:
// disable keywords, plain decimals, unit suffixes, leading non-digits,
// clamping (min 100 / max 1 000 000), null/empty, and the documented
// thousand-separator trap.  Returns non-zero on any failed assertion so
// CI can gate on it.  Pure host build (no ARM sysroot).

#include "common/config.h"

#include <cstdio>
#include <cstdint>

static int g_fail = 0;

#define CHECK(cond, msg) do {                                            \
        if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail = 1; }      \
        else         { printf("  ok:   %s\n", (msg)); }                  \
    } while (0)

#define CHECK_EQ_U(got, exp, msg) do {                                   \
        unsigned long _g = (unsigned long)(got), _e = (unsigned long)(exp); \
        if (_g != _e) { printf("  FAIL: %s (got %lu, want %lu)\n",       \
                               (msg), _g, _e); g_fail = 1; }             \
        else          { printf("  ok:   %s = %lu\n", (msg), _g); }       \
    } while (0)

#define CHECK_EQ_S(got, exp, msg) do {                                   \
        if (strcmp((got), (exp)) != 0) {                                 \
            printf("  FAIL: %s (got \"%s\", want \"%s\")\n",             \
                   (msg), (got), (exp)); g_fail = 1; }                   \
        else { printf("  ok:   %s = \"%s\"\n", (msg), (got)); }          \
    } while (0)

// Sentinel deflt that is distinct from any valid gate value.
static const uint32_t kDeflt = 9999;

int main()
{
    // [1] Disable keywords and numeric zero ------------------------------------
    // "0", false/no/off (case-insensitive) all return 0 (filter disabled).
    {
        printf("[1] disable keywords\n");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("0",     kDeflt), 0, "\"0\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("false", kDeflt), 0, "\"false\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("FALSE", kDeflt), 0, "\"FALSE\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("False", kDeflt), 0, "\"False\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("no",    kDeflt), 0, "\"no\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("NO",    kDeflt), 0, "\"NO\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("off",   kDeflt), 0, "\"off\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("OFF",   kDeflt), 0, "\"OFF\"");
    }

    // [2] Valid plain decimals within [100, 1 000 000] ------------------------
    {
        printf("[2] valid plain decimals\n");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("100",     kDeflt), 100,     "\"100\" (minimum)");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("5000",    kDeflt), 5000,    "\"5000\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("1000000", kDeflt), 1000000, "\"1000000\" (maximum)");
    }

    // [3] Unit suffixes — suffix is ignored, number is always treated as meters
    // "5km" means 5 metres (just the digit 5), not 5000.
    {
        printf("[3] unit suffixes (no conversion)\n");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("5000m",      kDeflt), 5000, "\"5000m\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("5000 m",     kDeflt), 5000, "\"5000 m\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("5000M",      kDeflt), 5000, "\"5000M\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("5000 meters", kDeflt), 5000, "\"5000 meters\"");
        // "5km" parses as 5 (strtoul stops at 'k'); 5 < min 100 -> clamped.
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("5km",        kDeflt), 100, "\"5km\" -> 5 -> clamped to 100 (no unit conversion)");
    }

    // [4] Leading non-digits skipped by strpbrk --------------------------------
    // A minus sign, whitespace, or any leading letter is ignored; the first
    // digit run is what gets parsed.  This is the fix for the PR bug where
    // " -5000" previously wrapped through strtoul to a huge unsigned value.
    {
        printf("[4] leading non-digits skipped\n");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("-5000",   kDeflt), 5000, "\"-5000\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters(" -5000",  kDeflt), 5000, "\" -5000\" (PR bug)");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("  5000",  kDeflt), 5000, "\"  5000\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("- 5000",  kDeflt), 5000, "\"- 5000\"");
    }

    // [5] Clamping -------------------------------------------------------------
    // Non-zero values below 100 are clamped up to 100.
    // Values above 1 000 000 are clamped down to 1 000 000.
    {
        printf("[5] clamping (min 100, max 1000000)\n");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("1",       kDeflt), 100,     "\"1\" -> clamped to 100");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("99",      kDeflt), 100,     "\"99\" -> clamped to 100");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("1000001", kDeflt), 1000000, "\"1000001\" -> clamped to 1000000");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("9999999", kDeflt), 1000000, "\"9999999\" -> clamped to 1000000");
    }

    // [6] No digits found -> deflt ---------------------------------------------
    {
        printf("[6] no digits -> deflt\n");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("",      kDeflt), kDeflt, "\"\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters(nullptr, kDeflt), kDeflt, "nullptr");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("abc",   kDeflt), kDeflt, "\"abc\"");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("m",     kDeflt), kDeflt, "\"m\"");
    }

    // [7] Thousand-separator trap (characterization) ---------------------------
    // ',' and '.' are NOT treated as separators: strtoul stops at them, so
    // "1,000" parses as 1 -> clamped to 100.  This documents the known
    // limitation described in the config.h comment; use plain digits instead.
    {
        printf("[7] thousand-separator trap (documented behavior)\n");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("1,000", kDeflt), 100, "\"1,000\" -> 1 -> clamped to 100");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("1.000", kDeflt), 100, "\"1.000\" -> 1 -> clamped to 100");
        CHECK_EQ_U(libpatch_config::parse_nonnegative_meters("1 000", kDeflt), 100, "\"1 000\" -> 1 -> clamped to 100");
    }

    // [8] parse_bool -----------------------------------------------------------
    // true  tokens: true / 1 / yes / on  (case-insensitive)
    // false tokens: false / 0 / no / off (case-insensitive)
    // anything else (including nullptr and empty string) -> deflt
    {
        printf("[8] parse_bool\n");

        // true tokens
        CHECK(libpatch_config::parse_bool("true",  false) == true,  "\"true\"");
        CHECK(libpatch_config::parse_bool("TRUE",  false) == true,  "\"TRUE\"");
        CHECK(libpatch_config::parse_bool("True",  false) == true,  "\"True\"");
        CHECK(libpatch_config::parse_bool("1",     false) == true,  "\"1\"");
        CHECK(libpatch_config::parse_bool("yes",   false) == true,  "\"yes\"");
        CHECK(libpatch_config::parse_bool("YES",   false) == true,  "\"YES\"");
        CHECK(libpatch_config::parse_bool("on",    false) == true,  "\"on\"");
        CHECK(libpatch_config::parse_bool("ON",    false) == true,  "\"ON\"");

        // false tokens
        CHECK(libpatch_config::parse_bool("false", true) == false, "\"false\"");
        CHECK(libpatch_config::parse_bool("FALSE", true) == false, "\"FALSE\"");
        CHECK(libpatch_config::parse_bool("0",     true) == false, "\"0\"");
        CHECK(libpatch_config::parse_bool("no",    true) == false, "\"no\"");
        CHECK(libpatch_config::parse_bool("NO",    true) == false, "\"NO\"");
        CHECK(libpatch_config::parse_bool("off",   true) == false, "\"off\"");
        CHECK(libpatch_config::parse_bool("OFF",   true) == false, "\"OFF\"");

        // unknown / null / empty -> deflt
        CHECK(libpatch_config::parse_bool(nullptr, true)  == true,  "nullptr -> deflt=true");
        CHECK(libpatch_config::parse_bool(nullptr, false) == false, "nullptr -> deflt=false");
        CHECK(libpatch_config::parse_bool("",      true)  == true,  "\"\" -> deflt=true");
        CHECK(libpatch_config::parse_bool("maybe", true)  == true,  "\"maybe\" -> deflt=true");
        CHECK(libpatch_config::parse_bool("2",     false) == false, "\"2\" -> deflt=false");
    }

    // [9] apply_kv — key dispatch, parser wiring, transport_name -------------
    // apply_kv takes a stack Settings via void*ud — no global singleton touched.
    // transport_name is exercised inline to verify round-trip.
    {
        printf("[9] apply_kv — key dispatch\n");

        // hud_transport: all accepted aliases + transport_name round-trip
        {
            libpatch_config::Settings s;
            libpatch_config::apply_kv("hud_transport", "vbs", &s);
            CHECK(s.hud_transport == libpatch_config::HUD_TRANSPORT_VBS, "hud_transport=vbs");
            CHECK_EQ_S(libpatch_config::transport_name(s.hud_transport), "vbs", "transport_name(VBS)");

            libpatch_config::apply_kv("hud_transport", "svcnavi", &s);
            CHECK(s.hud_transport == libpatch_config::HUD_TRANSPORT_SVCNAVI, "hud_transport=svcnavi");
            CHECK_EQ_S(libpatch_config::transport_name(s.hud_transport), "svcnavi", "transport_name(SVCNAVI)");

            libpatch_config::apply_kv("hud_transport", "svcjcinavi", &s);
            CHECK(s.hud_transport == libpatch_config::HUD_TRANSPORT_SVCNAVI, "hud_transport=svcjcinavi alias");

            libpatch_config::apply_kv("hud_transport", "VBS", &s);
            CHECK(s.hud_transport == libpatch_config::HUD_TRANSPORT_VBS, "hud_transport=VBS (value case)");
        }

        // hud_transport: unknown value keeps previous setting
        {
            libpatch_config::Settings s;
            libpatch_config::apply_kv("hud_transport", "vbs", &s);
            libpatch_config::apply_kv("hud_transport", "bogus", &s);
            CHECK(s.hud_transport == libpatch_config::HUD_TRANSPORT_VBS, "unknown transport keeps previous");
        }

        // hud_maneuver_max_distance_m wiring (new key introduced by this branch)
        {
            libpatch_config::Settings s;
            libpatch_config::apply_kv("hud_maneuver_max_distance_m", "5000", &s);
            CHECK_EQ_U(s.hud_maneuver_max_distance_m, 5000, "hud_maneuver_max_distance_m=5000");

            libpatch_config::apply_kv("hud_maneuver_max_distance_m", "0", &s);
            CHECK_EQ_U(s.hud_maneuver_max_distance_m, 0, "hud_maneuver_max_distance_m=0 (disable)");

            libpatch_config::apply_kv("hud_maneuver_max_distance_m", "false", &s);
            CHECK_EQ_U(s.hud_maneuver_max_distance_m, 0, "hud_maneuver_max_distance_m=false (disable)");

            libpatch_config::apply_kv("hud_maneuver_max_distance_m", "5000m", &s);
            CHECK_EQ_U(s.hud_maneuver_max_distance_m, 5000, "hud_maneuver_max_distance_m=5000m (suffix)");
        }

        // boolean key spot-checks (parse_bool wiring; not exhaustive — [8] covers that)
        {
            libpatch_config::Settings s;
            libpatch_config::apply_kv("touch", "false", &s);
            CHECK(!s.touch, "touch=false");

            libpatch_config::apply_kv("hud_fold_latin", "false", &s);
            CHECK(!s.hud_fold_latin, "hud_fold_latin=false");

            libpatch_config::apply_kv("hud_fold_latin", "true", &s);
            CHECK(s.hud_fold_latin, "hud_fold_latin=true");
        }

        // key names are case-insensitive
        {
            libpatch_config::Settings s;
            libpatch_config::apply_kv("HUD_FOLD_LATIN", "false", &s);
            CHECK(!s.hud_fold_latin, "HUD_FOLD_LATIN (key case)");

            libpatch_config::apply_kv("Hud_Transport", "vbs", &s);
            CHECK(s.hud_transport == libpatch_config::HUD_TRANSPORT_VBS, "Hud_Transport (key case)");

            libpatch_config::apply_kv("HUD_MANEUVER_MAX_DISTANCE_M", "2000", &s);
            CHECK_EQ_U(s.hud_maneuver_max_distance_m, 2000, "HUD_MANEUVER_MAX_DISTANCE_M (key case)");
        }

        // unknown key must not crash and must leave all settings unchanged
        {
            libpatch_config::Settings s;
            libpatch_config::apply_kv("hud_maneuver_max_distance_m", "3000", &s);
            libpatch_config::apply_kv("nonexistent_key", "anything", &s);
            CHECK_EQ_U(s.hud_maneuver_max_distance_m, 3000, "unknown key leaves settings unchanged");
            CHECK(s.hud_fold_latin, "unknown key leaves other fields unchanged");
        }
    }

    printf("\n%s\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS");
    return g_fail;
}
