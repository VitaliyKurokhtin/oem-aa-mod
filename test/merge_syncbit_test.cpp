// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host regression test for the syncbit fix in svcjcinavi/merge.cpp.
// Include the implementation so the test exercises the actual HUD hooks.

#define LOG_LEVEL 5
#include "../mazda/patches/svcjcinavi/merge.cpp"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
static VbsNaviHudDisplay g_forwarded_display = {};
static VbsNaviHudMsg2 g_forwarded_msg2 = {};
static unsigned g_set_calls = 0;
static unsigned g_msg2_calls = 0;

#define CHECK_EQ_U(got, exp, msg) do {                                   \
        unsigned long _g = (unsigned long)(got), _e = (unsigned long)(exp); \
        if (_g != _e) { printf("  FAIL: %s (got %lu, want %lu)\n",       \
                               (msg), _g, _e); g_fail = 1; }             \
        else          { printf("  ok:   %s = %lu\n", (msg), _g); }       \
    } while (0)

#define CHECK_EQ_S(got, exp, msg) do {                                   \
        if (std::strcmp((got), (exp)) != 0) {                            \
            printf("  FAIL: %s (got \"%s\", want \"%s\")\n",             \
                   (msg), (got), (exp)); g_fail = 1; }                   \
        else { printf("  ok:   %s = \"%s\"\n", (msg), (got)); }          \
    } while (0)

static int mock_set(void *, VbsNaviHudDisplay *disp, void *, void *, void *)
{
    g_forwarded_display = *disp;
    ++g_set_calls;
    return 17;
}

static int mock_msg2(void *, VbsNaviHudMsg2 *msg2, void *, void *, void *)
{
    g_forwarded_msg2 = *msg2;
    ++g_msg2_calls;
    return 23;
}

static void reset_merge_state()
{
    g_gate_done = true;
    g_enabled = true;
    g_real_set = mock_set;
    g_real_msg2 = mock_msg2;
    g_cur_street = nullptr;

    g_have_aap = false;
    g_aap_man = 0;
    g_aap_dist = 0;
    g_aap_dunit = 0;
    g_aap_last = 0;
    g_aap_syncbit = 0;
    g_aap_last_man = 0;
    g_oem_syncbit = 0;
    g_street_action = STREET_PASSTHROUGH;
    g_aap_street[0] = '\0';
    g_oem_speed = 0;
    g_oem_sunit = 0;

    g_forwarded_display = {};
    g_forwarded_msg2 = {};
    g_set_calls = 0;
    g_msg2_calls = 0;
}

static int send_aap(uint32_t maneuver, uint16_t distance, uint8_t syncbit)
{
    VbsNaviHudDisplay disp = {};
    disp.nextManeuverInfo = maneuver;
    disp.distanceValue = distance;
    disp.displaySpeedLimit = kAapSpeedSentinel;
    disp.text_ID3 = syncbit;
    return VBS_NAVI_SetHUDDisplayMsgReq(nullptr, &disp, nullptr, nullptr, nullptr);
}

static int send_oem(uint32_t maneuver, uint16_t distance, uint16_t speed,
                    uint8_t syncbit)
{
    VbsNaviHudDisplay disp = {};
    disp.nextManeuverInfo = maneuver;
    disp.distanceValue = distance;
    disp.displaySpeedLimit = speed;
    disp.text_ID3 = syncbit;
    return VBS_NAVI_SetHUDDisplayMsgReq(nullptr, &disp, nullptr, nullptr, nullptr);
}

static int send_street(const char *road, uint8_t syncbit)
{
    VbsNaviHudMsg2 msg2 = {};
    msg2.guidancePointName = road;
    msg2.syncBit = syncbit;
    return VBS_NAVI_TMC_SetHUD_Display_Msg2(nullptr, &msg2, nullptr, nullptr, nullptr);
}

