#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

// PSRAM-first allocators. Every large buffer / JSON document goes through
// here so we can graceful-fallback when PSRAM is absent (dev boards without
// -DBOARD_HAS_PSRAM, or a bad PSRAM chip) — callers never branch.

inline bool psHasPsram() {
  return ESP.getPsramSize() > 0;
}

// Prefer SPIRAM; on failure or absence, fall back to the internal heap so
// the caller doesn't have to handle nullptr for normal-sized requests.
inline void* psAlloc(size_t n) {
  if (n == 0) return nullptr;
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!p) p = malloc(n);
  return p;
}

inline void* psCalloc(size_t count, size_t size) {
  size_t n = count * size;
  void* p = psAlloc(n);
  if (p) memset(p, 0, n);
  return p;
}

inline void* psRealloc(void* ptr, size_t newSize) {
  void* p = heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM);
  if (!p) p = realloc(ptr, newSize);
  return p;
}

inline void psFree(void* ptr) {
  if (ptr) heap_caps_free(ptr);
}

// ArduinoJson 7 allocator that routes the DOM onto PSRAM. Used as:
//   JsonDocument doc(&psramJsonAllocator());
// ONLY safe to use before startBt() — the PSRAM heap's free-list is
// corrupted by BT controller init, so any post-BLE alloc/free crashes.
struct PsramJsonAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override      { return psAlloc(size); }
  void  deallocate(void* ptr) override      { psFree(ptr); }
  void* reallocate(void* ptr, size_t n) override { return psRealloc(ptr, n); }
};

inline PsramJsonAllocator& psramJsonAllocator() {
  static PsramJsonAllocator a;
  return a;
}

// ─── PSRAM arena ──────────────────────────────────────────────────────
// Bump allocator sitting inside a single large pre-BLE PSRAM allocation.
// After BLE starts the PSRAM heap metadata goes bad, but the contents of
// the pre-allocated arena block are still readable/writable normally.
// Use for runtime JSON parsing via PsramArenaAllocator + psArenaReset()
// between parses.
//
// Each allocation is prefixed with an 8-byte size header so reallocate()
// can copy the correct old payload into a fresh block when the caller's
// pointer isn't at the top of the arena. ArduinoJson 7 allocates both a
// VariantPool and a StringPool and grows them independently — neither is
// guaranteed to be the most-recent allocation when reallocate() fires.
extern uint8_t* g_psArenaBase;
extern size_t   g_psArenaCap;
extern size_t   g_psArenaUsed;
extern uint8_t* g_psArenaLastPtr;   // fast-path for top-of-arena realloc

inline void psArenaInit(size_t capacity) {
  if (g_psArenaBase) return;
  g_psArenaBase = (uint8_t*)psAlloc(capacity);
  g_psArenaCap  = g_psArenaBase ? capacity : 0;
  g_psArenaUsed = 0;
  g_psArenaLastPtr = nullptr;
}

inline void psArenaReset() {
  g_psArenaUsed = 0;
  g_psArenaLastPtr = nullptr;
}
inline size_t psArenaUsed() { return g_psArenaUsed; }
inline size_t psArenaCapacity() { return g_psArenaCap; }

// Allocate n bytes; returned pointer is preceded by an 8-byte size field
// (the payload size, not counting the header). 8-byte aligned to match
// ArduinoJson's slot expectations.
inline void* psArenaAlloc(size_t n) {
  n = (n + 7) & ~(size_t)7;
  size_t total = n + 8;
  if (g_psArenaUsed + total > g_psArenaCap) return nullptr;
  uint8_t* hdr = g_psArenaBase + g_psArenaUsed;
  *(size_t*)hdr = n;
  uint8_t* payload = hdr + 8;
  g_psArenaUsed += total;
  g_psArenaLastPtr = payload;
  return payload;
}

inline size_t _psArenaSizeOf(void* ptr) {
  return ((const size_t*)ptr)[-1];
}

// Arena-backed allocator for ArduinoJson. Safe post-BLE because the
// arena is fully inside one pre-allocated PSRAM region — no heap walk
// happens. Single JsonDocument lifetime: alloc only; deallocate is a
// no-op. Caller resets the arena between uses via psArenaReset().
struct PsramArenaAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override { return psArenaAlloc(size); }
  void  deallocate(void*) override {}
  // Reallocate: read the old size from the header, then either grow in
  // place (ptr is the top allocation) or allocate fresh and copy.
  void* reallocate(void* ptr, size_t newSize) override {
    if (!ptr) return psArenaAlloc(newSize);
    newSize = (newSize + 7) & ~(size_t)7;
    size_t oldSize = _psArenaSizeOf(ptr);
    if (ptr == g_psArenaLastPtr) {
      // Top allocation: adjust used pointer in place.
      size_t off = (uint8_t*)ptr - g_psArenaBase;
      if (off + newSize > g_psArenaCap) return nullptr;
      g_psArenaUsed = off + newSize;
      ((size_t*)ptr)[-1] = newSize;
      return ptr;
    }
    // Not the top — allocate a fresh block above and copy the
    // overlap. Old block becomes abandoned (reclaimed on reset()).
    void* p = psArenaAlloc(newSize);
    if (!p) return nullptr;
    size_t copy = oldSize < newSize ? oldSize : newSize;
    memcpy(p, ptr, copy);
    return p;
  }
};

inline PsramArenaAllocator& psArenaJsonAllocator() {
  static PsramArenaAllocator a;
  return a;
}
