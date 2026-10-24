// A normal goes through the orient's inverse transpose, so it stays perpendicular to a surface
// under a non-uniform scale (the retail-id models stretched 1.431 in height) and under a mirror.

#include "port_remastered_convert.h"
#include "port_test.h"

#include <cmath>

using namespace PortRemastered;

namespace {

void Apply(const double (*A)[3], const double* v, double* out) {
  for (int r = 0; r < 3; ++r) {
    out[r] = A[r][0] * v[0] + A[r][1] * v[1] + A[r][2] * v[2];
  }
}

double Dot(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Check(const double (*M)[3], const char* what) {
  double NM[3][3];
  NormalMatrix(M, NM);
  // A plane with two edges e1, e2 and normal n = e1 x e2 (not axis aligned).
  const double e1[3] = {1.0, 0.0, 1.0}, e2[3] = {0.0, 1.0, 1.0};
  const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0]};
  double m1[3], m2[3], mn[3], naive[3];
  Apply(M, e1, m1);
  Apply(M, e2, m2);
  Apply(NM, n, mn);
  Apply(M, n, naive);
  CHECK_MSG(std::fabs(Dot(mn, m1)) < 1e-9 && std::fabs(Dot(mn, m2)) < 1e-9, what);
  // The same surface, normal pointing the same side: the transformed one is not flipped by a
  // mirror (n . mn has the sign of det(M)^0 -> the normal matrix keeps the side).
  const double side = Dot(mn, naive);
  CHECK_MSG(side > 0.0, what);
}

}  // namespace

int main() {
  // Stretched 1.431 in height (gc y is the model's depth axis after the default swap, so z here).
  const double stretch[3][3] = {{-1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 1.431, 0.0}};
  Check(stretch, "a non-uniform orient keeps normals perpendicular");
  // A mirror times a non-uniform scale (negative determinant).
  const double mirror[3][3] = {{-2.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 0.5}};
  Check(mirror, "a mirroring non-uniform orient keeps normals perpendicular");
  // A rotation/mirror is its own inverse transpose.
  const double rot[3][3] = {{-1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0}};
  double NM[3][3];
  NormalMatrix(rot, NM);
  bool same = true;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      same = same && std::fabs(NM[r][c] - rot[r][c]) < 1e-12;
    }
  }
  CHECK_MSG(same, "an orthogonal orient carries normals as itself");
  const double flat[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 0}};
  NormalMatrix(flat, NM);
  CHECK_MSG(NM[0][0] == 0.0 && NM[2][2] == 0.0, "a singular orient gives the zero matrix");
  return PORT_TEST_RESULT();
}
