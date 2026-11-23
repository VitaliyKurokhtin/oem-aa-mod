// VietMap Live → BLE HUD emulator → USB CDC "SPLIM <km/h>" for Mazda CMU.
// Supports Headless (Super Mini) and Display (LilyGO T-Display S3 or I2C OLED).
// Codec: lib/vmhud/hud_codec.h (AGPL-3.0, shared with mzd-vietnav-hud).

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <atomic>
#include <mutex>
#include "mbedtls/md5.h"
#include "hud_codec.h"
#include "display_ui.h"

static String g_name = "VIETMAP-HUD";
static String g_model = "H50";
static String g_hw = "1.0.0";
static String g_fw = "1.0.2";
static String g_protocol = "2.2.2";
static String g_sid = "VMS3SM000001";
static String g_adv = "BOTH";

static Preferences g_store;
static std::mutex g_cfgMutex;
static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_hudInfoOk{false};
static uint16_t g_conn = BLE_HS_CONN_HANDLE_NONE;
static NimBLECharacteristic *g_h1 = nullptr;
static NimBLECharacteristic *g_h50 = nullptr;
static bool g_subH1 = false;
static bool g_subH50 = false;
static vmhud::Receiver<4096> g_rxH1, g_rxH50;

static std::atomic<uint8_t> g_postedLimit{0};
static std::atomic<uint16_t> g_currentSpeed{0};
static std::atomic<uint16_t> g_camDist{0};
static std::atomic<uint8_t> g_camType{0};
static std::mutex g_roadMutex;
static String g_currentRoad = "";

static std::atomic<uint32_t> g_camPktCount{0};
static std::atomic<uint32_t> g_navPktCount{0};
static std::atomic<uint32_t> g_hbPktCount{0};
static std::atomic<uint32_t> g_splimTxCount{0};
static std::atomic<uint32_t> g_lastSplimMs{0};

static uint8_t g_lastEmittedLimit = 0;
static uint32_t g_lastKeepaliveMs = 0;
static volatile bool g_displayDirty = true;
static uint32_t g_lastDisplayMs = 0;

// CMU read_splim() rejects entries older than 8 s — re-emit every 2 s while
// a valid limit is held so the HUD sign does not go stale between CAM frames.
static const uint32_t kKeepaliveMs = 2000;

static bool sdkChecksum(const uint8_t *p, size_t n, const uint8_t *uid, uint8_t *out) {
  uint8_t digest[16];
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts_ret(&ctx);
  mbedtls_md5_update_ret(&ctx, p, n);
  mbedtls_md5_update_ret(&ctx, uid, 16);
  mbedtls_md5_finish_ret(&ctx, digest);
  mbedtls_md5_free(&ctx);
  vmhud::put32(out, vmhud::be32(digest) + vmhud::be32(digest + 4) + vmhud::be32(digest + 8) +
                         vmhud::be32(digest + 12));
  return true;
}

static String textUtf8(const uint8_t *p, size_t n) {
  String value;
  value.reserve(n);
  for (size_t i = 0; i < n && p[i]; i++) {
    value += (char)p[i];
  }
  return value;
}

static void printHex(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    Serial.printf("%02X", p[i]);
  }
  Serial.println();
}

static void emitSplim(uint8_t limit) {
  if (limit < 5 || limit > 200) {
    return;
  }
  g_lastEmittedLimit = limit;
  g_lastKeepaliveMs = millis();
  g_lastSplimMs = g_lastKeepaliveMs;
  g_splimTxCount++;
  g_displayDirty = true;
  Serial.printf("SPLIM %u\n", (unsigned)limit);
}

static uint8_t postedLimitFromCamera(const vmhud::Frame &frame, uint16_t &outDist, uint8_t &outType) {
  outDist = 0;
  outType = 0;
  if (frame.command != 1 || frame.size < 4 || (frame.size % 4) != 0) {
    return 0;
  }
  uint8_t pick = 0;
  uint16_t pickDist = 0xFFFF;
  uint8_t pickType = 0;

  for (size_t i = 0; i < frame.size; i += 4) {
    const uint8_t type = frame.payload[i];
    const uint16_t dist = vmhud::be16(frame.payload + i + 1);
    const uint8_t lim = frame.payload[i + 3];
    if (lim < 5 || lim > 200) {
      continue;
    }
    if (dist >= 60000) {
      continue;
    }
    if (dist < pickDist) {
      pickDist = dist;
      pick = lim;
      pickType = type;
    }
  }
  if (pick) {
    outDist = pickDist;
    outType = pickType;
    return pick;
  }
  for (size_t i = 0; i < frame.size; i += 4) {
    const uint8_t lim = frame.payload[i + 3];
    if (lim >= 5 && lim <= 200) {
      outDist = vmhud::be16(frame.payload + i + 1);
      outType = frame.payload[i];
      return lim;
    }
  }
  return 0;
}

