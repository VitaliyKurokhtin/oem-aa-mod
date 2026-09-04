// SPDX-License-Identifier: AGPL-3.0-or-later
//
// nav16_test — host self-test for the AA GAL 1.6 nav decoder (hud_nav16).
// Feeds hand-built synthetic 0x8006/0x8007/0x8003 frames through the PUBLIC API
// and asserts the decoded guidance/position/glyph/units/status, then drives the
// same frames through the push path (hud_nav16_feed -> sink) and asserts the
// right callback fires. Returns non-zero on any failed assertion so CI can gate
// on it. Pure host build (no ARM sysroot).

#include "hud_nav16.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;

// Parse a space-separated hex string ("80 06 0a ...") into a byte vector.
static std::vector<uint8_t> hx(const char *s)
{
    std::vector<uint8_t> v;
    unsigned b;
    while (*s) {
        if (sscanf(s, "%2x", &b) == 1) v.push_back((uint8_t)b);
        s += (s[1] ? 2 : 1);
        while (*s == ' ') ++s;
    }
    return v;
}

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

int main()
{
    char buf[512];

    // --- DEPART, road "ROAD A", no lanes -------------------------------------
    // expect maneuver=1 (DEPART) glyph=HUD_FLAG(12) road="ROAD A" lanes=0
    {
        printf("[1] DEPART, road \"ROAD A\", no lanes\n");
        auto s = hx("80 06 0a 0e 0a 02 08 01 12 08 0a 06 52 4f 41 44 20 41");
        AaGuidance g;
        uint32_t id = hud_nav16_on_frame(s.data(), (int)s.size(), &g, nullptr);
        hud_nav16_format_guidance(&g, buf, sizeof(buf));
        printf("  %s\n", buf);
        CHECK_EQ_U(id, 0x8006, "msgId");
        CHECK_EQ_U(g.maneuver_type, 1u, "maneuver_type");
        CHECK_EQ_U(hud_nav16_glyph(&g), 12u, "glyph (HUD_FLAG)");
        CHECK_EQ_S(g.road, "ROAD A", "road");
        CHECK_EQ_U(g.n_lanes, 0, "n_lanes");
    }

    // --- CurrentPosition: step 120 m "120", dest 1500 m "1,5" km, eta "12:34" -
    {
        printf("[2] CurrentPosition: step 120m, dest 1500m km, eta\n");
        auto pz = hx("80 07 0a 0b 0a 09 08 78 12 03 31 32 30 18 01 12 13 0a 0a 08 dc 0b 12 03 31 2c 35 18 03 12 05 31 32 3a 33 34");
        AaPosition p;
        uint32_t id = hud_nav16_on_frame(pz.data(), (int)pz.size(), nullptr, &p);
        hud_nav16_format_position(&p, buf, sizeof(buf));
        printf("  %s\n", buf);
        CHECK_EQ_U(id, 0x8007, "msgId");
        CHECK(p.have_step, "have_step");
        CHECK_EQ_U(p.step_meters, 120, "step_meters");
        CHECK_EQ_S(p.step_display, "120", "step_display");
        CHECK_EQ_U(p.step_units, 1u, "step_units (METERS)");
        CHECK_EQ_U(aa_to_mazda_unit(p.step_units), 1u, "mazda step unit");
        CHECK(p.have_dest, "have_dest");
        CHECK_EQ_U(p.dest_meters, 1500, "dest_meters");
        CHECK_EQ_S(p.dest_display, "1,5", "dest_display");
        CHECK_EQ_U(p.dest_units, 3u, "dest_units (KILOMETERS)");
        CHECK_EQ_U(aa_to_mazda_unit(p.dest_units), 3u, "mazda dest unit");
        CHECK_EQ_S(p.eta, "12:34", "eta");
        CHECK_EQ_U(parse_dist_x10(p.dest_display), 15, "parse_dist_x10(\"1,5\")");
    }

    // --- Junction WITH lanes ------------------------------------------------
    // maneuver=8 TURN_NORMAL_RIGHT -> glyph=HUD_RIGHT(3), road "ROAD B",
    // lane0=[STRAIGHT, NORMAL_RIGHT*hl], lane1=[NORMAL_RIGHT*hl]
    //   expect L0 pres=0x022 hi=0x020, L1 pres=0x020 hi=0x020
    {
        printf("[3] Junction with lanes (TURN_NORMAL_RIGHT)\n");
        auto j = hx("80 06 0a 22 0a 02 08 08 12 08 0a 06 52 4f 41 44 20 42 1a 0a 0a 02 08 01 0a 04 08 05 10 01 1a 06 0a 04 08 05 10 01");
        AaGuidance g;
        uint32_t id = hud_nav16_on_frame(j.data(), (int)j.size(), &g, nullptr);
        hud_nav16_format_guidance(&g, buf, sizeof(buf));
        printf("  %s\n", buf);
        CHECK_EQ_U(id, 0x8006, "msgId");
        CHECK_EQ_U(g.maneuver_type, 8u, "maneuver_type");
        CHECK_EQ_U(hud_nav16_glyph(&g), 3u, "glyph (HUD_RIGHT)");
        CHECK_EQ_S(g.road, "ROAD B", "road");
        CHECK_EQ_U(g.n_lanes, 2, "n_lanes");
        CHECK_EQ_U(g.lanes[0].present_mask, 0x022u, "L0 present_mask");
        CHECK_EQ_U(g.lanes[0].highlight_mask, 0x020u, "L0 highlight_mask");
        CHECK_EQ_U(g.lanes[1].present_mask, 0x020u, "L1 present_mask");
        CHECK_EQ_U(g.lanes[1].highlight_mask, 0x020u, "L1 highlight_mask");
    }

    // --- Roundabout: maneuver=34 RA_ENTER_EXIT_CCW, exit angle 90 ------------
    //   CCW = right-hand traffic -> base 37 + round(90/30)=3 -> glyph 40
    {
        printf("[4] Roundabout RA_ENTER_EXIT_CCW, angle 90\n");
        auto r = hx("80 06 0a 06 0a 04 08 22 18 5a");
        AaGuidance g;
        uint32_t id = hud_nav16_on_frame(r.data(), (int)r.size(), &g, nullptr);
        hud_nav16_format_guidance(&g, buf, sizeof(buf));
        printf("  %s\n", buf);
        CHECK_EQ_U(id, 0x8006, "msgId");
        CHECK_EQ_U(g.maneuver_type, 34u, "maneuver_type");
        CHECK_EQ_U(g.roundabout_exit_angle, 90, "roundabout_exit_angle");
        CHECK_EQ_U(hud_nav16_glyph(&g), 40u, "glyph (roundabout 37+3)");
    }

    // --- NavigationStatus (0x8003): status field 1 varint --------------------
    //   0x8003 body: field 1 (tag 0x08) varint = 2  -> status 2
    {
        printf("[5] NavigationStatus 0x8003 field 1\n");
        auto st = hx("80 03 08 02");
        int status = 999;
        bool ok = hud_nav16_read_status(st.data(), (int)st.size(), &status);
        printf("  read_status -> ok=%d status=%d\n", ok, status);
        CHECK(ok, "read_status returned true");
        CHECK_EQ_U(status, 2, "status value");
    }

    // --- Push API: hud_nav16_feed() dispatches by msgId to the sink -----------
    // Same synthetic frames as above, but driven through the receiver-side
    // push path: register a sink, feed the raw frame, assert the right
    // callback fired with the decoded struct.
    {
        printf("[6] hud_nav16_feed -> sink dispatch\n");
        static int   n_g = 0, n_p = 0, n_s = 0;
        static char  last_road[128] = {0};
        static int   last_step = -1;
        static int   last_status = -1;
        static bool  last_cluster_stop = false;

        struct Cb {
            static void g(const AaGuidance *a) {
                ++n_g; strncpy(last_road, a->road, sizeof(last_road) - 1);
            }
            static void p(const AaPosition *a) {
                ++n_p; last_step = a->have_step ? a->step_meters : -1;
            }
            static void s(const AaStatus *a) {
                ++n_s; last_status = a->nav_status; last_cluster_stop = a->cluster_stop;
            }
        };
        hud_nav16_set_sink(&Cb::g, &Cb::p, &Cb::s);

        // 0x8006 -> on_guidance
        auto st6 = hx("80 06 0a 0e 0a 02 08 01 12 08 0a 06 52 4f 41 44 20 41");
        hud_nav16_feed(st6.data(), (int)st6.size());
        // 0x8007 -> on_position
        auto st7 = hx("80 07 0a 0b 0a 09 08 78 12 03 31 32 30 18 01 12 13 0a 0a 08 dc 0b 12 03 31 2c 35 18 03 12 05 31 32 3a 33 34");
        hud_nav16_feed(st7.data(), (int)st7.size());
        // 0x8003 -> on_status (lifecycle, status 2)
        auto st3 = hx("80 03 08 02");
        hud_nav16_feed(st3.data(), (int)st3.size());
        // 0x8002 -> on_status (cluster STOP)
        auto st2 = hx("80 02");
        hud_nav16_feed(st2.data(), (int)st2.size());
        // 0x8004 (NEXT_TURN) -> nothing rendered
        auto st4 = hx("80 04 08 01");
        hud_nav16_feed(st4.data(), (int)st4.size());

        printf("  g=%d p=%d s=%d road=\"%s\" step=%d status=%d stop=%d\n",
               n_g, n_p, n_s, last_road, last_step, last_status, last_cluster_stop);
        CHECK_EQ_U(n_g, 1, "on_guidance fired once");
        CHECK_EQ_U(n_p, 1, "on_position fired once");
        CHECK_EQ_U(n_s, 2, "on_status fired twice (0x8003 + 0x8002)");
        CHECK_EQ_S(last_road, "ROAD A", "guidance road");
        CHECK_EQ_U(last_step, 120, "position step_meters");
        CHECK(last_cluster_stop, "last status was cluster STOP");

        hud_nav16_set_sink(nullptr, nullptr, nullptr);
        // After detach, feeding must not fire callbacks.
        hud_nav16_feed(st6.data(), (int)st6.size());
        CHECK_EQ_U(n_g, 1, "no dispatch after sink detach");
    }

    // =========================================================================
    // ROUNDABOUT GLYPHS
    //
    // The roundabout maneuver types state their own circulation direction, so
    // the glyph bank is picked from the wire and never guessed:
    //   32/33 = clockwise        -> bank base 49 (left-hand traffic)
    //   34/35 = counterclockwise -> bank base 37 (right-hand traffic)
    // Within a bank the glyph is the circulation angle in 30-degree steps,
    // index = round(angle / 30); the angle increments in the driving direction
    // for BOTH banks, so the index is handedness-independent.
    // =========================================================================

    // --- REGRESSION: the angle-bearing types (33/35) --------------------------
    // These four (exit_number, angle) pairs are the ones real captures contain,
    // and this mapping is road-validated. It must not change.
    {
        printf("[7] regression: angle present -> unchanged fan-out\n");
        struct Case { const char *hex; unsigned exp_glyph; const char *msg; };
        static const Case kC[] = {
            // 0a NN = step; 0a 07 = maneuver{ type, exit_number, angle }
            {"80 06 0a 09 0a 07 08 23 10 02 18 87 01", 42, "CCW angle 135 -> 42"},
            {"80 06 0a 09 0a 07 08 23 10 02 18 b4 01", 43, "CCW angle 180 -> 43"},
            {"80 06 0a 09 0a 07 08 23 10 03 18 e1 01", 45, "CCW angle 225 -> 45"},
            {"80 06 0a 09 0a 07 08 23 10 03 18 8e 02", 46, "CCW angle 270 -> 46"},
            // the legitimate "back out the entry" U-turn: angle 360 -> index 0
            {"80 06 0a 09 0a 07 08 23 10 02 18 e8 02", 37, "CCW angle 360 -> 37 (U-turn)"},
            // clockwise bank, same index arithmetic
            {"80 06 0a 09 0a 07 08 21 10 02 18 b4 01", 55, "CW  angle 180 -> 55"},
        };
        for (unsigned i = 0; i < sizeof(kC)/sizeof(kC[0]); ++i) {
            auto s = hx(kC[i].hex);
            AaGuidance g;
            hud_nav16_on_frame(s.data(), (int)s.size(), &g, nullptr);
            CHECK_EQ_U(hud_nav16_glyph(&g), kC[i].exp_glyph, kC[i].msg);
        }
    }

    // --- REGRESSION: plain turns keep their table glyph ----------------------
    // A stale roundabout_exit_number rides along on the plain-turn steps that
    // follow a roundabout, so it must never turn a turn into a roundabout.
    {
        printf("[8] regression: plain turns unaffected by a stale exit_number\n");
        auto a = hx("80 06 0a 06 0a 04 08 08 10 02");   // TURN_NORMAL_RIGHT + exit 2
        AaGuidance g;
        hud_nav16_on_frame(a.data(), (int)a.size(), &g, nullptr);
        CHECK_EQ_U(g.maneuver_type, 8u, "maneuver_type");
        CHECK_EQ_U(hud_nav16_glyph(&g), 3u, "TURN_NORMAL_RIGHT -> HUD_RIGHT (3)");
        auto b = hx("80 06 0a 06 0a 04 08 07 10 02");   // TURN_NORMAL_LEFT + exit 2
        hud_nav16_on_frame(b.data(), (int)b.size(), &g, nullptr);
        CHECK_EQ_U(hud_nav16_glyph(&g), 2u, "TURN_NORMAL_LEFT -> HUD_LEFT (2)");
    }

    // --- NEW: angle absent -> estimate the angle from the exit number --------
    // Senders that use the non-WITH_ANGLE roundabout types (32/34) give only an
    // exit number. Exit N of an E-arm roundabout sits at 360*N/E, so with E
    // unknown the angle is estimated as the value minimising expected error
    // over the arm-count distribution: 90, 180, 270, 270, 300, 300 for N=1..6
    // (it saturates because a 4th exit implies at least 5 arms).
    {
        printf("[9] angle absent -> exit-number estimate\n");
        // real captured frame: type 34, exit 2, no angle, road+cue "422"
        auto real = hx("80 06 0a 14 0a 04 08 22 10 02"
                       " 12 05 0a 03 34 32 32 22 05 0a 03 34 32 32");
        AaGuidance g;
        hud_nav16_on_frame(real.data(), (int)real.size(), &g, nullptr);
        CHECK_EQ_U(g.maneuver_type, 34u, "maneuver_type (RA_ENTER_EXIT_CCW)");
        CHECK_EQ_U(g.roundabout_exit_number, 2, "roundabout_exit_number");
        CHECK_EQ_S(g.road, "422", "road");
        CHECK_EQ_U(hud_nav16_glyph(&g), 43u, "real frame: exit 2 -> 43 (was 37)");

        struct Case { const char *hex; unsigned exp; const char *msg; };
        static const Case kC[] = {
            {"80 06 0a 06 0a 04 08 22 10 01", 40, "CCW exit 1 ->  90deg -> 40"},
            {"80 06 0a 06 0a 04 08 22 10 02", 43, "CCW exit 2 -> 180deg -> 43"},
            {"80 06 0a 06 0a 04 08 22 10 03", 46, "CCW exit 3 -> 270deg -> 46"},
            {"80 06 0a 06 0a 04 08 22 10 04", 46, "CCW exit 4 -> 270deg -> 46"},
            {"80 06 0a 06 0a 04 08 22 10 05", 47, "CCW exit 5 -> 300deg -> 47"},
            {"80 06 0a 06 0a 04 08 22 10 06", 47, "CCW exit 6 -> 300deg -> 47"},
            {"80 06 0a 06 0a 04 08 22 10 09", 47, "CCW exit 9 -> clamped -> 47"},
            {"80 06 0a 06 0a 04 08 20 10 01", 52, "CW  exit 1 ->  90deg -> 52"},
            {"80 06 0a 06 0a 04 08 20 10 02", 55, "CW  exit 2 -> 180deg -> 55"},
            {"80 06 0a 06 0a 04 08 20 10 03", 58, "CW  exit 3 -> 270deg -> 58"},
        };
        for (unsigned i = 0; i < sizeof(kC)/sizeof(kC[0]); ++i) {
            auto s = hx(kC[i].hex);
            hud_nav16_on_frame(s.data(), (int)s.size(), &g, nullptr);
            CHECK_EQ_U(hud_nav16_glyph(&g), kC[i].exp, kC[i].msg);
        }
    }

    // --- NEW: neither angle nor exit number ---------------------------------
    // Nothing directional on the wire. Fall back to the straight-through
    // position rather than index 0, which means "back out the entry".
    {
        printf("[10] neither angle nor exit number -> straight through\n");
        auto s = hx("80 06 0a 04 0a 02 08 22");        // type 34 alone
        AaGuidance g;
        hud_nav16_on_frame(s.data(), (int)s.size(), &g, nullptr);
        CHECK_EQ_U(g.roundabout_exit_number, 0, "exit_number absent -> 0");
        CHECK_EQ_U(hud_nav16_glyph(&g), 43u, "CCW no data -> 180deg -> 43");
        auto t = hx("80 06 0a 04 0a 02 08 20");        // type 32 alone
        hud_nav16_on_frame(t.data(), (int)t.size(), &g, nullptr);
        CHECK_EQ_U(hud_nav16_glyph(&g), 55u, "CW  no data -> 180deg -> 55");
    }

    // --- NEW: an exit number must never override a real angle ---------------
    {
        printf("[11] angle wins over exit number\n");
        // exit 1 (estimate 90 -> 40) but a real angle of 180 -> must be 43
        auto s = hx("80 06 0a 09 0a 07 08 22 10 01 18 b4 01");
        AaGuidance g;
        hud_nav16_on_frame(s.data(), (int)s.size(), &g, nullptr);
        CHECK_EQ_U(hud_nav16_glyph(&g), 43u, "angle 180 beats exit-1 estimate");
    }

    printf("\n%s\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS");
    return g_fail;
}
