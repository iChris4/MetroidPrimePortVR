// The ANUV flattener and evaluator, checked on the arrow-cannon model's real ANUV
// bytes (embedded, so no game files are needed) and on hand-built transforms.

#include "port_remastered_anuv.h"
#include "port_remastered_anuv_gun.h"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

bool Near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) < eps; }

void TestGun() {
  Anuv anuv;
  std::string error;
  const bool parsed = ParseAnuv(kAnuvGun, sizeof(kAnuvGun), anuv, error);
  CHECK_MSG(parsed, ("the gun ANUV parses: " + error).c_str());
  if (anuv.entries.empty()) {
    CHECK_MSG(false, "the gun ANUV has an entry");
    return;
  }
  const AnuvEntry& e = anuv.entries[0];
  CHECK_MSG(e.skip == AnuvSkip::None, "the gun entry flattens");
  CHECK_MSG(e.xf[0].Identity() && e.xf[2].Identity(), "transforms 0 and 2 are the identity");
  CHECK_MSG(!e.xf[1].Identity(), "transform 1 moves");
  // Root-time semantics, as anuv.py evaluates it: the U translation pulses along
  // the ICAN ramp.
  const double want[13] = {0, .25, .5, .2733, .0056, .2005, .4936, .3192, .0215, .1528, .4757, .3629, .0461};
  for (int i = 0; i < 13; ++i) {
    float m[8];
    EvalAnuvTransform(e.xf[1], i * 0.25, m);
    char what[64];
    std::snprintf(what, sizeof what, "gun curve at t=%.2f (got %.4f)", i * 0.25, m[3]);
    CHECK_MSG(Near(m[3], want[i], 1e-3), what);
  }
}

void TestEval() {
  {  // The identity.
    AnuvTransform xf;
    float m[8];
    EvalAnuvTransform(xf, 12.3, m);
    CHECK_MSG(xf.Identity(), "a default transform is the identity");
    CHECK_MSG(Near(m[0], 1) && Near(m[1], 0) && Near(m[2], 0) && Near(m[3], 0) && Near(m[4], 0) && Near(m[5], 1) &&
                  Near(m[6], 0) && Near(m[7], 0),
              "the identity matrix");
  }
  {  // A linear curve interpolates between samples and loops at its period.
    AnuvTransform xf;
    xf.arg[0].kind = AnuvArg::Kind::Linear;
    xf.arg[0].samplesPerSecond = 1.f;
    xf.arg[0].period = 2.f;
    xf.arg[0].samples = {0.f, 1.f, 0.f};
    float m[8];
    EvalAnuvTransform(xf, 0.5, m);
    const float mid = m[3];
    EvalAnuvTransform(xf, 2.5, m);
    CHECK_MSG(Near(mid, 0.5) && Near(m[3], mid), "a linear curve interpolates and loops");
  }
  {  // A nearest curve holds a sample.
    AnuvTransform xf;
    xf.arg[0].kind = AnuvArg::Kind::Nearest;
    xf.arg[0].samplesPerSecond = 1.f;
    xf.arg[0].period = 2.f;
    xf.arg[0].samples = {0.f, 1.f, 0.f};
    float a[8], b[8];
    EvalAnuvTransform(xf, 0.2, a);
    EvalAnuvTransform(xf, 0.4, b);
    CHECK_MSG(Near(a[3], b[3]), "a nearest curve holds its sample between steps");
  }
  {  // A constant rotation of a quarter turn turns the U axis onto V.
    AnuvTransform xf;
    xf.arg[4].value = 1.5707963f;
    float m[8];
    EvalAnuvTransform(xf, 0, m);
    CHECK_MSG(Near(std::fabs(m[0]), 0, 1e-4) && Near(std::fabs(m[1]), 1, 1e-4), "a quarter-turn rotation");
  }
}

// Curves the converter never writes but a damaged import could: they read a sample
// and never past the ends.
void TestEdges() {
  float m[8];
  AnuvTransform xf;
  xf.arg[0].kind = AnuvArg::Kind::Linear;
  xf.arg[0].samplesPerSecond = 1.f;
  xf.arg[0].samples = {0.f, 1.f, 0.f};
  xf.arg[0].period = 0.f;
  EvalAnuvTransform(xf, 5.0, m);
  CHECK_MSG(Near(m[3], 0.0), "a zero period reads the first sample");
  xf.arg[0].period = std::nanf("");
  EvalAnuvTransform(xf, 5.0, m);
  CHECK_MSG(Near(m[3], 0.0), "a NaN period reads the first sample");
  xf.arg[0].period = 2.f;
  xf.arg[0].samplesPerSecond = -1.f;
  EvalAnuvTransform(xf, 0.5, m);  // x = -0.5, wrapped to 1.5
  CHECK_MSG(Near(m[3], 0.5), "a negative rate wraps into the period");
  xf.arg[0].samplesPerSecond = 1.f;
  xf.arg[0].samples = {0.25f};  // fewer samples than the period needs
  EvalAnuvTransform(xf, 1.5, m);
  CHECK_MSG(Near(m[3], 0.25), "a short curve clamps to its last sample");
  xf.arg[0].kind = AnuvArg::Kind::Nearest;
  EvalAnuvTransform(xf, 1.9, m);
  CHECK_MSG(Near(m[3], 0.25), "a short nearest curve clamps too");
  xf.arg[0].samples.clear();
  EvalAnuvTransform(xf, 1.0, m);
  CHECK_MSG(Near(m[3], 0.0), "a curve with no samples reads zero");
}

void TestWords() {
  AnuvTransform xf;
  xf.arg[0].value = 0.25f;
  xf.arg[1].kind = AnuvArg::Kind::Linear;
  xf.arg[1].samplesPerSecond = 2.f;
  xf.arg[1].period = 1.f;
  xf.arg[1].samples = {0.f, 0.5f, 1.f};
  const std::vector<uint32_t> w = AnuvAnimWords(xf);
  CHECK_MSG(!w.empty() && w[0] == kAnuvAnimType, "the words open with the port-only type");
  // type, arg0 (kind, value), arg1 (kind, sps, period, n, 3 samples), 3 more consts.
  CHECK_MSG(w.size() == 1 + 2 + 7 + 2 + 2 + 2, "the words' length");
}

} // namespace

int main() {
  TestGun();
  TestEval();
  TestEdges();
  TestWords();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_anuv_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
