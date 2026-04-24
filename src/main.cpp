#include <M5StickCPlus2.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"
#include "power.h"
#include "ui_text.h"
#include "safe_log.h"

volatile bool g_uartAlive = true;

// PSRAM arena globals (see psram_util.h).
uint8_t* g_psArenaBase = nullptr;
size_t   g_psArenaCap  = 0;
size_t   g_psArenaUsed = 0;
uint8_t* g_psArenaLastPtr = nullptr;

// Dedicated pre-BLE PSRAM buffer for askJson — lives for the life of
// the firmware, reused for each AskUserQuestion capture.
char*  g_askJsonBuf = nullptr;
size_t g_askJsonCap = 0;

// Cached at boot before BLE init — the live heap_caps_get_info() would
// walk the corrupt PSRAM free list post-BLE. Reported by info pages
// and the status xfer command (xfer.h declares matching externs).
size_t g_psramTotalCached = 0;
size_t g_psramFreeAtBoot = 0;

M5Canvas spr(&M5.Lcd);

// HUD scratch buffers in PSRAM — allocated in hudPrealloc() before BLE
// init. Post-BLE PSRAM malloc is unsafe; these are used for the lifetime
// of the firmware so there's no need to free them.
//
// At 48-byte stride we fit ~15 CJK glyphs per wrapped row. 128 rows at
// 15 glyphs = ~1920 characters of transcript history expandable via
// scroll — plenty for a few hundred-character bridge messages plus
// surrounding context lines.
static const uint8_t HUD_MAX_ROWS = 128;
static const uint8_t HUD_ROW_STRIDE = 48;

char     (*g_hudDisp)[HUD_ROW_STRIDE] = nullptr;
uint8_t* g_hudSrcOf = nullptr;
static void hudPrealloc() {
  if (!g_hudDisp)  g_hudDisp  = (char (*)[HUD_ROW_STRIDE])psAlloc(HUD_MAX_ROWS * HUD_ROW_STRIDE);
  if (!g_hudSrcOf) g_hudSrcOf = (uint8_t*)psAlloc(HUD_MAX_ROWS);
}

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = 135, H = 240;
const int CX = W / 2;
const int CY_BASE = 120;
const int LED_PIN = 10;          // red LED, active-low

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;
bool    btnBLong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// AskUserQuestion: cursor position in the options list. Reset to 0 on
// each new promptId. Clamped at render time in case the captured JSON
// had fewer options than expected.
uint8_t  askSel = 0;

// Is the pending prompt an AskUserQuestion (with captured options)?
static bool promptIsAsk() {
  return tama.promptId[0]
      && tama.askJson
      && strcmp(tama.promptTool, "AskUserQuestion") == 0;
}

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

// brightLevel 0..4 → 51, 102, 153, 204, 255 on the 0..255 panel scale.
static void applyBrightness() { M5.Display.setBrightness((brightLevel + 1) * 51); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Display.wakeup();
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Speaker.tone(freq, dur);
}

static void sendCmd(const char* json) {
  LOGLN(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "jp font", "cn font", "kr font", "clock rot", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 13;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.cjkJa = !s.cjkJa; uiSetCjkRoute(s.cjkJa, s.cjkCn, s.cjkKr); break;
    case 7: s.cjkCn = !s.cjkCn; uiSetCjkRoute(s.cjkJa, s.cjkCn, s.cjkKr); break;
    case 8: s.cjkKr = !s.cjkKr; uiSetCjkRoute(s.cjkJa, s.cjkCn, s.cjkKr); break;
    case 9:  s.clockRot = (s.clockRot + 1) % 3; break;
    case 10: nextPet(); return;
    case 11: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 12: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud, s.cjkJa, s.cjkCn, s.cjkKr };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, my + 8 + i * 14);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 8) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 9) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 10) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: M5.Power.powerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static m5::rtc_time_t _clkTm;
static m5::rtc_date_t _clkDt;
uint32_t              _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool           _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = M5.Power.isCharging();
  M5.Rtc.getTime(&_clkTm);
  M5.Rtc.getDate(&_clkDt);
}

