#include "display_ui.h"

#if defined(VMHUD_HAS_TFT)
#include "pin_config.h"
#include <TFT_eSPI.h>

#if defined(LCD_MODULE_CMD_1)
typedef struct {
  uint8_t cmd;
  uint8_t data[14];
  uint8_t len;
} lcd_cmd_t;

static const lcd_cmd_t kSt7789Init[] = {
    {0x11, {0}, 0 | 0x80},
    {0x3A, {0X05}, 1},
    {0xB2, {0X0B, 0X0B, 0X00, 0X33, 0X33}, 5},
    {0xB7, {0X75}, 1},
    {0xBB, {0X28}, 1},
    {0xC0, {0X2C}, 1},
    {0xC2, {0X01}, 1},
    {0xC3, {0X1F}, 1},
    {0xC6, {0X13}, 1},
    {0xD0, {0XA7}, 1},
    {0xD0, {0XA4, 0XA1}, 2},
    {0xD6, {0XA1}, 1},
    {0xE0, {0XF0, 0X05, 0X0A, 0X06, 0X06, 0X03, 0X2B, 0X32, 0X43, 0X36, 0X11, 0X10, 0X2B, 0X32}, 14},
    {0xE1, {0XF0, 0X08, 0X0C, 0X0B, 0X09, 0X24, 0X2B, 0X22, 0X43, 0X38, 0X15, 0X16, 0X2F, 0X37}, 14},
};

static void runLcdInitSequence(TFT_eSPI &tft) {
  for (uint8_t i = 0; i < (sizeof(kSt7789Init) / sizeof(lcd_cmd_t)); i++) {
    tft.writecommand(kSt7789Init[i].cmd);
    for (int j = 0; j < (kSt7789Init[i].len & 0x7f); j++) {
      tft.writedata(kSt7789Init[i].data[j]);
    }
    if (kSt7789Init[i].len & 0x80) {
      delay(120);
    }
  }
}
#endif

static TFT_eSPI g_tft;
static bool g_ready = false;

void displayInit() {
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);

  g_tft.init();
#if defined(LCD_MODULE_CMD_1)
  runLcdInitSequence(g_tft);
#endif
  g_tft.setRotation(1);
  g_tft.fillScreen(TFT_BLACK);
  digitalWrite(PIN_LCD_BL, HIGH);
  g_ready = true;

  DisplayState initSt;
  displayUpdate(initSt, true);
}

void displayUpdate(const DisplayState &st, bool force) {
  if (!g_ready) {
    return;
  }

  // If force redraw, paint background and static frames
  if (force) {
    g_tft.fillScreen(TFT_BLACK);
  }

  // --- Top Bar (Y: 2..24) ---
  g_tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  g_tft.drawString("VIETMAP HUD", 4, 4, 2);

  const uint16_t dot = st.bleConnected ? TFT_GREEN : TFT_RED;
  g_tft.fillCircle(200, 12, 5, dot);

  g_tft.setTextColor(st.bleConnected ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  g_tft.setTextPadding(80);
  g_tft.drawString(st.bleConnected ? "BLE: OK" : "BLE: ADV", 212, 4, 2);

  g_tft.setTextColor(st.hudInfoOk ? TFT_CYAN : TFT_DARKGREY, TFT_BLACK);
  g_tft.setTextPadding(45);
  g_tft.drawString(st.hudInfoOk ? "H50" : "---", 285, 4, 2);

  // --- Speed Limit Section (Y: 28..90, Left) ---
  g_tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  g_tft.drawString("GIOI HAN", 4, 28, 2);

  g_tft.setTextPadding(90);
  if (st.postedLimit >= 5 && st.postedLimit <= 200) {
    g_tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    g_tft.drawNumber(st.postedLimit, 4, 46, 7);
    g_tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    g_tft.drawString("km/h", 100, 70, 2);
  } else {
    g_tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    g_tft.drawString("--", 4, 46, 7);
    g_tft.setTextPadding(40);
    g_tft.drawString("    ", 100, 70, 2);
  }

  // --- Camera / GPS Speed Section (Y: 28..90, Right) ---
  g_tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  g_tft.drawString("CAMERA", 160, 28, 2);

  g_tft.setTextPadding(140);
  if (st.camDist > 0 && st.camDist < 60000) {
    g_tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    String cStr = "Dist: " + String(st.camDist) + "m";
    g_tft.drawString(cStr, 160, 48, 4);
  } else {
    g_tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    g_tft.drawString("Dist: --", 160, 48, 4);
  }

  g_tft.setTextColor(TFT_WHITE, TFT_BLACK);
  g_tft.setTextPadding(150);
  String spdStr = "GPS: " + String(st.currentSpeed) + " km/h";
  g_tft.drawString(spdStr, 160, 74, 2);

  // --- Road Name (Y: 96..114) ---
  g_tft.setTextColor(TFT_CYAN, TFT_BLACK);
  g_tft.setTextPadding(310);
  String roadLine = "Duong: ";
  if (st.currentRoad.length() > 0) {
    roadLine += (st.currentRoad.length() > 24 ? st.currentRoad.substring(0, 24) + "..." : st.currentRoad);
  } else {
    roadLine += "--";
  }
  g_tft.drawString(roadLine, 4, 98, 2);

  // --- Divider (Y: 118) ---
  g_tft.drawFastHLine(0, 118, 320, TFT_DARKGREY);

  // --- CMU Sync & Diagnostics (Y: 122..168) ---
  g_tft.setTextPadding(310);
  if (st.lastEmittedLimit >= 5 && st.lastEmittedLimit <= 200) {
    g_tft.setTextColor(TFT_GREEN, TFT_BLACK);
    char cmuLine[64];
    snprintf(cmuLine, sizeof(cmuLine), "CMU TX: SPLIM %u (#%u)", (unsigned)st.lastEmittedLimit, (unsigned)st.splimTxCount);
    g_tft.drawString(cmuLine, 4, 122, 2);
  } else {
    g_tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    g_tft.drawString("CMU TX: SPLIM -- (Waiting)", 4, 122, 2);
  }

  g_tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  g_tft.setTextPadding(310);
  char diagLine[64];
  uint32_t sec = st.uptimeSec;
  snprintf(diagLine, sizeof(diagLine), "PKTS: C:%u N:%u H:%u | UP: %02u:%02u:%02u",
           (unsigned)st.camPktCount, (unsigned)st.navPktCount, (unsigned)st.hbPktCount,
           (unsigned)(sec / 3600), (unsigned)((sec % 3600) / 60), (unsigned)(sec % 60));
  g_tft.drawString(diagLine, 4, 142, 2);
}

#elif defined(VMHUD_HAS_OLED)
#include "pin_config.h"
#include <Wire.h>
#include <U8g2lib.h>

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C g_u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ OLED_SCL, /* data=*/ OLED_SDA);
static bool g_ready = false;

