#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "psram_util.h"
#include "safe_log.h"
#include "xfer.h"

// Large fields live in PSRAM — lines[] and promptHint[] are the bulk of
// TamaState and are only written during live sessions. Allocated once in
// tamaInit(). Small fields (counters, flags, short strings) stay inline
// so the fast path stays cache-friendly.
//
// lines[][] width = 2048 bytes ≈ 680 CJK characters per entry. Bridge
// occasionally sends essay-length messages; 256 or smaller caps them
// mid-character, leaving malformed UTF-8 tails that render as "missing
// glyph" boxes.
static const size_t TAMA_LINE_CAP = 2048;

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     (*lines)[TAMA_LINE_CAP];   // [8][TAMA_LINE_CAP] on PSRAM
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[48];
  char*    promptHint;       // [1024] on PSRAM — tamaInit()
  // AskUserQuestion payload captured from the preceding evt:"turn"
  // tool_use event. Serialized questions array (subset of the fields —
  // see captureAskQuestions). NULL when no questions queued.
  char*    askJson;          // ps_alloc'd per prompt
};

inline void tamaInit(TamaState* t) {
  // Place the transcript cache in INTERNAL heap, not PSRAM. tama.lines
  // is read once per frame (only the last slot, via drawHUD) but written
  // on heartbeats via tamaUtf8Copy. Short entries that aren't read every
  // frame get evicted from the PSRAM cache and then re-read as zeros on
  // ESP32's PSRAM controller — experimentally observed as "lines[0] and
  // lines[2] going empty between heartbeats". Internal SRAM has no such
  // cache staleness. 8 × 2048 = 16 KB is trivial vs ~100 KB free heap.
  if (!t->lines)       t->lines       = (char (*)[TAMA_LINE_CAP])calloc(8, TAMA_LINE_CAP);
  if (!t->promptHint)  t->promptHint  = (char*)calloc(1, 1024);
}

// Copy src to dst (capacity cap bytes including NUL), walking by UTF-8
// codepoint. Stops before copying a codepoint that wouldn't fit,
// guaranteeing dst ends at a valid codepoint boundary with no partial
// byte sequences left behind.
inline void tamaUtf8Copy(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  dst[0] = 0;
  if (!src) return;
  size_t outPos = 0;
  size_t i = 0;
  while (src[i]) {
    uint8_t c = (uint8_t)src[i];
    size_t need;
    if      (c < 0x80)  need = 1;
    else if (c < 0xC0) { i++; continue; }  // orphan continuation, skip
    else if (c < 0xE0)  need = 2;
    else if (c < 0xF0)  need = 3;
    else                need = 4;
    if (outPos + need >= cap) break;        // leave room for NUL
    for (size_t k = 0; k < need; k++) dst[outPos + k] = src[i + k];
    outPos += need;
    i += need;
  }
  dst[outPos] = 0;
}

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Claude connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

// AskUserQuestion tool_use turns carry the questions+options we need
// to render the selection UI. They arrive BEFORE the matching prompt
// event. Use ArduinoJson's Filter feature to extract only the fields
// we care about — this keeps DOM heap peak <2KB even when the raw
// line has multi-KB `thinking` content we'd otherwise skip.
static void captureAskQuestions(const char* line, TamaState* out) {
  // Runtime JSON parsing uses the default (internal heap) allocator.
  // PSRAM arena was tried here but ArduinoJson's pool-moves-on-realloc
  // design breaks when the StringPool is relocated: VariantPool slots
  // hold raw pointers into the StringPool, and they dangle after a
  // move. Internal heap is plenty — each parse peaks at a few KB.
  JsonDocument filter;
  filter["content"][0]["input"]["questions"][0]["question"]     = true;
  filter["content"][0]["input"]["questions"][0]["header"]       = true;
  filter["content"][0]["input"]["questions"][0]["multiSelect"]  = true;
  filter["content"][0]["input"]["questions"][0]["options"][0]["label"]       = true;
  filter["content"][0]["input"]["questions"][0]["options"][0]["description"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line,
      DeserializationOption::Filter(filter));
  if (err) {
#ifdef BUDDY_DUMP_RAW
    LOGF("[ask] filter parse err: %s\n", err.c_str());
#endif
    return;
  }

  JsonVariantConst questions = doc["content"][0]["input"]["questions"];
  if (questions.isNull() || !questions.is<JsonArrayConst>()) return;

  // askJson lives in a dedicated pre-BLE PSRAM buffer (g_askJsonBuf).
  // Length-prefixed: we just memcpy the serialized JSON into it. The
  // previous contents are overwritten on every new capture, which
  // matches the lifecycle of askJson pointers in main.cpp.
  extern char*  g_askJsonBuf;
  extern size_t g_askJsonCap;
  out->askJson = nullptr;
  if (!g_askJsonBuf) return;
  size_t need = measureJson(questions) + 1;
  if (need > g_askJsonCap) {
#ifdef BUDDY_DUMP_RAW
    LOGF("[ask] %u > askJson buf %u\n", (unsigned)need, (unsigned)g_askJsonCap);
#endif
    return;
  }
  out->askJson = g_askJsonBuf;
  serializeJson(questions, out->askJson, need);

#ifdef BUDDY_DUMP_RAW
  LOGF("[ask] captured %u bytes, %u question(s) (heap %uKB)\n",
       (unsigned)(need - 1),
       (unsigned)questions.size(),
       (unsigned)(ESP.getFreeHeap() / 1024));
#endif
}

