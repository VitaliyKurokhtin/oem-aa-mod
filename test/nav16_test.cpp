// SPDX-License-Identifier: AGPL-3.0-or-later
//
// nav16_test — host self-test for the AA GAL 1.6 nav decoder (hud_nav16).
// Feeds hand-built synthetic 0x8006/0x8007/0x8003 frames through the PUBLIC API
// and asserts the decoded guidance/position/glyph/units/status, then drives the
// same frames through the push path (hud_nav16_feed -> sink) and asserts the
// right callback fires. Returns non-zero on any failed assertion so CI can gate
// on it. Pure host build (no ARM sysroot).

#include "hud_nav16.h"
#include "hud_nav.h"

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

    // --- Legacy GAL 1.5 roundabout glyph wrap at 360 degrees ----------------
    // 12 glyphs exist per traffic side, so the rounded index must wrap 12 -> 0.
    {
        printf("[5] Legacy GAL 1.5 roundabout angle wrap\n");
        CHECK_EQ_U(roundabout_icon(344, 1), 48u, "RHT angle 344 -> last glyph");
        CHECK_EQ_U(roundabout_icon(345, 1), 37u, "RHT angle 345 wraps to base");
        CHECK_EQ_U(roundabout_icon(359, 1), 37u, "RHT angle 359 wraps to base");
        CHECK_EQ_U(roundabout_icon(360, 1), 37u, "RHT angle 360 wraps to base");
        CHECK_EQ_U(roundabout_icon(360, 0), 49u, "LHT angle 360 wraps to base");

        CHECK_EQ_U(roundabout_icon(-1, 1), 37u, "RHT angle -1 wraps to base");
        CHECK_EQ_U(roundabout_icon(-15, 1), 37u, "RHT angle -15 wraps to base");
        CHECK_EQ_U(roundabout_icon(-16, 1), 48u, "RHT angle -16 wraps to last glyph");
        CHECK_EQ_U(roundabout_icon(-360, 1), 37u, "RHT angle -360 wraps to base");

        CHECK_EQ_U(roundabout_icon(-16, 0), 60u, "LHT angle -16 wraps to last glyph");
        CHECK_EQ_U(roundabout_icon(-360, 0), 49u, "LHT angle -360 wraps to base");
    }

    // --- NavigationStatus (0x8003): status field 1 varint --------------------
    //   0x8003 body: field 1 (tag 0x08) varint = 2  -> status 2
    {
        printf("[6] NavigationStatus 0x8003 field 1\n");
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
        printf("[7] hud_nav16_feed -> sink dispatch\n");
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

    printf("\n%s\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS");
    return g_fail;
}
