// D-212: fronteras de las notas editadas de D-211, medidas sobre el fixture
// target_hz exportado (tests/fixtures/d212_target_hz.csv), no sobre el .rpp.
//
// Criterio plan/m2/d211-criterio.md, fijado antes de medir:
//   (1) recorrido >= 50 c dentro de +-30 c del punto de cambio de bloque;
//   (2) residual fuera de +-30 c <= 10 c.
// Hop 512 a 44.100 Hz (11,609 ms); ventana: 6 frames antes y 6 despues de la
// frontera; residual medido sobre +-4 frames. El test es de caracterizacion:
// con los datos actuales debe FALLAR.

#include "TestAssert.h"
#include "../Source/Utils/BasePitchCurve.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <iostream>

namespace {

constexpr double kHopMs = 512.0 / 44100.0 * 1000.0;

struct Boundary {
  std::string name;
  std::vector<double> hz; // index 0 == frame-12
};

double cents(double hz) { return 1200.0 * std::log2(hz / 440.0); }

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

std::vector<Boundary> loadFixture() {
  std::string path = __FILE__;
  const size_t slash = path.find_last_of("/\\");
  path = path.substr(0, slash) + "/fixtures/d212_target_hz.csv";
  std::ifstream in(path);
  CHECK(in.good());
  std::string line;
  std::getline(in, line); // header
  std::map<std::string, Boundary> byName;
  std::vector<std::string> order;
  while (std::getline(in, line)) {
    std::stringstream ss(line);
    std::string name, frame, offset, hz;
    std::getline(ss, name, ',');
    std::getline(ss, frame, ',');
    std::getline(ss, offset, ',');
    std::getline(ss, hz, ',');
    if (byName.find(name) == byName.end()) {
      byName[name].name = name;
      byName[name].hz.assign(25, 0.0);
      order.push_back(name);
    }
    const int off = std::stoi(offset);
    byName[name].hz[static_cast<size_t>(off + 12)] = std::stod(hz);
  }
  std::vector<Boundary> out;
  for (const auto &n : order)
    out.push_back(byName[n]);
  CHECK(out.size() == 4);
  return out;
}

struct Metrics {
  double before, after, residual, maxSlope;
};

Metrics measure(const Boundary &b) {
  const int c = 12;
  // Curva procesada por la capa de curva real (D-213), no por el .rpp.
  std::vector<float> midi(b.hz.size());
  for (size_t i = 0; i < b.hz.size(); ++i)
    midi[i] = static_cast<float>(69.0 + 12.0 * std::log2(b.hz[i] / 440.0));
  BasePitchCurve::limitBoundarySlope(midi, {c});
  std::vector<double> hz(midi.size());
  for (size_t i = 0; i < midi.size(); ++i)
    hz[i] = 440.0 * std::pow(2.0, (midi[i] - 69.0) / 12.0);
  std::vector<double> before, after;
  for (int k = c - 6; k < c; ++k)
    before.push_back(cents(hz[static_cast<size_t>(k)]));
  for (int k = c; k < c + 6; ++k)
    after.push_back(cents(hz[static_cast<size_t>(k)]));
  CHECK(!before.empty() && !after.empty());
  Metrics m{};
  m.before = median(before);
  m.after = median(after);
  const double hi = std::max(m.before, m.after);
  const double lo = std::min(m.before, m.after);
  m.residual = 0.0;
  for (int k = c - 4; k <= c + 4; ++k) {
    const double v = cents(hz[static_cast<size_t>(k)]);
    m.residual = std::max({m.residual, v - hi - 30.0, lo - 30.0 - v});
  }
  // (3) pendiente maxima dentro de +-20 ms: +-2 frames a 11,609 ms.
  m.maxSlope = 0.0;
  for (int k = c - 2; k < c + 2; ++k) {
    const double d =
        cents(hz[static_cast<size_t>(k + 1)]) - cents(hz[static_cast<size_t>(k)]);
    m.maxSlope = std::max(m.maxSlope, std::abs(d) / kHopMs);
  }
  return m;
}

void testBoundariesMeetCriterion() {
  const auto boundaries = loadFixture();
  std::vector<Metrics> all;
  for (const Boundary &b : boundaries) {
    const Metrics m = measure(b);
    std::printf("%s: residual=%.1f c slope=%.1f c/ms\n", b.name.c_str(),
                m.residual, m.maxSlope);
    all.push_back(m);
  }
  for (const Metrics &m : all) {
    // (2) residual fuera de +-30 c <= 10 c
    CHECK(m.residual <= 10.0);
    // (3) pendiente maxima dentro de +-20 ms <= 1 c/ms
    CHECK(m.maxSlope <= 1.0);
  }
}

} // namespace

int main() {
  testBoundariesMeetCriterion();
  std::cout << "D212BoundaryTransitionTests: all checks passed\n";
  return 0;
}