static void clockUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return ((uint8_t)_clkDt.weekDay) % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.hours, _clkTm.minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.seconds);
  uint8_t mi = (_clkDt.month >= 1 && _clkDt.month <= 12) ? _clkDt.month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.date);

  if (clockOrient == 0) {
    paintedOrient = 0;
    // Bottom half — buddy naturally lives at y=0..82, GIF peeks at top
    // via peek mode. Clearing from 90 leaves both untouched.
    spr.fillRect(0, 90, W, H - 90, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 140);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 175);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 200);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Landscape: 240×135 direct-to-LCD. Full fill only on entry; after that
  // text glyph bg cells repaint themselves and the pet box (small, ~90×50)
  // gets a fillRect each pet tick — small enough not to tear.
  M5.Lcd.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Lcd.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  // Seconds tick at 1Hz; redrawing 3 strings at 60fps is 180 SPI ops/sec
  // for nothing. Gate on the second changing (or full repaint).
  if (repaint || _clkTm.seconds != lastSec) {
    lastSec = _clkTm.seconds;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.date);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.seconds);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(p.text, p.bg);    M5.Lcd.drawString(hm, 170, 42);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(p.textDim, p.bg); M5.Lcd.drawString(ssl, 170, 72);
                                                                  M5.Lcd.drawString(wdl, 170, 102);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
  }

  // Pet on left at 5 fps. Clear includes the overlay-particle zone above
  // the body (y<30) — species draw Zzz/hearts there via BUDDY_Y_OVERLAY=6
  // which doesn't go through _yb, so the box has to cover it.
  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    if (buddyMode) {
      // ASCII glyphs don't self-clear; wipe the box each tick. Species
      // hardcode BUDDY_X_CENTER=67 / BUDDY_Y_OVERLAY=6 for particles so
      // keep portrait coords and just swap the surface — pet lands
      // upper-left of landscape, which is where we want it anyway.
      M5.Lcd.fillRect(0, 0, 115, 90, p.bg);
      buddyRenderTo(&M5.Lcd, activeState);
    } else {
      // Full-frame GIFs paint every pixel (transparent → pal.bg), so a
      // per-tick clear just adds a visible black flash between wipe and
      // last scanline. The entry fillScreen on paintedOrient change
      // already covers the surround.
      characterSetState(activeState);
      characterRenderTo(&M5.Lcd, 57, 45);
    }
  }
  M5.Lcd.setRotation(0);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 184); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("A   front");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    approve prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("B   right side");
    spr.setTextColor(p.textDim, p.bg); ln("    next page");
    ln("    deny prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold A");
    spr.setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Power  left side");
    spr.setTextColor(p.textDim, p.bg); ln("    tap = screen off");
    ln("    hold 6s = off");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = M5.Power.getBatteryVoltage();
    int iBat_mA = (int)M5.Power.getBatteryCurrent();
    int vBus_mV = M5.Power.getVBUSVoltage();
    int pct = batteryPercent(vBat_mV);
    bool usb = M5.Power.isCharging() || vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(60, y + 4);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    // PSRAM stats snapshot from before BLE started (live heap walk
    // post-BLE would crash on the corrupted free-list). Arena usage
    // is tracked by us directly, safe to read any time.
    if (g_psramTotalCached > 0) {
      ln("  psram    %u/%uKB",
         (unsigned)((g_psramTotalCached - g_psramFreeAtBoot) / 1024),
         (unsigned)(g_psramTotalCached / 1024));
      ln("  ps-arena %u/%uB",
         (unsigned)psArenaUsed(), (unsigned)psArenaCapacity());
    }
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    float imuTemp = 0.0f;
    M5.Imu.getTemp(&imuTemp);
    ln("  temp     %dC", (int)imuTemp);

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    ln("M5StickC Plus2");
    ln("ESP32-PICO-V3-02");
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written. Runtime stride lets callers use
// different row widths for portrait (24B rows × 21 cols) and landscape
// (48B rows × 40 cols).
static uint8_t wrapInto(const char* in, char* out, uint8_t stride,
                        uint8_t maxRows, uint8_t width) {
  auto ROW = [&](uint8_t r) -> char* { return out + (int)r * stride; };
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      ROW(row)[col] = 0;
      if (++row >= maxRows) return row;
      ROW(row)[0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && ROW(row)[0] != ' ')) ROW(row)[col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&ROW(row)[col], w, take); col += take; w += take; wlen -= take;
      ROW(row)[col] = 0;
      if (++row >= maxRows) return row;
      ROW(row)[0] = ' '; col = 1;
    }
    memcpy(&ROW(row)[col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { ROW(row)[col] = 0; row++; }
  return row;
}

// AskUserQuestion full-screen selection UI. Uses the whole 135×240
// sprite instead of the bottom 78px strip because we need room for
// question + N options + description of the highlighted one.
static void drawApprovalAsk() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);

  // Timer row
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
  spr.setCursor(4, 4);
  spr.printf("approve? %lus", (unsigned long)waited);

  JsonDocument doc;   // runtime parse → internal heap
  if (deserializeJson(doc, tama.askJson)) {
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(4, 20); spr.print("(askJson parse fail)");
    return;
  }
  JsonVariantConst q = doc[0];
  const char* qtext = q["question"] | "?";

  JsonArrayConst opts = q["options"];
  uint8_t cnt = opts.size();
  if (cnt == 0) return;
  uint8_t sel = askSel % cnt;

  // User-originated text (question, option labels, description) uses the
  // CJK font; scope it so the footer below keeps the default 6x8 ASCII font.
  {
    UiCjkFont uf(&spr);
    const uint16_t TEXT_PX  = W - 8;
    const uint16_t LABEL_PX = W - 8 - 12;   // after "> " prefix

    // Question (up to 3 wrapped rows at UI_CJK_LH pitch).
    char qrows[3][64];
    uint8_t qn = uiWrapUtf8(&spr, qtext, (char*)qrows, 64, 3, TEXT_PX);
    spr.setTextColor(p.text, p.bg);
    for (uint8_t i = 0; i < qn; i++) {
      spr.setCursor(4, 16 + i * UI_CJK_LH);
      uiPrintUtf8(&spr, qrows[i]);
    }
    int optY = 16 + qn * UI_CJK_LH + 6;

    // Options: single line per option, ellipsize to fit.
    const uint16_t OPT_LH = UI_CJK_LH + 2;
    for (uint8_t i = 0; i < cnt; i++) {
      bool selected = (i == sel);
      spr.setTextColor(selected ? p.text : p.textDim, p.bg);
      spr.setCursor(4, optY + i * OPT_LH);
      uiPrintUtf8(&spr, selected ? "> " : "  ");
      const char* lbl = opts[i]["label"] | "";
      char lbuf[64];
      strncpy(lbuf, lbl, sizeof(lbuf) - 1);
      lbuf[sizeof(lbuf) - 1] = 0;
      if (uiTextWidthUtf8(&spr, lbuf) > LABEL_PX) uiEllipsize(&spr, lbuf, sizeof(lbuf), LABEL_PX);
      uiPrintUtf8(&spr, lbuf);
    }

    // Description of the highlighted option, wrapped below the list.
    int dy = optY + cnt * OPT_LH + 6;
    if (dy < H - 32) {
      const char* desc = opts[sel]["description"] | "";
      char drows[4][64];
      uint8_t dn = uiWrapUtf8(&spr, desc, (char*)drows, 64, 4, TEXT_PX);
      spr.setTextColor(p.body, p.bg);
      for (uint8_t i = 0; i < dn && dy + (int)(i + 1) * UI_CJK_LH < H - 16; i++) {
        spr.setCursor(4, dy + i * UI_CJK_LH);
        uiPrintUtf8(&spr, drows[i]);
      }
    }
  }

  // Footer hints. B sends decision="once" (bridge doesn't accept
  // answers from device today) — user picks on the desktop UI;
  // device just shows what's being asked. Label as "skip" to match
  // actual effect.
  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12); spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12); spr.print("A: next");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 44, H - 12); spr.print("B: skip");
  }
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Tool name: size 2 when short, word-wrap to size 1 otherwise.
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  if (toolLen <= 10) {
    spr.setTextSize(2);
    spr.setCursor(4, H - AREA + 14);
    spr.print(tama.promptTool);
    spr.setTextSize(1);
  } else {
    char trows[2][24];
    uint8_t tn = wrapInto(tama.promptTool, (char*)trows, 24, 2, 22);
    for (uint8_t i = 0; i < tn; i++) {
      spr.setCursor(4, H - AREA + 18 + i * 8);
      spr.print(trows[i]);
    }
  }

  // Hint: CJK-aware word-wrap to 2 visible rows; ellipsize row 1 if the
  // wrap would have continued onto a third row.
  {
    UiCjkFont uf(&spr);
    const uint16_t HINT_PX = W - 8;
    spr.setTextColor(p.textDim, p.bg);
    char hrows[3][64];
    uint8_t hn = uiWrapUtf8(&spr, tama.promptHint, (char*)hrows, 64, 3, HINT_PX);
    uint8_t shown = hn > 2 ? 2 : hn;
    if (hn > 2) uiEllipsize(&spr, hrows[1], 64, HINT_PX);
    for (uint8_t i = 0; i < shown; i++) {
      spr.setCursor(4, H - AREA + 34 + i * UI_CJK_LH);
      uiPrintUtf8(&spr, hrows[i]);
    }
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 48, H - 12);
    spr.print("B: deny");
  }
}