int main()
{
    reset_merge_state();

    printf("[1] maneuver changes while distance remains zero and road stays the same\n");
    CHECK_EQ_U(send_aap(8, 0, 2), 17, "initial AAP maneuver forwarded");
    CHECK_EQ_U(g_forwarded_display.nextManeuverInfo, 8, "initial maneuver retained");
    CHECK_EQ_U(g_forwarded_display.distanceValue, 0, "initial distance is zero");
    CHECK_EQ_U(g_forwarded_display.text_ID3, 2, "initial SyncBit retained");

    CHECK_EQ_U(send_street("MAIN STREET", 2), 23, "initial road forwarded");
    CHECK_EQ_S(g_forwarded_msg2.guidancePointName, "MAIN STREET",
               "initial road captured");

    CHECK_EQ_U(send_aap(9, 0, 2), 17, "changed AAP maneuver forwarded");
    CHECK_EQ_U(g_forwarded_display.nextManeuverInfo, 9, "new maneuver forwarded");
    CHECK_EQ_U(g_forwarded_display.distanceValue, 0,
               "distance remains zero for changed maneuver");
    CHECK_EQ_U(g_forwarded_display.text_ID3, 3,
               "SyncBit advances when maneuver changes with unchanged distance");

    CHECK_EQ_U(send_street("MAIN STREET", 2), 23, "unchanged road forwarded");
    CHECK_EQ_U(g_forwarded_msg2.syncBit, 3,
               "AAP street strip matches incremented maneuver SyncBit");
    CHECK_EQ_S(g_forwarded_msg2.guidancePointName, "MAIN STREET",
               "road remains unchanged after maneuver update");

    CHECK_EQ_U(send_aap(9, 0, 2), 17, "repeated maneuver with stale OEM SyncBit forwarded");
    CHECK_EQ_U(g_forwarded_display.text_ID3, 3,
               "stale OEM SyncBit does not roll back our generation");

    CHECK_EQ_U(send_aap(9, 0, 5), 17, "changed OEM SyncBit forwarded");
    CHECK_EQ_U(g_forwarded_display.text_ID3, 4,
               "OEM SyncBit change advances our local generation");

    CHECK_EQ_U(send_aap(9, 0, 5), 17, "repeated OEM SyncBit forwarded");
    CHECK_EQ_U(g_forwarded_display.text_ID3, 4,
               "unchanged OEM SyncBit does not advance our generation");

    CHECK_EQ_U(send_aap(9, 0, 2), 17, "lower OEM SyncBit forwarded");
    CHECK_EQ_U(g_forwarded_display.text_ID3, 5,
               "lower OEM SyncBit advances locally instead of rolling back");

    CHECK_EQ_U(send_oem(0, 0, 50, 2), 17, "OEM frame reasserted while AAP is active");
    CHECK_EQ_U(g_forwarded_display.text_ID3, 5,
               "OEM maneuver frame uses current AAP generation");
    CHECK_EQ_U(send_street("OEM STREET", 2), 23, "OEM street strip replaced while AAP is active");
    CHECK_EQ_U(g_forwarded_msg2.syncBit, 5,
               "replaced OEM street strip matches current AAP generation");
    CHECK_EQ_S(g_forwarded_msg2.guidancePointName, "MAIN STREET",
               "replaced OEM street uses captured AAP street");

    CHECK_EQ_U(send_aap(0, 0, 0), 17, "empty AAP frame relinquishes ownership");
    CHECK_EQ_U(send_oem(0, 0, 50, 2), 17, "OEM frame passes through after AAP stops");
    CHECK_EQ_U(g_forwarded_display.text_ID3, 2,
               "inactive OEM maneuver retains its own SyncBit");
    CHECK_EQ_U(send_street("OEM STREET", 2), 23,
               "OEM street strip passes through after AAP stops");
    CHECK_EQ_U(g_forwarded_msg2.syncBit, 2,
               "inactive OEM street retains its own SyncBit");
    CHECK_EQ_S(g_forwarded_msg2.guidancePointName, "OEM STREET",
               "inactive OEM street is not replaced by stale AAP street");

    CHECK_EQ_U(g_set_calls, 9, "all maneuver frames reached the real setter");
    CHECK_EQ_U(g_msg2_calls, 4, "all road strips reached the real setter");

    reset_merge_state();
    printf("[2] locally generated SyncBit wraps within the HUD generation range\n");
    CHECK_EQ_U(send_aap(20, 0, 1), 17, "initial generation seeded at one");
    for (uint8_t source_syncbit = 2; source_syncbit <= 7; ++source_syncbit) {
        CHECK_EQ_U(send_aap(20, 0, source_syncbit), 17,
                   "OEM generation change forwarded");
    }
    CHECK_EQ_U(g_forwarded_display.text_ID3, 7,
               "local SyncBit reaches the maximum generation value");
    CHECK_EQ_U(send_aap(20, 0, 1), 17, "OEM generation wraps to one");
    CHECK_EQ_U(g_forwarded_display.text_ID3, 1,
               "local SyncBit wraps from seven to one");

    return g_fail ? 1 : 0;
}
