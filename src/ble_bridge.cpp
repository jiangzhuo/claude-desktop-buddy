#include "ble_bridge.h"
#include <NimBLEDevice.h>
#include <Arduino.h>
#include <string.h>
#include <esp_random.h>
#include "psram_util.h"
#include "safe_log.h"

// Nordic UART Service UUIDs — every BLE serial example uses these, so
// existing tools (nRF Connect, bluefy, Web Bluetooth examples) can talk to
// us without custom UUIDs.
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Incoming bytes are buffered in a simple ring for bleRead()/bleAvailable().
// Sized to hold a transcript snapshot JSON plus headroom; the GATT layer
// will flow-control if we fall behind. Lives in PSRAM — BLE stack callbacks
// just memcpy into it, no DMA. Allocated in bleInit().
static const size_t    RX_CAP = 2048;
static uint8_t*        rxBuf = nullptr;
static volatile size_t rxHead = 0;
static volatile size_t rxTail = 0;

static NimBLEServer*         server = nullptr;
static NimBLECharacteristic* txChar = nullptr;
static NimBLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
static volatile uint16_t  mtu = 23;

// Event counters consumed by the main loop for deferred logging. On this
// chip, calling Serial.println/printf from inside a BLE callback race-
// crashes the UART driver's tx_mux — the callback runs on the NimBLE host
// task on core 0, and concurrent Serial access from that context is
// unsafe. Main-loop polls these counters and emits any logs from core 1.
static volatile uint8_t  evConnect = 0;
static volatile uint8_t  evDisconnect = 0;
static volatile uint16_t evMtu = 0;        // 0 = no new MTU event
static volatile uint32_t evAuthOk = 0;     // 0xFFFFFFFF = FAIL, 0 = none, 1 = ok
static volatile uint32_t evPasskey = 0;    // 0 = none

static void rxPush(const uint8_t* p, size_t n) {
  if (!rxBuf) return;
  for (size_t i = 0; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) return;  // full — drop (upstream should keep up)
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (!v.empty()) rxPush((const uint8_t*)v.data(), v.size());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    connected = true;
    evConnect++;
    // ets_printf is safe from any context/core — unlike arduino Serial
    // which is guarded off post-BLE. Lets us see the full connection
    // lifecycle from the host task without touching UART driver state.
    ets_printf("[ble cb] connect conn=%u\n", desc->conn_handle);
  }
  void onDisconnect(NimBLEServer* s) override {
    connected = false;
    secure = false;
    mtu = 23;
    evDisconnect++;
    ets_printf("[ble cb] disconnect\n");
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t newMtu, ble_gap_conn_desc* desc) override {
    mtu = newMtu;
    evMtu = newMtu;
    ets_printf("[ble cb] mtu=%u\n", newMtu);
  }
  uint32_t onPassKeyRequest() override {
    ets_printf("[ble cb] onPassKeyRequest -> %06lu\n", (unsigned long)passkey);
    return passkey;
  }
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    secure = desc->sec_state.encrypted;
    evAuthOk = secure ? 1 : 0xFFFFFFFF;
    ets_printf("[ble cb] auth complete enc=%u auth=%u bonded=%u reason=-\n",
               desc->sec_state.encrypted, desc->sec_state.authenticated,
               desc->sec_state.bonded);
    if (!secure && server) server->disconnect(desc->conn_handle);
  }
};

// NimBLE 1.4 doesn't auto-generate a random passkey and notify us
// (unlike Bluedroid). Instead the stack reads the value previously set
// via NimBLEDevice::setSecurityPasskey() and either uses it directly
// or — if left at the compile-time default of 123456 — falls back to
// onPassKeyRequest(). We pre-set a random 6-digit passkey in bleInit()
// and expose it through blePasskey() for the UI to display. onPassKey-
// Request is kept as a belt-and-suspenders fallback.
class SecCallbacks : public NimBLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    ets_printf("[sec cb] PassKeyRequest -> %06lu\n", (unsigned long)passkey);
    return passkey;
  }
  bool onConfirmPIN(uint32_t pk) override {
    // Negotiated Numeric Comparison (shouldn't happen with DisplayOnly +
    // KeyboardOnly, but some stacks pick NC anyway). Accept so pairing
    // doesn't silently fail.
    ets_printf("[sec cb] ConfirmPIN %06lu -> accept\n", (unsigned long)pk);
    return true;
  }
  bool onSecurityRequest() override {
    ets_printf("[sec cb] SecurityRequest -> accept\n");
    return true;
  }
  void onPassKeyNotify(uint32_t pk) override {
    passkey = pk;
    evPasskey = pk;
    ets_printf("[sec cb] PassKeyNotify %06lu\n", (unsigned long)pk);
  }
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    secure = desc->sec_state.encrypted;
    evAuthOk = secure ? 1 : 0xFFFFFFFF;
    ets_printf("[sec cb] auth complete enc=%u auth=%u bonded=%u\n",
               desc->sec_state.encrypted, desc->sec_state.authenticated,
               desc->sec_state.bonded);
  }
};

