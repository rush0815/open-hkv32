#pragma once
// Calibration run of the room sensor against a temporary reference sensor.
//
// Model of the room sensor in a device mounted on the radiator (see allocator.h):
//   raw + offset = c * R_lag + (1 - c) * T_ref
//   R_lag = radiator temperature low-pass filtered with time constant tau
// With y = raw - T_ref and x = R_lag - T_ref this is the straight line
//   y = -offset + c * x
// so offset and c follow from a linear least-squares fit. tau is found by running the
// fit for a grid of time constants in parallel and picking the one with the smallest
// residual. Only running sums are kept (a few hundred bytes, no sample buffer), so it
// also fits on small MCUs.

#include <stddef.h>
#include <stdint.h>

namespace hkv {

struct CalibResult {
  bool valid = false;           // enough samples
  bool couplingSolved = false;  // radiator varied enough to separate offset and c
  float offsetC = 0.0f;         // -> Settings::roomOffsetC
  float coupling = 0.0f;        // -> Settings::roomCoupling
  float tauMin = 0.0f;          // -> Settings::roomCouplingTauMin
  float rmsBeforeK = 0.0f;      // room temperature error with the settings at start (RMS)
  float rmsAfterK = 0.0f;       // ... with the result
  float spanK = 0.0f;           // range of (radiator - reference) seen during the run
  uint32_t samples = 0;         // samples used for the fit (after warm-up)
  uint32_t outliers = 0;        // samples rejected as outliers (see fitWithOutlierRejection)
  bool includesWarmup = false;  // offset-only result from all samples incl. the warm-up
};

// One stored sample of a calibration run (10 bytes; temperatures in 1/100 C).
struct CalibSample {
  int16_t radiatorCenti;   // incl. radiator offset
  int16_t roomRawCenti;    // built-in room sensor without offset
  int16_t referenceCenti;
  uint16_t tS;             // seconds since the start of the run (max. 18 h)
  uint8_t rejected;        // 1 = outlier, not used for the fit
  uint8_t reserved;
};
CalibSample makeCalibSample(float radiatorC, float roomRawC, float referenceC, uint32_t tS);

class CouplingCalibrator {
 public:
  static constexpr int kTauCount = 11;
  static const float kTauGridMin[kTauCount];
  // Filters run during warm-up, but samples are not used for the fit yet.
  // Default 10 min; short runs use at most a third of the run time (see warmupFor()).
  static constexpr float kWarmupS = 600.0f;
  static uint32_t warmupFor(uint32_t runS) { return runS / 3u < 600u ? runS / 3u : 600u; }
  // Below this radiator variation (max - min of radiator - reference) offset and c can't
  // be told apart; only the offset is computed then (with the current c and tau).
  static constexpr float kMinSpanK = 8.0f;
  static constexpr uint32_t kMinSamples = 10;  // for the fit (after warm-up)

  // A result is available from the first sample on: until the fit has kMinSamples after the
  // warm-up, only the offset is computed (from all samples, with the current c and tau).
  // Current settings: used for the "before" error and the offset-only fallback.
  void start(float currentOffsetC, float currentCoupling, float currentTauMin, float warmupS = kWarmupS);
  // radiatorC including the radiator offset; roomRawC WITHOUT room offset.
  // dtS = time since the previous sample (0 for the first one).
  // useForFit = false: the filters follow the sample, but it is not part of the fit (outlier).
  void add(float radiatorC, float roomRawC, float referenceC, float dtS, bool useForFit = true);
  CalibResult result() const;
  uint32_t samples() const { return used_; }

 private:
  struct Sums {
    float lagC;
    double n, sx, sy, sxx, sxy, syy;
  };
  static void addTo(Sums& s, double x, double y);

  Sums grid_[kTauCount];
  Sums current_;  // with the current tau (for "before" and the offset-only fallback)
  Sums all_;      // same, but including the warm-up (run ended early: offset only)
  float curOffset_ = 0.0f, curCoupling_ = 0.0f, curTauMin_ = 0.0f;
  float elapsedS_ = 0.0f;
  float warmupS_ = kWarmupS;
  float xMin_ = 0.0f, xMax_ = 0.0f;
  bool first_ = true;
  uint32_t used_ = 0;
};

// Second pass over the stored samples of a run (the online sums cannot do this):
//  1. fit as usual,
//  2. reject samples whose residual is above 3 sigma (and above 0.2 K), at most 10 %,
//     e.g. a draught or sun on the reference sensor,
//  3. fit again without them.
// Marks the rejected samples in s[]. Returns the result of step 1 if nothing was rejected.
CalibResult fitWithOutlierRejection(CalibSample* s, size_t n, float currentOffsetC, float currentCoupling,
                                    float currentTauMin, float warmupS);

}  // namespace hkv
