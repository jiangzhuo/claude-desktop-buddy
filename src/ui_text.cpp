#include "ui_text.h"
#include <string.h>

// Decode one UTF-8 codepoint starting at p. Returns byte length consumed
// (1..4, or 1 for a malformed lead). Writes the decoded codepoint to *cp.
static inline uint8_t utf8Decode(const char* p, uint32_t* cp) {
  uint8_t c = (uint8_t)*p;
  if (c < 0x80)            { *cp = c;           return 1; }
  if ((c & 0xE0) == 0xC0)  { *cp = c & 0x1F;    goto c2; }
  if ((c & 0xF0) == 0xE0)  { *cp = c & 0x0F;    goto c3; }
  if ((c & 0xF8) == 0xF0)  { *cp = c & 0x07;    goto c4; }
  *cp = 0xFFFD;
  return 1;
c4:
  if ((p[1] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
  *cp = (*cp << 6) | (p[1] & 0x3F);
  if ((p[2] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
  *cp = (*cp << 6) | (p[2] & 0x3F);
  if ((p[3] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
  *cp = (*cp << 6) | (p[3] & 0x3F);
  return 4;
c3:
  if ((p[1] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
  *cp = (*cp << 6) | (p[1] & 0x3F);
  if ((p[2] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
  *cp = (*cp << 6) | (p[2] & 0x3F);
  return 3;
c2:
  if ((p[1] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
  *cp = (*cp << 6) | (p[1] & 0x3F);
  return 2;
}

static bool g_rtJa = true;
static bool g_rtCn = true;
static bool g_rtKr = true;

void uiSetCjkRoute(bool ja, bool cn, bool kr) {
  g_rtJa = ja;
  g_rtCn = cn;
  g_rtKr = kr;
}

// Short-circuits: a font is considered "available" only when both the
// compile-time region switch AND the runtime toggle are on.
#if UI_CJK_JA
  #define JA_ENABLED() (g_rtJa)
#else
  #define JA_ENABLED() (false)
#endif
#if UI_CJK_CN
  #define CN_ENABLED() (g_rtCn)
#else
  #define CN_ENABLED() (false)
#endif
#if UI_CJK_KR
  #define KR_ENABLED() (g_rtKr)
#else
  #define KR_ENABLED() (false)
#endif

const lgfx::IFont* uiDefaultCjkFont() {
#if UI_CJK_JA
  if (JA_ENABLED()) return &fonts::efontJA_14;
#endif
#if UI_CJK_CN
  if (CN_ENABLED()) return &fonts::efontCN_14;
#endif
#if UI_CJK_KR
  if (KR_ENABLED()) return &fonts::efontKR_14;
#endif
  // Fall back to whatever is compiled in even if runtime flag is off, so
  // ASCII still renders rather than disappearing entirely.
#if UI_CJK_JA
  return &fonts::efontJA_14;
#elif UI_CJK_CN
  return &fonts::efontCN_14;
#elif UI_CJK_KR
  return &fonts::efontKR_14;
#else
  return &fonts::Font0;
#endif
}

const lgfx::IFont* uiFontForCp(uint32_t cp) {
#if UI_CJK_JA
  // Hiragana + Katakana + Katakana Phonetic Extensions: JA-only.
  if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) {
    if (JA_ENABLED()) return &fonts::efontJA_14;
  }
#endif
#if UI_CJK_KR
  // Hangul Jamo, Hangul Compatibility Jamo, Hangul Syllables: KR-only.
  if ((cp >= 0xAC00 && cp <= 0xD7AF)
   || (cp >= 0x1100 && cp <= 0x11FF)
   || (cp >= 0x3130 && cp <= 0x318F)) {
    if (KR_ENABLED()) return &fonts::efontKR_14;
  }
#endif
  // CJK Unified Ideographs (+ Ext A, Compatibility): CN has the widest
  // coverage; fall back to JA or KR if CN is off (runtime or compile).
  if ((cp >= 0x4E00 && cp <= 0x9FFF)
   || (cp >= 0x3400 && cp <= 0x4DBF)
   || (cp >= 0xF900 && cp <= 0xFAFF)) {
#if UI_CJK_CN
    if (CN_ENABLED()) return &fonts::efontCN_14;
#endif
#if UI_CJK_JA
    if (JA_ENABLED()) return &fonts::efontJA_14;
#endif
#if UI_CJK_KR
    if (KR_ENABLED()) return &fonts::efontKR_14;
#endif
  }
  return uiDefaultCjkFont();
}

// Walk a UTF-8 string, batching contiguous same-font codepoints into runs
// and invoking `emit(font, utf8_bytes, byte_len)` for each run.
template<typename Emit>
static void forEachRun(const char* s, Emit emit) {
  if (!s || !*s) return;
  const lgfx::IFont* curFont = nullptr;
  char run[96];
  uint16_t runLen = 0;

  auto flush = [&]() {
    if (runLen == 0) return;
    run[runLen] = 0;
    emit(curFont, run, runLen);
    runLen = 0;
  };

  const char* p = s;
  while (*p) {
    uint32_t cp;
    uint8_t n = utf8Decode(p, &cp);
    const lgfx::IFont* f = uiFontForCp(cp);
    if (f != curFont || (uint32_t)runLen + n + 1 > sizeof(run)) {
      flush();
      curFont = f;
    }
    memcpy(run + runLen, p, n);
    runLen += n;
    p += n;
  }
  flush();
}

void uiPrintUtf8(LovyanGFX* g, const char* s) {
  if (!g) return;
  forEachRun(s, [&](const lgfx::IFont* f, const char* bytes, uint16_t /*n*/) {
    if (f) g->setFont(f);
    g->print(bytes);
  });
}

uint16_t uiTextWidthUtf8(LovyanGFX* g, const char* s) {
  if (!g) return 0;
  const lgfx::IFont* origFont = g->getFont();
  uint32_t total = 0;
  forEachRun(s, [&](const lgfx::IFont* f, const char* bytes, uint16_t /*n*/) {
    if (f) g->setFont(f);
    total += g->textWidth(bytes);
  });
  g->setFont(origFont);
  return (uint16_t)(total > 0xFFFF ? 0xFFFF : total);
}

// UTF-8 + pixel-aware greedy wrap. Uses uiTextWidthUtf8 to measure so the
// widths match what uiPrintUtf8 will actually draw (each codepoint's
// routed font).
uint8_t uiWrapUtf8(LovyanGFX* g, const char* in, char* out,
                   uint16_t stride, uint8_t maxRows, uint16_t maxPx) {
  if (!g || !in || !out || stride < 2 || maxRows == 0) return 0;
  auto ROW = [&](uint8_t r) -> char* { return out + (size_t)r * stride; };

  uint8_t  row  = 0;
  uint16_t rLen = 0;
  uint16_t rPx  = 0;
  ROW(0)[0] = 0;

  auto closeRow = [&]() {
    if (rLen >= stride) rLen = stride - 1;
    ROW(row)[rLen] = 0;
    row++;
    if (row < maxRows) ROW(row)[0] = 0;
    rLen = 0;
    rPx  = 0;
  };

  auto appendRaw = [&](const char* tok, uint16_t tokLen, uint16_t tokPx) -> bool {
    if ((uint32_t)rLen + tokLen + 1 > stride) return false;
    memcpy(ROW(row) + rLen, tok, tokLen);
    rLen += tokLen;
    ROW(row)[rLen] = 0;
    rPx += tokPx;
    return true;
  };

  auto widthOf = [&](const char* tok, uint16_t tokLen) -> uint16_t {
    char tmp[96];
    uint16_t m = tokLen < sizeof(tmp) - 1 ? tokLen : sizeof(tmp) - 1;
    memcpy(tmp, tok, m);
    tmp[m] = 0;
    return uiTextWidthUtf8(g, tmp);
  };

  auto hardBreakAtom = [&](const char* atom, uint16_t atomLen) {
    uint16_t i = 0;
    while (i < atomLen && row < maxRows) {
      uint32_t cp;
      uint8_t qLen = utf8Decode(atom + i, &cp);
      if (i + qLen > atomLen) qLen = (uint8_t)(atomLen - i);
      char qb[5];
      memcpy(qb, atom + i, qLen);
      qb[qLen] = 0;
      uint16_t qpx = uiTextWidthUtf8(g, qb);
      if (rPx + qpx > maxPx && rLen > 0) {
        closeRow();
        if (row >= maxRows) return;
      }
      if (!appendRaw(qb, qLen, qpx)) {
        if (rLen == 0) return;   // single cp wider than budget AND stride
        closeRow();
        if (row >= maxRows) return;
        appendRaw(qb, qLen, qpx);
      }
      i += qLen;
    }
  };

  const char* p = in;
  while (*p && row < maxRows) {
    // Collapse whitespace runs; drop leading whitespace of a row.
    if (*p == ' ' || *p == '\t') {
      while (*p == ' ' || *p == '\t') p++;
      if (rLen == 0) continue;
      uint16_t spx = widthOf(" ", 1);
      if (rPx + spx > maxPx) { closeRow(); continue; }
      appendRaw(" ", 1, spx);
      continue;
    }

    // Atom = ASCII run until space / non-ASCII, or a single multi-byte cp.
    const char* aStart = p;
    uint16_t aLen;
    uint8_t c = (uint8_t)*p;
    if (c < 0x80) {
      while (*p && *p != ' ' && *p != '\t' && (uint8_t)*p < 0x80) p++;
      aLen = (uint16_t)(p - aStart);
    } else {
      uint32_t cp;
      aLen = utf8Decode(p, &cp);
      for (uint16_t i = 0; i < aLen; i++) {
        if (!p[i]) { aLen = i; break; }
      }
      p += aLen;
    }
    if (aLen == 0) { p++; continue; }

    uint16_t atomPx = widthOf(aStart, aLen);
    bool fits = (rPx + atomPx <= maxPx) && (rLen + aLen + 1 <= stride);
    if (fits) {
      appendRaw(aStart, aLen, atomPx);
      continue;
    }

    if (rLen > 0) {
      closeRow();
      if (row >= maxRows) return row;
      fits = (rPx + atomPx <= maxPx) && (rLen + aLen + 1 <= stride);
      if (fits) {
        appendRaw(aStart, aLen, atomPx);
        continue;
      }
    }
    hardBreakAtom(aStart, aLen);
  }

  if (rLen > 0 && row < maxRows) {
    ROW(row)[rLen] = 0;
    row++;
  }
  return row;
}

void uiEllipsize(LovyanGFX* g, char* row, uint16_t stride, uint16_t maxPx) {
  if (!g || !row || stride < 3) return;
  uint16_t ellPx = uiTextWidthUtf8(g, "..");
  if (ellPx > maxPx) { row[0] = 0; return; }

  uint16_t len = (uint16_t)strlen(row);
  while (len > 0 && row[len - 1] == ' ') row[--len] = 0;

  uint16_t curPx = uiTextWidthUtf8(g, row);
  while (len > 0 && curPx + ellPx > maxPx) {
    len--;
    while (len > 0 && ((uint8_t)row[len] & 0xC0) == 0x80) len--;
    row[len] = 0;
    curPx = uiTextWidthUtf8(g, row);
  }

  if ((uint32_t)len + 3 > stride) return;
  row[len++] = '.';
  row[len++] = '.';
  row[len]   = 0;
}

UiCjkFont::UiCjkFont(LovyanGFX* g)
  : _g(g), _savedFont(g->getFont()),
    _savedSx(g->getTextSizeX()), _savedSy(g->getTextSizeY()) {
  _g->setFont(uiDefaultCjkFont());
  _g->setTextSize(1);
}

UiCjkFont::~UiCjkFont() {
  _g->setFont(_savedFont);
  _g->setTextSize(_savedSx, _savedSy);
}