void displayInit() {
  Wire.begin(OLED_SDA, OLED_SCL);
  g_u8g2.begin();
  g_ready = true;
  DisplayState initSt;
  displayUpdate(initSt, true);
}

void displayUpdate(const DisplayState &st, bool /*force*/) {
  if (!g_ready) {
    return;
  }

  g_u8g2.clearBuffer();

  // Line 1: Header + BLE Status
  g_u8g2.setFont(u8g2_font_6x10_tr);
  g_u8g2.drawStr(0, 9, "VM-HUD");
  g_u8g2.drawStr(46, 9, st.bleConnected ? "[BLE:OK]" : "[BLE:ADV]");
  if (st.hudInfoOk) {
    g_u8g2.drawStr(104, 9, "H50");
  }

  // Line 2: Speed Limit & Camera distance
  g_u8g2.setFont(u8g2_font_7x14B_tr);
  char limBuf[32];
  if (st.postedLimit >= 5 && st.postedLimit <= 200) {
    if (st.camDist > 0 && st.camDist < 60000) {
      snprintf(limBuf, sizeof(limBuf), "LIM: %u (%um)", st.postedLimit, st.camDist);
    } else {
      snprintf(limBuf, sizeof(limBuf), "LIM: %u km/h", st.postedLimit);
    }
  } else {
    snprintf(limBuf, sizeof(limBuf), "LIM: --");
  }
  g_u8g2.drawStr(0, 24, limBuf);

  // Line 3: GPS Speed & Road
  g_u8g2.setFont(u8g2_font_6x10_tr);
  char spdBuf[32];
  snprintf(spdBuf, sizeof(spdBuf), "SPD: %u km/h", st.currentSpeed);
  g_u8g2.drawStr(0, 36, spdBuf);

  if (st.currentRoad.length() > 0) {
    g_u8g2.drawStr(0, 47, st.currentRoad.substring(0, 20).c_str());
  } else {
    g_u8g2.drawStr(0, 47, "Road: --");
  }

  // Line 4: CMU UART Status & Packet count
  g_u8g2.drawHLine(0, 50, 128);
  char cmuBuf[32];
  if (st.lastEmittedLimit >= 5 && st.lastEmittedLimit <= 200) {
    snprintf(cmuBuf, sizeof(cmuBuf), "CMU: %u (#%u)", st.lastEmittedLimit, (unsigned)st.splimTxCount);
  } else {
    snprintf(cmuBuf, sizeof(cmuBuf), "CMU: waiting...");
  }
  g_u8g2.drawStr(0, 61, cmuBuf);

  g_u8g2.sendBuffer();
}

#endif