// Landscape approval: direct-to-LCD like drawClock. Fills screen only on
// orientation transition; per-frame only repaints the waited counter and
// the response footer. clockOrient + paintedOrient are shared with the
// clock path but gated by landscapeApproval in the main loop so the two
// never overlap.
static void drawApprovalLandscape() {
  const Palette& p = characterPalette();
  M5.Lcd.setRotation(clockOrient);
  static uint32_t lastWaited = 0xFFFFFFFF;
  static bool lastSent = false;
  static uint16_t lastToolHash = 0;
  static uint16_t lastHintHash = 0;

  bool repaint = paintedOrient != clockOrient;
  if (repaint) {
    M5.Lcd.fillScreen(p.bg);
    paintedOrient = clockOrient;
    lastWaited = 0xFFFFFFFF;
    lastSent = !responseSent;   // force footer redraw
    lastToolHash = 0;
    lastHintHash = 0;
  }

  // Cheap FNV-1a over the strings to catch prompt transitions without
  // repainting every frame. Not crypto — just a change detector.
  auto fnv = [](const char* s) -> uint16_t {
    uint16_t h = 0x811c;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
  };
  uint16_t toolHash = fnv(tama.promptTool);
  uint16_t hintHash = fnv(tama.promptHint);

  // Landscape layout (240×135):
  //   y=2..10  timer "approve? Ns"    (size 1)
  //   y=16..32 tool name              (size 2, visual anchor)
  //   y=38..108 hint (up to 7 rows × 40 cols ≈ 280 chars)
  //   y=120    A/B button row

  // Timer row (1Hz)
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited != lastWaited) {
    lastWaited = waited;
    M5.Lcd.fillRect(0, 0, 240, 12, p.bg);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
    M5.Lcd.setCursor(4, 2);
    M5.Lcd.printf("approve? %lus", (unsigned long)waited);
  }

  // Tool name
  if (toolHash != lastToolHash) {
    lastToolHash = toolHash;
    M5.Lcd.fillRect(0, 14, 240, 20, p.bg);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(p.text, p.bg);
    M5.Lcd.setCursor(4, 16);
    M5.Lcd.print(tama.promptTool);
    M5.Lcd.setTextSize(1);
  }

  // Hint — CJK-aware wrap. Landscape has 72 vertical px (y=36..108) for the
  // hint. At UI_CJK_LH=15px that fits 4 rows. Wider max width accommodates
  // up to ~28 CJK glyphs or ~56 ASCII per row.
  if (hintHash != lastHintHash) {
    lastHintHash = hintHash;
    M5.Lcd.fillRect(0, 36, 240, 76, p.bg);
    UiCjkFont uf(&M5.Lcd);
    M5.Lcd.setTextColor(p.textDim, p.bg);
    const uint8_t  MAX_ROWS = 4;
    const uint16_t HINT_PX  = 240 - 8;
    char hrows[MAX_ROWS + 1][96];
    uint8_t hn = uiWrapUtf8(&M5.Lcd, tama.promptHint, (char*)hrows, 96, MAX_ROWS + 1, HINT_PX);
    uint8_t shown = hn > MAX_ROWS ? MAX_ROWS : hn;
    if (hn > MAX_ROWS) uiEllipsize(&M5.Lcd, hrows[MAX_ROWS - 1], 96, HINT_PX);
    for (uint8_t i = 0; i < shown; i++) {
      M5.Lcd.setCursor(4, 38 + i * UI_CJK_LH);
      uiPrintUtf8(&M5.Lcd, hrows[i]);
    }
  }

  // Footer
  if (responseSent != lastSent) {
    lastSent = responseSent;
    M5.Lcd.fillRect(0, 115, 240, 14, p.bg);
    M5.Lcd.setTextSize(1);
    if (responseSent) {
      M5.Lcd.setTextColor(p.textDim, p.bg);
      M5.Lcd.setCursor(4, 120);
      M5.Lcd.print("sent...");
    } else {
      M5.Lcd.setTextColor(GREEN, p.bg);
      M5.Lcd.setCursor(4, 120);
      M5.Lcd.print("A: approve");
      M5.Lcd.setTextColor(HOT, p.bg);
      M5.Lcd.setCursor(240 - 52, 120);
      M5.Lcd.print("B: deny");
    }
  }

  M5.Lcd.setRotation(0);
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1); spr.printf("Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(6, y + 10);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y + 20);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += 12;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap");
  ln(p.textDim, " refills to full"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any button = wake"); gap();

  ln(p.textDim, "A: screens  B: page");
  ln(p.textDim, "hold A: menu");
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — owner/pet names may be UTF-8 so
  // scope the CJK font to them; the "n/m" counter stays ASCII.
  {
    UiCjkFont uf(&spr);
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, y + 2);
    if (ownerName()[0]) {
      char hdr[80];
      snprintf(hdr, sizeof(hdr), "%s's %s", ownerName(), petName());
      uiPrintUtf8(&spr, hdr);
    } else {
      uiPrintUtf8(&spr, petName());
    }
  }
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  if (tama.promptId[0]) {
    if (promptIsAsk()) drawApprovalAsk();
    else               drawApproval();
    return;
  }
  const Palette& p = characterPalette();
  // CJK font is 14px tall with 15px pitch. HUD spans y=146..240 (6
  // rows + slack). Character body never extends below y=140 (GIFs
  // bottom-out at 140, ASCII buddy 2× body is 60..140 with padding
  // 140..164). HUD's fillRect into 146..240 only overwrites that
  // padding band + the gap below the character, not ink.
  const int SHOW = 6;
  const int LH   = UI_CJK_LH;
  const int AREA = SHOW * LH + 4;
  const uint16_t TEXT_PX = W - 8;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  UiCjkFont uf(&spr);

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, H - LH - 2);
    uiPrintUtf8(&spr, tama.msg);
    return;
  }

  // HUD shows ONLY the latest stored entry. Bridge sends a transcript
  // with multiple entries (user prompt, assistant response, a "done"
  // summary); we wrap just the last one across the 6-row HUD window
  // rather than mixing them, which keeps streaming-response updates
  // readable and avoids reflow between prompt and response slots.
  extern char (*g_hudDisp)[HUD_ROW_STRIDE];
  if (!g_hudDisp) return;
  char (*disp)[HUD_ROW_STRIDE] = g_hudDisp;
  const char* latest = tama.lines[tama.nLines - 1];
  uint8_t nDisp = uiWrapUtf8(&spr, latest, (char*)disp,
                             HUD_ROW_STRIDE, HUD_MAX_ROWS, TEXT_PX);

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  for (int i = 0; start + i < end; i++) {
    // All rows belong to the same (latest) message — uniform color.
    // Scrolled-back rows dim as a cue that newer content is below.
    spr.setTextColor((msgScroll == 0) ? p.text : p.textDim, p.bg);
    spr.setCursor(4, H - AREA + 2 + i * LH);
    uiPrintUtf8(&spr, disp[start + i]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 18, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

#define BOOT_CHK(tag) do { \
    bool ok_i = heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true); \
    bool ok_p = heap_caps_check_integrity(MALLOC_CAP_SPIRAM,   true); \
    Serial.printf("[boot] %s heap(int)=%s heap(psram)=%s\n", \
                  tag, ok_i ? "ok" : "CORRUPT", ok_p ? "ok" : "CORRUPT"); \
    Serial.flush(); \
  } while (0)