static void notifyInfo(const vmhud::Frame &request, NimBLECharacteristic *route) {
  const bool subscribed = route == g_h1 ? g_subH1 : g_subH50;
  if (!g_connected || !subscribed) {
    Serial.println("[HUD] HUDInfo before notify subscribe");
    return;
  }
  String info;
  {
    std::lock_guard<std::mutex> guard(g_cfgMutex);
    info = "MODEL:" + g_model + ",HW:" + g_hw + ",FW:" + g_fw + ",PROTOCOL:" + g_protocol +
           ",OBDV:0_1.0,SID:" + g_sid;
  }
  static uint8_t frame[512];
  const size_t count =
      vmhud::build(0x0e, (const uint8_t *)info.c_str(), info.length(), request.timestamp, frame,
                   sizeof(frame), sdkChecksum, request.legacy);
  if (!count) {
    Serial.println("[HUD] HUDInfo build failed");
    return;
  }
  for (size_t offset = 0; offset < count; offset += 20) {
    const size_t chunk = count - offset < 20 ? count - offset : 20;
    if (!route->notify(frame + offset, chunk, g_conn)) {
      Serial.println("[HUD] notify failed");
      return;
    }
  }
  g_hudInfoOk = true;
  g_displayDirty = true;
  Serial.printf("[HUD] HUDInfo %u bytes\n", (unsigned)count);
}

static void onFrame(const vmhud::Frame &frame, void *user) {
  Serial.printf("[RX] CMD 0x%02X plen=%u\n", frame.command, (unsigned)frame.size);
  printHex(frame.payload, frame.size);

  if (frame.command == 0x0e) {
    notifyInfo(frame, (NimBLECharacteristic *)user);
    return;
  }

  if (frame.command == 0 || frame.command == 0x40) {
    vmhud::Nav nav;
    if (!vmhud::navigation(frame, nav)) {
      return;
    }
    g_navPktCount++;
    g_currentSpeed = nav.speed;
    const String current = textUtf8(nav.current, nav.currentSize);
    {
      std::lock_guard<std::mutex> guard(g_roadMutex);
      g_currentRoad = current;
    }
    g_displayDirty = true;
    Serial.printf("[NAV] speed=%u cur=\"%s\"\n", (unsigned)nav.speed, current.c_str());
    return;
  }

  // Camera and speed limit (CMD 0x01, multiple of 4 bytes)
  if (frame.command == 1 && frame.size >= 4 && (frame.size % 4) == 0) {
    g_camPktCount++;
    for (size_t i = 0; i < frame.size; i += 4) {
      Serial.printf("[CAM] type=%u dist=%u limit=%u\n", frame.payload[i],
                    (unsigned)vmhud::be16(frame.payload + i + 1), frame.payload[i + 3]);
    }
    uint16_t dist = 0;
    uint8_t type = 0;
    const uint8_t lim = postedLimitFromCamera(frame, dist, type);
    g_camDist = dist;
    g_camType = type;
    g_postedLimit = lim;
    g_displayDirty = true;
    if (lim) {
      emitSplim(lim);
    }
    return;
  }

  if (frame.command == 2 && (frame.size == 1 || frame.size == 2)) {
    g_hbPktCount++;
    const uint16_t spd = frame.size == 1 ? frame.payload[0] : vmhud::be16(frame.payload);
    g_currentSpeed = spd;
    g_displayDirty = true;
    Serial.printf("[HB] speed=%u\n", (unsigned)spd);
  }
}

static void startConnectable() { NimBLEDevice::startAdvertising(0); }

struct ServerCB : NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &conn) override {
    if (g_connected) {
      server->disconnect(conn.getConnHandle());
      return;
    }
    g_conn = conn.getConnHandle();
    g_connected = true;
    g_hudInfoOk = false;
    g_subH1 = g_subH50 = false;
    g_rxH1.reset();
    g_rxH50.reset();
    g_displayDirty = true;
    Serial.println("[BLE] connected");
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &conn, int reason) override {
    if (conn.getConnHandle() != g_conn) {
      return;
    }
    g_connected = false;
    g_hudInfoOk = false;
    g_conn = BLE_HS_CONN_HANDLE_NONE;
    g_subH1 = g_subH50 = false;
    g_rxH1.reset();
    g_rxH50.reset();
    g_postedLimit = 0;
    g_camDist = 0;
    g_camType = 0;
    g_lastEmittedLimit = 0;
    g_displayDirty = true;
    Serial.printf("[BLE] disconnected reason=%d\n", reason);
    startConnectable();
  }
};

struct SubscribeCB : NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *c, NimBLEConnInfo &conn, uint16_t sub) override {
    if (conn.getConnHandle() != g_conn) {
      return;
    }
    if (c == g_h1) {
      g_subH1 = (sub & 1) != 0;
    } else {
      g_subH50 = (sub & 1) != 0;
    }
    g_displayDirty = true;
    Serial.printf("[BLE] notify sub=0x%04X\n", sub);
  }
};

