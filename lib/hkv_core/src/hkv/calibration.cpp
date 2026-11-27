#include "hkv/calibration.h"

#include <math.h>
#include <string.h>

namespace hkv {

const float CouplingCalibrator::kTauGridMin[kTauCount] = {0, 2, 4, 6, 8, 10, 12, 15, 20, 25, 30};

namespace {
constexpr float kMaxCoupling = 0.8f;  // same limit as Settings validation

// Sum of squared residuals of y + offset - c * x (sensor space) from the running sums.
double sse(double n, double sx, double sy, double sxx, double sxy, double syy, double o, double c) {
  const double v = syy + n * o * o + c * c * sxx + 2.0 * o * sy - 2.0 * c * sxy - 2.0 * o * c * sx;
  return v > 0.0 ? v : 0.0;
}

void lagStep(float& lag, float value, float tauMin, float dtS) {
  if (tauMin <= 0.0f || dtS <= 0.0f) {
    if (tauMin <= 0.0f) lag = value;
    return;
  }
  lag += (value - lag) * (1.0f - expf(-dtS / (tauMin * 60.0f)));
}
}  // namespace

void CouplingCalibrator::start(float currentOffsetC, float currentCoupling, float currentTauMin, float warmupS) {
  warmupS_ = warmupS;
  memset(grid_, 0, sizeof(grid_));
  memset(&current_, 0, sizeof(current_));
  memset(&all_, 0, sizeof(all_));
  curOffset_ = currentOffsetC;
  curCoupling_ = currentCoupling;
  curTauMin_ = currentTauMin;
  elapsedS_ = 0.0f;
  xMin_ = xMax_ = 0.0f;
  first_ = true;
  used_ = 0;
}

void CouplingCalibrator::addTo(Sums& s, double x, double y) {
  s.n += 1.0;
  s.sx += x;
  s.sy += y;
  s.sxx += x * x;
  s.sxy += x * y;
  s.syy += y * y;
}

void CouplingCalibrator::add(float radiatorC, float roomRawC, float referenceC, float dtS, bool useForFit) {
  if (first_) {
    // Filters start settled at the first value: start the run with a radiator that has been
    // cold (or constant) for a while, then the start state is right.
    for (Sums& s : grid_) s.lagC = radiatorC;
    current_.lagC = radiatorC;
    all_.lagC = radiatorC;
    first_ = false;
    dtS = 0.0f;
  }
  for (int i = 0; i < kTauCount; ++i) lagStep(grid_[i].lagC, radiatorC, kTauGridMin[i], dtS);
  lagStep(current_.lagC, radiatorC, curTauMin_, dtS);
  all_.lagC = current_.lagC;
  elapsedS_ += dtS;
  if (!useForFit) return;
  const double y = static_cast<double>(roomRawC) - referenceC;
  addTo(all_, static_cast<double>(all_.lagC) - referenceC, y);
  if (elapsedS_ < warmupS_) return;

  for (Sums& s : grid_) addTo(s, static_cast<double>(s.lagC) - referenceC, y);
  addTo(current_, static_cast<double>(current_.lagC) - referenceC, y);

  const float span = radiatorC - referenceC;
  if (used_ == 0 || span < xMin_) xMin_ = span;
  if (used_ == 0 || span > xMax_) xMax_ = span;
  ++used_;
}

CalibResult CouplingCalibrator::result() const {
  CalibResult r;
  r.samples = used_;
  r.spanK = xMax_ - xMin_;
  const bool fit = used_ >= kMinSamples;
  const Sums& cu = fit ? current_ : all_;
  if (cu.n < 1.0) return r;
  r.valid = true;
  if (!fit) {
    r.samples = static_cast<uint32_t>(cu.n);
    r.includesWarmup = true;
  }

  const double n = cu.n;
  const double cc = curCoupling_;
  const double before = sse(n, cu.sx, cu.sy, cu.sxx, cu.sxy, cu.syy, curOffset_, cc);
  r.rmsBeforeK = static_cast<float>(sqrt(before / n) / (1.0 - cc));

  // Fallback: offset only, with the current coupling and time constant.
  const double oOnly = (cc * cu.sx - cu.sy) / n;
  r.offsetC = static_cast<float>(oOnly);
  r.coupling = curCoupling_;
  r.tauMin = curTauMin_;
  r.rmsAfterK = static_cast<float>(sqrt(sse(n, cu.sx, cu.sy, cu.sxx, cu.sxy, cu.syy, oOnly, cc) / n) / (1.0 - cc));
  if (!fit || r.spanK < kMinSpanK) return r;

  int best = -1;
  double bestSse = 0.0, bestO = 0.0, bestC = 0.0;
  for (int i = 0; i < kTauCount; ++i) {
    const Sums& s = grid_[i];
    const double den = s.n * s.sxx - s.sx * s.sx;
    if (den <= 1e-9) continue;
    const double c = (s.n * s.sxy - s.sx * s.sy) / den;
    if (c < 0.0 || c > kMaxCoupling) continue;  // physically implausible
    const double o = (c * s.sx - s.sy) / s.n;   // offset = -intercept
    const double e = sse(s.n, s.sx, s.sy, s.sxx, s.sxy, s.syy, o, c);
    if (best < 0 || e < bestSse) {
      best = i;
      bestSse = e;
      bestO = o;
      bestC = c;
    }
  }
  if (best < 0) return r;
  r.couplingSolved = true;
  r.offsetC = static_cast<float>(bestO);
  r.coupling = static_cast<float>(bestC);
  r.tauMin = kTauGridMin[best];
  r.rmsAfterK = static_cast<float>(sqrt(bestSse / n) / (1.0 - bestC));
  return r;
}

namespace {
int16_t centi(float c) {
  const float v = roundf(c * 100.0f);
  return static_cast<int16_t>(v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v);
}
float fromCenti(int16_t v) { return static_cast<float>(v) / 100.0f; }

CalibResult replay(const CalibSample* s, size_t n, float o, float c, float tau, float warmupS) {
  CouplingCalibrator cal;
  cal.start(o, c, tau, warmupS);
  for (size_t i = 0; i < n; ++i) {
    const float dt = i ? static_cast<float>(s[i].tS - s[i - 1].tS) : 0.0f;
    cal.add(fromCenti(s[i].radiatorCenti), fromCenti(s[i].roomRawCenti), fromCenti(s[i].referenceCenti), dt,
            s[i].rejected == 0);
  }
  return cal.result();
}
}  // namespace

CalibSample makeCalibSample(float radiatorC, float roomRawC, float referenceC, uint32_t tS) {
  CalibSample s;
  s.radiatorCenti = centi(radiatorC);
  s.roomRawCenti = centi(roomRawC);
  s.referenceCenti = centi(referenceC);
  s.tS = static_cast<uint16_t>(tS > 65535u ? 65535u : tS);
  s.rejected = 0;
  s.reserved = 0;
  return s;
}

CalibResult fitWithOutlierRejection(CalibSample* s, size_t n, float currentOffsetC, float currentCoupling,
                                    float currentTauMin, float warmupS) {
  for (size_t i = 0; i < n; ++i) s[i].rejected = 0;
  const CalibResult r1 = replay(s, n, currentOffsetC, currentCoupling, currentTauMin, warmupS);
  if (!r1.valid || n < 10) return r1;

  // Residuals of the model found in pass 1 (sensor space): e = raw + o - c*R_lag - (1-c)*ref.
  // Only samples that took part in the fit are candidates.
  const auto forEachResidual = [&](auto&& fn) {
    float lag = fromCenti(s[0].radiatorCenti);
    float elapsed = 0.0f;
    for (size_t i = 0; i < n; ++i) {
      const float rad = fromCenti(s[i].radiatorCenti);
      const float dt = i ? static_cast<float>(s[i].tS - s[i - 1].tS) : 0.0f;
      lagStep(lag, rad, r1.tauMin, dt);
      elapsed += dt;
      if (!r1.includesWarmup && elapsed < warmupS) continue;
      const float ref = fromCenti(s[i].referenceCenti);
      const float e = fromCenti(s[i].roomRawCenti) + r1.offsetC - r1.coupling * lag - (1.0f - r1.coupling) * ref;
      fn(i, fabsf(e));
    }
  };
  double sum2 = 0.0;
  uint32_t count = 0;
  forEachResidual([&](size_t, float e) {
    sum2 += static_cast<double>(e) * e;
    ++count;
  });
  if (count < 10) return r1;
  float thr = 3.0f * static_cast<float>(sqrt(sum2 / count));
  if (thr < 0.2f) thr = 0.2f;
  const uint32_t maxReject = count / 10u;
  uint32_t rejected = 0;
  for (int iter = 0; iter < 30; ++iter) {
    rejected = 0;
    forEachResidual([&](size_t, float e) {
      if (e > thr) ++rejected;
    });
    if (rejected <= maxReject) break;
    thr *= 1.25f;
  }
  if (rejected == 0 || rejected > maxReject) return r1;
  forEachResidual([&](size_t i, float e) {
    if (e > thr) s[i].rejected = 1;
  });
  CalibResult r2 = replay(s, n, currentOffsetC, currentCoupling, currentTauMin, warmupS);
  if (!r2.valid) return r1;
  r2.outliers = rejected;
  return r2;
}

}  // namespace hkv
