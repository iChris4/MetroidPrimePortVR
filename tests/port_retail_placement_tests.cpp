#include "port_retail_placement.h"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using M3 = double[3][3];

void Mul(const M3 a, const M3 b, M3 out) {
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    }
  }
}

double Det(const M3 m) {
  return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
         m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

void Invert(const M3 m, M3 out) {
  const double d = Det(m);
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
      out[r][c] = (m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1]) / d;
    }
  }
}

// Z * Y * X, as the editor composes a rotation (degrees).
void Rotation(double x, double y, double z, M3 out) {
  const double k = 3.14159265358979323846 / 180.0;
  const double cx = std::cos(x * k), sx = std::sin(x * k), cy = std::cos(y * k), sy = std::sin(y * k),
               cz = std::cos(z * k), sz = std::sin(z * k);
  const M3 rx = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
  const M3 ry = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
  const M3 rz = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
  M3 zy;
  Mul(rz, ry, zy);
  Mul(zy, rx, out);
}

struct Case {
  double rot[3], scale[3], pos[3];        // the retail object
  double eRot[3], eScale[3], ePos[3];     // Remastered's entity
};

// The object's model matrix with the placement composed reproduces the entity's transform, and
// the normal matrix (its inverse transpose) keeps normals perpendicular to the tangents.
void TestCompose(const Case& t) {
  M3 r, s = {{t.scale[0], 0, 0}, {0, t.scale[1], 0}, {0, 0, t.scale[2]}}, rs, inv;
  Rotation(t.rot[0], t.rot[1], t.rot[2], r);
  Mul(r, s, rs);
  Invert(rs, inv);
  M3 er, es = {{t.eScale[0], 0, 0}, {0, t.eScale[1], 0}, {0, 0, t.eScale[2]}}, e;
  Rotation(t.eRot[0], t.eRot[1], t.eRot[2], er);
  Mul(er, es, e);
  // The importer's placement (port_remastered_room.cpp, PairRetailModels).
  M3 orient;
  Mul(inv, e, orient);
  float correction[12];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      correction[i * 4 + j] = float(orient[i][j]);
    }
    correction[i * 4 + 3] = float(inv[i][0] * (t.ePos[0] - t.pos[0]) + inv[i][1] * (t.ePos[1] - t.pos[1]) +
                                  inv[i][2] * (t.ePos[2] - t.pos[2]));
  }
  float xf[12], scale[3], out[12];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      xf[i * 4 + j] = float(r[i][j]);
    }
    xf[i * 4 + 3] = float(t.pos[i]);
    scale[i] = float(t.scale[i]);
  }
  PortRetailPlacement::Compose(xf, scale, correction, out);
  double worst = 0;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      worst = std::max(worst, std::fabs(out[i * 4 + j] - e[i][j]));
    }
    worst = std::max(worst, std::fabs(out[i * 4 + 3] - t.ePos[i]) / std::max(1.0, std::fabs(t.ePos[i])));
  }
  CHECK_MSG(worst < 1e-4, "composite = entity transform, worst " + std::to_string(worst));

  // A model-space vertex lands where the entity puts it.
  const double v[3] = {1.5, -2.25, 0.75};
  for (int i = 0; i < 3; ++i) {
    const double want = e[i][0] * v[0] + e[i][1] * v[1] + e[i][2] * v[2] + t.ePos[i];
    const double got = out[i * 4] * v[0] + out[i * 4 + 1] * v[1] + out[i * 4 + 2] * v[2] + out[i * 4 + 3];
    CHECK_MSG(std::fabs(got - want) < 1e-3 * std::max(1.0, std::fabs(want)), "vertex row " + std::to_string(i));
  }

  M3 m, mi, nrm;
  for (int i = 0; i < 9; ++i) {
    m[i / 3][i % 3] = out[(i / 3) * 4 + i % 3];
  }
  Invert(m, mi);
  for (int i = 0; i < 9; ++i) {
    nrm[i / 3][i % 3] = mi[i % 3][i / 3];
  }
  const double n0[3] = {0, 0, 1}, t0[3] = {0.6, 0.8, 0};
  double n[3], tg[3];
  for (int i = 0; i < 3; ++i) {
    n[i] = nrm[i][0] * n0[0] + nrm[i][1] * n0[1] + nrm[i][2] * n0[2];
    tg[i] = m[i][0] * t0[0] + m[i][1] * t0[1] + m[i][2] * t0[2];
  }
  const double dot = (n[0] * tg[0] + n[1] * tg[1] + n[2] * tg[2]) /
                     (std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) *
                      std::sqrt(tg[0] * tg[0] + tg[1] * tg[1] + tg[2] * tg[2]));
  CHECK_MSG(std::fabs(dot) < 1e-5, "normal stays perpendicular, dot " + std::to_string(dot));
}