void bleInit(const char* deviceName) {
  if (!rxBuf) rxBuf = (uint8_t*)psAlloc(RX_CAP);
  NimBLEDevice::init(deviceName);
  // Request the biggest MTU we can get. macOS negotiates to ~185 typically.
  NimBLEDevice::setMTU(517);

  // Generate a random 6-digit passkey for this boot. Values 0..999999
  // are the valid range for BLE SMP passkeys. esp_random() gives good
  // entropy from the hardware RNG. Leave out 123456 (NimBLE's sentinel
  // meaning "no static passkey set") — in the unlikely event we draw
  // it, re-roll.
  uint32_t pk;
  do { pk = esp_random() % 1000000u; } while (pk == 123456u);
  passkey = pk;
  NimBLEDevice::setSecurityPasskey(pk);

  // LE Secure Connections, passkey-entry: we are DisplayOnly, the central
  // is KeyboardOnly. The user reads `passkey` off the LCD and types it
  // on the desktop. main.cpp polls blePasskey() to render it.
  NimBLEDevice::setSecurityAuth(/*bonding*/ true, /*MITM*/ true, /*SC*/ true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityCallbacks(new SecCallbacks());

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = server->createService(NUS_SERVICE_UUID);

  // NimBLE auto-creates CCCD (BLE2902 equivalent) when NOTIFY is set;
  // READ_ENC / WRITE_ENC on the characteristic flags the whole exchange
  // as encryption-required.
  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC
  );

  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC
  );
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);   // iOS-friendly connection interval
  adv->setMaxPreferred(0x12);
  NimBLEDevice::startAdvertising();
}

bool bleConnected() { return connected; }
bool bleSecure()    { return secure; }
// Expose the static passkey only while a pairing attempt is in flight
// (connected but not yet encrypted). The value itself is set once in
// bleInit() and never changes — the gate here is just for UI.
uint32_t blePasskey() { return (connected && !secure) ? passkey : 0; }

void bleClearBonds() {
  int n = NimBLEDevice::getNumBonds();
  if (n > 0) NimBLEDevice::deleteAllBonds();
  LOGF("[ble] cleared %d bond(s)\n", n);
}

size_t bleAvailable() {
  return (rxHead + RX_CAP - rxTail) % RX_CAP;
}

int bleRead() {
  if (!rxBuf || rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  // ATT notify payload is limited to (MTU - 3). macOS negotiates 185, so
  // the 182-byte chunk works there; use the live mtu so a peer that caps
  // at the 23-byte default doesn't get truncated notifies.
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    // Small yield so the BLE stack flushes before the next chunk.
    delay(4);
  }
  return sent;
}

// Drain deferred event logs from core 1. Call from main loop; prints all
// pending events then clears counters. Safe to call every loop iteration —
// only emits output when events have happened.
void bleDrainEvents() {
  uint8_t c = evConnect;    if (c) { evConnect = 0;    for (uint8_t i = 0; i < c; i++) ROMLOGF("[ble] connected\n"); }
  uint8_t d = evDisconnect; if (d) { evDisconnect = 0; for (uint8_t i = 0; i < d; i++) ROMLOGF("[ble] disconnected\n"); }
  uint16_t m = evMtu;       if (m) { evMtu = 0;        ROMLOGF("[ble] mtu=%u\n", m); }
  uint32_t a = evAuthOk;    if (a) { evAuthOk = 0;     ROMLOGF("[ble] auth %s\n", a == 0xFFFFFFFFu ? "FAIL" : "ok"); }
  uint32_t p = evPasskey;   if (p) { evPasskey = 0;    ROMLOGF("[ble] passkey %06lu\n", (unsigned long)p); }
}
