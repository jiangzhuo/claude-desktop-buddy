#pragma once

// LiPo discharge is non-linear; the old (mv-3200)/10 mapping under-reports
// above 4.0V and over-reports near empty. Piecewise linear between measured
// sample points gives a usable curve without a full fuel-gauge IC.
static inline int batteryPercent(int vBat_mV) {
  static const int16_t pts[][2] = {
    {3300,   0}, {3500,   3}, {3600,  10}, {3700,  25}, {3800,  45},
    {3900,  60}, {4000,  75}, {4100,  90}, {4200, 100},
  };
  const int N = sizeof(pts) / sizeof(pts[0]);
  if (vBat_mV <= pts[0][0])     return 0;
  if (vBat_mV >= pts[N-1][0])   return 100;
  for (int i = 1; i < N; i++) {
    if (vBat_mV <= pts[i][0]) {
      int dv = pts[i][0]   - pts[i-1][0];
      int dp = pts[i][1]   - pts[i-1][1];
      return pts[i-1][1] + (vBat_mV - pts[i-1][0]) * dp / dv;
    }
  }
  return 100;
}