void TestFile() {
  std::vector<PortRetailPlacement::Record> records(2);
  records[0].mrea = 0x1A2B3C4D;
  records[0].editorId = 0x0412002A;
  records[0].model = 0x004A5B57;
  for (int i = 0; i < 12; ++i) {
    records[0].correction[size_t(i)] = float(i) * 0.5f - 1.f;
  }
  records[1].mrea = 0xFFFFFFFF;
  records[1].editorId = 0x08000001;
  records[1].model = 0x12345678;
  records[1].correction[0] = -0.f;
  records[1].correction[11] = 1e-30f;
  const std::vector<uint8_t> bytes = PortRetailPlacement::Encode(records);
  CHECK(bytes.size() == 12 + 2 * 60);
  CHECK(bytes[0] == 'M' && bytes[1] == 'P' && bytes[2] == 'R' && bytes[3] == 'P');
  std::vector<PortRetailPlacement::Record> back;
  CHECK(PortRetailPlacement::Decode(bytes.data(), bytes.size(), back));
  CHECK(back.size() == 2);
  for (size_t r = 0; r < back.size() && r < 2; ++r) {
    CHECK(back[r].mrea == records[r].mrea && back[r].editorId == records[r].editorId &&
          back[r].model == records[r].model);
    for (size_t i = 0; i < 12; ++i) {
      CHECK(std::memcmp(&back[r].correction[i], &records[r].correction[i], 4) == 0);
    }
  }
  CHECK_MSG(!PortRetailPlacement::Decode(bytes.data(), bytes.size() - 1, back) && back.empty(), "short file");
  std::vector<uint8_t> bad = bytes;
  bad[4] = 2;
  CHECK_MSG(!PortRetailPlacement::Decode(bad.data(), bad.size(), back), "other version");
  bad = bytes;
  bad[0] = 'X';
  CHECK_MSG(!PortRetailPlacement::Decode(bad.data(), bad.size(), back), "bad magic");
  const std::vector<uint8_t> empty = PortRetailPlacement::Encode({});
  CHECK(PortRetailPlacement::Decode(empty.data(), empty.size(), back) && back.empty());
}

}  // namespace

int main() {
  // A small move of the entity's own (the per-instance case), a scaled object, a non-uniform
  // scale under a rotation, and a mirrored entity.
  TestCompose({{0, 0, 90}, {1, 1, 1}, {100, -50, 20}, {0, 0, 90.23}, {1, 1, 1}, {100.01, -50, 20.5}});
  TestCompose({{10, 20, 30}, {2, 2, 2}, {-300, 412.5, -7}, {12, 18, 33}, {2.1, 1.9, 2}, {-299, 413, -6}});
  TestCompose({{45, 0, 135}, {1, 3, 0.5}, {5, 6, 7}, {40, 5, 130}, {1.2, 2.5, 0.75}, {5.5, 6, 6.5}});
  TestCompose({{0, 90, 0}, {1, 1, 1}, {0, 0, 0}, {0, 90, 0}, {-1, 1, 1}, {1, 2, 3}});
  TestFile();
  return PORT_TEST_RESULT();
}