struct WriteCB : NimBLECharacteristicCallbacks {
  NimBLECharacteristic *route;
  const char *label;
  vmhud::Receiver<4096> *rx;
  WriteCB(NimBLECharacteristic *r, const char *l, vmhud::Receiver<4096> *b) : route(r), label(l), rx(b) {}
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &conn) override {
    if (conn.getConnHandle() != g_conn) {
      return;
    }
    const std::string data = c->getValue();
    Serial.printf("[W:%s] %u bytes: ", label, (unsigned)data.size());
    printHex((const uint8_t *)data.data(), data.size());
    rx->feed((const uint8_t *)data.data(), data.size(), sdkChecksum, onFrame, route);
  }
};

template <typename Advertisement>
static void fillServiceUuids(Advertisement &data) {
  if (g_adv == "BOTH" || g_adv == "FFF0") {
    data.addServiceUUID("FFF0");
  }
  if (g_adv == "BOTH" || g_adv == "FFFF") {
    data.addServiceUUID("FFFF");
  }
}

static void applyAdvertising() {
  const std::string name(g_name.c_str());
  NimBLEDevice::setDeviceName(name);

  NimBLEAdvertisementData primary;
  primary.setFlags(0x06);
  fillServiceUuids(primary);

  NimBLEAdvertisementData scanResp;
  scanResp.setName(name);

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setAdvertisementData(primary);
  adv->setScanResponseData(scanResp);
  adv->enableScanResponse(true);
  startConnectable();
  Serial.printf("[ADV] name=%s (scan response) adv=%s\n", g_name.c_str(), g_adv.c_str());
}

void setup() {
  Serial.begin(115200);
  // Native USB CDC blocks each write up to 100 ms when the host is attached but not
  // reading (CMU daemon not running yet). Our prints run on the BLE host task, so cap
  // the stall; a dropped line is harmless because SPLIM is re-sent every 2 s.
  Serial.setTxTimeoutMs(10);
  delay(400);
  Serial.println("\n[BOOT] vmhud-esp32-s3 — VietMap BLE HUD → USB SPLIM");

  displayInit();

  if (g_store.begin("vmhud", false)) {
    g_name = g_store.getString("name", g_name);
    g_model = g_store.getString("model", g_model);
    g_hw = g_store.getString("hw", g_hw);
    g_fw = g_store.getString("fw", g_fw);
    g_protocol = g_store.getString("protocol", g_protocol);
    g_sid = g_store.getString("sid", g_sid);
    g_adv = g_store.getString("adv", g_adv);
  }

  NimBLEDevice::init(std::string(g_name.c_str()));
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  NimBLEService *h1 = server->createService("FFF0");
  g_h1 = h1->createCharacteristic("FFF1", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_h1->setCallbacks(new SubscribeCB());
  const uint32_t writable = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR;
  h1->createCharacteristic("FFF2", writable)->setCallbacks(new WriteCB(g_h1, "FFF2", &g_rxH1));
  h1->createCharacteristic("FFF3", writable)->setCallbacks(new WriteCB(g_h1, "FFF3", &g_rxH1));
  h1->createCharacteristic("FFF4", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  h1->start();

  NimBLEService *h50 = server->createService("FFFF");
  g_h50 = h50->createCharacteristic("1234", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_h50->setCallbacks(new SubscribeCB());
  h50->createCharacteristic("9ABC", writable)->setCallbacks(new WriteCB(g_h50, "9ABC", &g_rxH50));
  h50->start();

  applyAdvertising();
}

void loop() {
  const uint32_t now = millis();
  const uint8_t lim = g_postedLimit.load();

  // SPLIM USB serial keepalive emission to CMU
  if (lim >= 5 && lim <= 200) {
    if (lim != g_lastEmittedLimit || (now - g_lastKeepaliveMs) >= kKeepaliveMs) {
      emitSplim(lim);
    }
  }

  // Display update (refresh every 500ms or when state changes)
#if VMHUD_HAS_DISPLAY
  if (g_displayDirty || (now - g_lastDisplayMs >= 500)) {
    g_displayDirty = false;
    g_lastDisplayMs = now;

    DisplayState st;
    st.bleConnected = g_connected.load();
    st.hudInfoOk = g_hudInfoOk.load();
    st.notifySubscribed = g_subH1 || g_subH50;
    st.postedLimit = lim;
    st.camDist = g_camDist.load();
    st.camType = g_camType.load();
    st.currentSpeed = g_currentSpeed.load();
    {
      std::lock_guard<std::mutex> guard(g_roadMutex);
      st.currentRoad = g_currentRoad;
    }
    st.lastEmittedLimit = g_lastEmittedLimit;
    st.splimTxCount = g_splimTxCount.load();
    st.lastSplimMs = g_lastSplimMs.load();
    st.camPktCount = g_camPktCount.load();
    st.navPktCount = g_navPktCount.load();
    st.hbPktCount = g_hbPktCount.load();
    st.uptimeSec = now / 1000;

    displayUpdate(st);
  }
#endif

  delay(50);
}