void setup() {
  Serial.begin(115200);
  delay(200);  // let USB serial attach before the first diagnostic
  // Pin the default malloc() path to internal SRAM. ESP32 + PSRAM heap
  // has a cache race with NVS flash reads (which disable cache briefly)
  // — when a transient default-malloc (Preferences internal, BLE init
  // bookkeeping, …) spills into PSRAM during that window, the PSRAM
  // free-list metadata gets clobbered. Our own code targets PSRAM
  // explicitly via heap_caps_malloc(SPIRAM) / psAlloc, which bypasses
  // this threshold.
  heap_caps_malloc_extmem_enable(0x40000000);
  Serial.printf("[boot] psram size=%u free=%u internal heap free=%u\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
                (unsigned)ESP.getFreeHeap());

  // ─── Phase 1: every PSRAM allocation, BEFORE any flash read ─────
  // Flash reads (NVS, LittleFS) briefly disable PSRAM cache on this
  // chip. After the first such read, TLSF metadata on the PSRAM heap
  // becomes unsafe to walk, and any later psAlloc crashes inside the
  // allocator. Front-load all PSRAM demands here so later code never
  // touches the PSRAM heap's bookkeeping.
  tamaInit(&tama);
  characterPreallocTables();   // textStates + gifPaths
  dataPrealloc();              // USB+BT line buffers
  hudPrealloc();               // transcript display scratch
  psArenaInit(32 * 1024);      // runtime JSON arena
  g_askJsonCap = 4096;
  g_askJsonBuf = (char*)psAlloc(g_askJsonCap);
  // Snapshot PSRAM stats before the heap goes bad. Info pages use these.
  g_psramTotalCached = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  g_psramFreeAtBoot  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  Serial.printf("[boot] psram arena=%uB askJsonBuf=%uB total=%u freeAtBoot=%u\n",
                (unsigned)psArenaCapacity(), (unsigned)g_askJsonCap,
                (unsigned)g_psramTotalCached, (unsigned)g_psramFreeAtBoot);

  // ─── Phase 2: hardware / flash-backed init ───────────────────────
  auto cfg = M5.config();
  StickCP2.begin(cfg);
  M5.Lcd.setRotation(0);
  M5.Imu.begin();
  M5.Speaker.begin();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // off
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  uiSetCjkRoute(settings().cjkJa, settings().cjkCn, settings().cjkKr);
  petNameLoad();
  buddyInit();
  applyBrightness();

  // Sprite stays in DMA-capable internal SRAM. Trying to route the 64 KB
  // RGB565 buffer to PSRAM corrupts the PSRAM heap's TLSF metadata on
  // this chip (the exact mechanism is unclear — possibly the large
  // contiguous allocation triggers a cache invalidation race). With BLE
  // not yet started we have ~200 KB internal free; 64 KB fits easily.
  spr.setPsram(false);
  spr.createSprite(W, H);
  Serial.printf("[boot] psram free=%u heap free=%u\n",
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());
  // characterInit reads manifest.json from LittleFS. Tables were
  // pre-allocated in Phase 1; this call only fills them.
  characterInit(nullptr);

  // Flip the UART kill switch. After startBt() the UART driver's
  // tx_mux is zeroed by Bluedroid/NimBLE init on this chip/framework
  // combo (arduino-esp32 3.x + PSRAM). All downstream Serial output
  // goes through LOG_* macros that no-op when g_uartAlive is false.
  Serial.flush();
  g_uartAlive = false;
  startBt();
  // Probe (ets_printf bypasses the dead UART): confirm the arena is
  // still functional post-BLE. Allocating from the arena doesn't walk
  // the corrupted PSRAM heap free-list.
  void* p1 = psArenaAlloc(1024);
  ROMLOGF("[probe] arena alloc 1024 -> %p  used=%u\n",
          p1, (unsigned)psArenaUsed());
  psArenaReset();
  ROMLOGF("[probe] arena reset, used=%u\n", (unsigned)psArenaUsed());
  // ─── Post-BLE: PSRAM heap is poisoned at the free-list level. ────
  // Existing PSRAM allocations still work; new ones must not happen.
  // All runtime malloc/free goes through the internal heap instead.

  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    if (ownerName()[0]) {
      // Owner / pet names may be UTF-8 — route per-codepoint through the
      // CJK fonts at 2x for prominence. drawString can't route, so we
      // measure+setCursor manually to center-align.
      UiCjkFont uf(&spr);
      spr.setTextSize(2);
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      const int GLYPH_H = UI_CJK_LH * 2;
      spr.setTextColor(p.text, p.bg);
      spr.setCursor(W/2 - uiTextWidthUtf8(&spr, line) / 2, (H/2 - 16) - GLYPH_H/2);
      uiPrintUtf8(&spr, line);
      spr.setTextColor(p.body, p.bg);
      spr.setCursor(W/2 - uiTextWidthUtf8(&spr, petName()) / 2, (H/2 + 16) - GLYPH_H/2);
      uiPrintUtf8(&spr, petName());
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextSize(2);
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  // Setup complete. No Serial output — UART is unusable post-BLE init.
}

void loop() {
  M5.update();
  t++;
  uint32_t now = millis();

  bleDrainEvents();     // no-ops silently once UART is dead
  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on attention, otherwise off
  if (activeState == P_ATTENTION && settings().led) {
    digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      LOGLN("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    askSel = 0;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // Power button (left side): short-press toggles screen off.
  // Long-press (~6s) still powers off the device via hardware.
  if (M5.BtnPWR.wasClicked()) {
    if (screenOff) {
      wake();
    } else {
      M5.Display.sleep();
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    LOGLN(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt && promptIsAsk()) {
        // AskUserQuestion: cycle through options. Sending happens on B.
        JsonDocument d;   // runtime parse → internal heap
        if (deserializeJson(d, tama.askJson) == DeserializationError::Ok) {
          uint8_t cnt = d[0]["options"].size();
          if (cnt > 0) askSel = (askSel + 1) % cnt;
        }
        beep(1800, 30);
      } else if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB long-press → reverse-scroll the HUD transcript. Short-press
  // (below, on release) steps msgScroll forward; holding B rolls it
  // back toward the newest line at ~5 Hz. Only active in the normal
  // HUD context; in menus / prompts / info pages the release path
  // runs normally (we don't set btnBLong, so it doesn't swallow).
  {
    static uint32_t _btnBScrollAt = 0;
    bool inHud = !inPrompt && !menuOpen && !settingsOpen && !resetOpen
               && displayMode == DISP_NORMAL;
    if (inHud && !swallowBtnB && M5.BtnB.pressedFor(600)) {
      uint32_t now = millis();
      if (now - _btnBScrollAt >= 200) {
        btnBLong = true;
        if (msgScroll > 0) { msgScroll--; beep(1400, 20); }
        _btnBScrollAt = now;
      }
    }
  }

  // BtnB release: short-press → confirm / scroll-forward / page, depending
  // on context. Skipped if the long-press scroll-back ran (btnBLong).
  if (M5.BtnB.wasReleased()) {
    if (swallowBtnB) { swallowBtnB = false; btnBLong = false; }
    else if (btnBLong) { btnBLong = false; }
    else
    if (inPrompt && promptIsAsk()) {
      // AskUserQuestion confirm. The Hardware Buddy BLE bridge in
      // Claude Desktop does not (as of 2026-04) wire up the
      // canUseTool `updatedInput.answers` path for AskUserQuestion —
      // tried answers:{}, decision:"answer"+answers, decision:"once"
      // +answers, decision:"allow"+updatedInput.answers, and a new
      // cmd:"answer" verb, all ignored. Fall back to "once" so the
      // prompt at least resolves (empty answer); user answers on the
      // desktop UI. The option cycle is kept as visual/future-proof:
      // the day bridge ships a real protocol, swap the cmd body here.
      char cmd[96];
      snprintf(cmd, sizeof(cmd),
               "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}",
               tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnApproval((millis() - promptArrivedMs) / 1000);
      beep(2400, 60);
    } else if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      beep(2400, 30);
      // Cap at HUD_MAX_ROWS - 1; drawHUD clamps msgScroll to maxBack
      // anyway so stepping one past the actual bottom just wraps on
      // the next press.
      msgScroll = (msgScroll >= HUD_MAX_ROWS - 1) ? 0 : msgScroll + 1;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;
  // Orientation detection runs whenever a landscape-capable screen is
  // active. Today that's charging-clock and any pending approval
  // (including the brief "sent..." tail after the user decides but
  // before the desktop clears promptId). Those two are mutually
  // exclusive by construction — clocking requires !inPrompt.
  bool approvalUp = tama.promptId[0];
  bool orientTracked = clocking || approvalUp;
  if (orientTracked) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; }
  // Always clear paintedOrient when we're in portrait — drawClock does
  // this in its own portrait branch, but the approval portrait path
  // runs via sprite and would otherwise leave paintedOrient stale, so
  // the next landscape entry skips its repaint and shows a blank frame.
  if (clockOrient == 0) paintedOrient = 0;
  bool landscapeClock    = clocking && clockOrient != 0;
  bool landscapeApproval = approvalUp && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (napping || screenOff || landscapeClock || landscapeApproval) {
    // skip sprite render — face-down, powered off, or a direct-to-LCD
    // landscape screen is about to paint (clock or approval)
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (landscapeClock) {
    drawClock();
  } else if (landscapeApproval) {
    drawApprovalLandscape();
  } else if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    spr.pushSprite(0, 0);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5.Display.setBrightness(20);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    M5.Display.sleep();
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
