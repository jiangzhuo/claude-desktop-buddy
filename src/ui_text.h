#pragma once
#include <M5StickCPlus2.h>
#include <stdint.h>

// Which efont regions to compile in. Default: JA + CN + KR (~910KB total).
// Referencing a font symbol pulls its full bitmap into the app partition —
// partitions_buddy.csv carves out 3MB of app to fit all three. Set any of
// these to 0 via build_flags to shrink the binary.
#ifndef UI_CJK_JA
#define UI_CJK_JA 1
#endif
#ifndef UI_CJK_CN
#define UI_CJK_CN 1
#endif
#ifndef UI_CJK_KR
#define UI_CJK_KR 1
#endif

// Line height to use when CJK fonts are active (14px glyph + 1px leading).
#ifndef UI_CJK_LH
#define UI_CJK_LH 15
#endif

// Runtime on/off for each CJK font. Compiled-in fonts stay linked regardless,
// but when a flag is false uiFontForCp won't route codepoints to that font —
// they fall through to the next-best compiled+enabled font (or render blank
// glyphs if nothing can represent them). Call after settingsLoad().
void uiSetCjkRoute(bool ja, bool cn, bool kr);

// Returns the best-match efont for a Unicode codepoint based on the
// enabled UI_CJK_* regions AND the runtime uiSetCjkRoute flags.
// Hiragana/Katakana → JA, Hangul → KR, CJK Unified Ideographs → CN
// (largest coverage); ASCII / other → the default CJK font.
const lgfx::IFont* uiFontForCp(uint32_t cp);

// The font used for ASCII / unknown codepoints and as the baseline for
// layout calculations. First enabled of JA, CN, KR, else Font0.
const lgfx::IFont* uiDefaultCjkFont();

// Print UTF-8 text starting at the target's current cursor, switching the
// font on the fly for each codepoint run. Side-effect: the target's font
// may be any of the CJK fonts when this returns (caller is responsible
// for restoring if needed — UiCjkFont handles this at scope exit).
void uiPrintUtf8(LovyanGFX* g, const char* s);

// Sum of pixel widths of each UTF-8 codepoint run under its routed font.
// Temporarily swaps fonts to measure; restores the target's original font
// before returning.
uint16_t uiTextWidthUtf8(LovyanGFX* g, const char* s);

// UTF-8 + pixel-aware greedy wrap using per-codepoint font routing.
// Writes up to maxRows NUL-terminated strings into out[row * stride].
uint8_t uiWrapUtf8(LovyanGFX* g, const char* in, char* out,
                   uint16_t stride, uint8_t maxRows, uint16_t maxPx);

// Trim row (in place) and append ".." so uiTextWidthUtf8(row) <= maxPx,
// backing off whole UTF-8 codepoints so nothing is left half-encoded.
void uiEllipsize(LovyanGFX* g, char* row, uint16_t stride, uint16_t maxPx);

// RAII: establish the default CJK font + size 1 as the baseline so any
// residual spr.print / spr.drawString inside the scope renders in a CJK
// font. Restore the caller's previous font/size at destruction so the
// buddy ASCII art and other ASCII-only surfaces keep the default 6x8.
struct UiCjkFont {
  LovyanGFX* _g;
  const lgfx::IFont* _savedFont;
  float _savedSx;
  float _savedSy;
  UiCjkFont(LovyanGFX* g);
  ~UiCjkFont();
};