static void _applyJson(const char* line, TamaState* out) {
#ifdef BUDDY_DUMP_RAW
  LOGPRINT("[raw] ");
  LOGLN(line);
#endif

  // evt:* events are informational — they don't drive session state.
  // AskUserQuestion turns though carry the options payload we need,
  // so pick those out via cheap strstr before doing any JSON parsing.
  if (strncmp(line, "{\"evt\"", 6) == 0) {
    if (strstr(line, "\"AskUserQuestion\"")) {
      captureAskQuestions(line, out);
    }
    _lastLiveMs = millis();
    return;
  }

  JsonDocument doc;   // runtime parse → internal heap (see note above)
  if (deserializeJson(doc, line)) return;
  if (xferCommand(doc)) { _lastLiveMs = millis(); return; }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components including weekday.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt; gmtime_r(&local, &lt);
    m5::rtc_time_t tm((int8_t)lt.tm_hour, (int8_t)lt.tm_min, (int8_t)lt.tm_sec);
    m5::rtc_date_t dt((int16_t)(lt.tm_year + 1900), (int8_t)(lt.tm_mon + 1),
                      (int8_t)lt.tm_mday, (int8_t)lt.tm_wday);
    M5.Rtc.setTime(&tm);
    M5.Rtc.setDate(&dt);
    extern uint32_t _clkLastRead;
    _clkLastRead = 0;   // force re-read so _clkDt and _rtcValid agree
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char* m = doc["msg"];
  if (m) { strncpy(out->msg, m, sizeof(out->msg)-1); out->msg[sizeof(out->msg)-1]=0; }
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    // Bridge maintains a FIFO window of the last few transcript items —
    // on every heartbeat the array SHIFTS as new messages come in and
    // old ones fall off. Comparing index-to-index therefore reports
    // "everything changed" on every shift, flashing the HUD.
    //
    // Treat our local store as a SET instead: an incoming entry that
    // matches anything we already have is a no-op. An entry that is
    // the previous "latest" grown longer is a streaming extension —
    // update in place. A genuinely new entry gets appended (shifting
    // the oldest out when full). Only genuine appends bump lineGen.
    bool bumpedGen = false;
    for (JsonVariant v : la) {
      const char* s = v.as<const char*>();
      if (!s || !s[0]) continue;
      size_t sLen = strlen(s);

      // Already in the set?
      bool found = false;
      for (uint8_t i = 0; i < out->nLines; i++) {
        size_t stLen = strnlen(out->lines[i], TAMA_LINE_CAP - 1);
        if (stLen == sLen && memcmp(s, out->lines[i], stLen) == 0) {
          found = true;
          break;
        }
      }
      if (found) continue;

      // Streaming extension of our latest slot?
      if (out->nLines > 0) {
        uint8_t last = out->nLines - 1;
        size_t stLen = strnlen(out->lines[last], TAMA_LINE_CAP - 1);
        if (stLen > 0 && sLen > stLen &&
            memcmp(s, out->lines[last], stLen) == 0) {
          tamaUtf8Copy(out->lines[last], TAMA_LINE_CAP, s);
          continue;   // in-place update; keep msgScroll steady
        }
      }

      // Novel message — append, evicting oldest if the ring is full.
      if (out->nLines == 8) {
        for (uint8_t i = 0; i < 7; i++) {
          memcpy(out->lines[i], out->lines[i + 1], TAMA_LINE_CAP);
        }
        out->nLines = 7;
      }
      tamaUtf8Copy(out->lines[out->nLines], TAMA_LINE_CAP, s);
      out->nLines++;
      bumpedGen = true;
      ROMLOGF("[append] slot=%u len=%u\n", out->nLines - 1, (unsigned)sLen);
    }
    if (bumpedGen) out->lineGen++;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
    strncpy(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool)-1); out->promptTool[sizeof(out->promptTool)-1]=0;
    strncpy(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint)-1); out->promptHint[sizeof(out->promptHint)-1]=0;
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
    // askJson points into g_askJsonBuf — no free, just drop the pointer.
    out->askJson = nullptr;
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

// Line buffers live in PSRAM — one per transport, 4 KB each. Allocated
// eagerly via dataPrealloc() at setup time BEFORE the BT controller
// starts; allocating on the PSRAM heap afterwards is unsafe (see data.h
// top-of-file note on PSRAM cache race).
struct _LineBuf {
  static const size_t CAP = 4096;
  char*    buf = nullptr;
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    if (!buf) return;
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < CAP-1) {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf _usbLine, _btLine;

inline void dataPrealloc() {
  if (!_usbLine.buf) _usbLine.buf = (char*)psAlloc(_LineBuf::CAP);
  if (!_btLine.buf)  _btLine.buf  = (char*)psAlloc(_LineBuf::CAP);
}

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  // Only touch USB serial while UART driver is alive — post-BLE its
  // rx_mux becomes NULL and any Serial.read() asserts. See safe_log.h.
  if (g_uartAlive) _usbLine.feed(Serial, out);
  // BLE ring buffer is drained manually since it's not a Stream.
  while (_btLine.buf && bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < _LineBuf::CAP - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
