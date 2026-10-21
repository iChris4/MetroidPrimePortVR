#include "port_remastered_import.h"
#include "port_test.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace PortRemastered::ImportDetail;

namespace {

void PutFourCC(std::vector<uint8_t>& data, const char (&tag)[5]) { data.insert(data.end(), tag, tag + 4); }

void PutReversedFourCC(std::vector<uint8_t>& data, const char (&tag)[5]) {
  for (int i = 3; i >= 0; --i) {
    data.push_back(uint8_t(tag[i]));
  }
}

void PutBe32(std::vector<uint8_t>& data, uint32_t value) {
  data.push_back(uint8_t(value >> 24));
  data.push_back(uint8_t(value >> 16));
  data.push_back(uint8_t(value >> 8));
  data.push_back(uint8_t(value));
}

void PutLe32(std::vector<uint8_t>& data, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    data.push_back(uint8_t(value >> (8 * i)));
  }
}

void PutLe64(std::vector<uint8_t>& data, uint64_t value) {
  PutLe32(data, uint32_t(value));
  PutLe32(data, uint32_t(value >> 32));
}

void SetLe64(std::vector<uint8_t>& data, size_t at, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    data[at + size_t(i)] = uint8_t(value >> (8 * i));
  }
}

void PutRetailLink(std::vector<uint8_t>& data, const char (&field)[5], uint32_t id) {
  PutFourCC(data, field);
  PutFourCC(data, "CNST");
  PutBe32(data, id);
}

// A valid IVEC CNST of three real CNST values. Its first two words deliberately
// resemble "COLR CNST <id>" to exercise the non-role-payload decoy case.
void PutRetailVectorDecoy(std::vector<uint8_t>& data, uint32_t id) {
  PutFourCC(data, "IVEC");
  PutFourCC(data, "CNST");
  PutFourCC(data, "CNST");
  PutBe32(data, PortRemastered::EffectFourCC("COLR"));
  PutFourCC(data, "CNST");
  PutBe32(data, id);
  PutFourCC(data, "CNST");
  PutBe32(data, 0);
}

void PutReversedGuidLink(std::vector<uint8_t>& data, const char (&field)[5], uint8_t kind,
                         const std::array<uint8_t, 16>& guid) {
  PutReversedFourCC(data, field);
  data.push_back(kind);
  data.insert(data.end(), guid.begin(), guid.end());
}

void PutReversedBoolProperty(std::vector<uint8_t>& data, const char (&field)[5], uint8_t value) {
  PutReversedFourCC(data, field);
  data.push_back(0);
  PutReversedFourCC(data, "CNST");
  data.push_back(value);
}

void PutReversedRealConstant(std::vector<uint8_t>& data, uint32_t bits = 0) {
  PutReversedFourCC(data, "CNST");
  PutLe32(data, bits);
}

void PutReversedEulerRotation(std::vector<uint8_t>& data, uint8_t order = 0, size_t realCount = 3,
                              bool includeOrder = true) {
  PutReversedFourCC(data, "REUL");
  for (size_t i = 0; i < realCount; ++i) {
    PutReversedRealConstant(data);
  }
  if (includeOrder) {
    data.push_back(order);
  }
}

void PutRoleKeyframeProperty(std::vector<uint8_t>& data, const char (&field)[5],
                             const char (&element)[5], uint32_t count, size_t keySize,
                             uint8_t keyFill = 0) {
  PutReversedFourCC(data, field);
  data.push_back(0);
  PutReversedFourCC(data, element);
  data.insert(data.end(), 18, 0); // source key-emitter fields before its vector count
  PutLe32(data, count);
  data.insert(data.end(), size_t(count) * keySize, keyFill);
}

void PutPvarDecoy(std::vector<uint8_t>& data, const std::array<uint8_t, 16>& guid) {
  PutReversedFourCC(data, "PVAR");
  data.push_back(0);
  std::vector<uint8_t> payload(14, 0);
  payload[12] = 21; // data byte count, little endian
  payload.insert(payload.end(), {'R', 'L', 'O', 'C', 0});
  payload.insert(payload.end(), guid.begin(), guid.end());
  data.insert(data.end(), payload.begin(), payload.end());
}

// A real RFRM resource: 32-byte form header, one DESC chunk, the four-byte
// DESC prefix, then a reversed-FourCC property stream ending in _END.
std::vector<uint8_t> RemasteredForm(const char (&outerForm)[5], const char (&rootForm)[5],
                                    std::vector<uint8_t> properties) {
  std::vector<uint8_t> data(32, 0);
  std::memcpy(data.data(), "RFRM", 4);
  data[20] = uint8_t(outerForm[0]);
  data[21] = uint8_t(outerForm[1]);
  data[22] = uint8_t(outerForm[2]);
  data[23] = uint8_t(outerForm[3]);
  data[24] = 0x3d;
  data[28] = 0x3e;
  PutFourCC(data, "DESC");
  const uint64_t chunkSize = 4 + 4 + properties.size(); // prefix + root form + property list
  PutLe64(data, chunkSize);
  PutLe32(data, 1); // chunk version/unknown
  PutLe64(data, 0); // padding before chunk data
  PutLe32(data, 0x00016e19); // DESC prefix used by the shipped assets
  PutReversedFourCC(data, rootForm);
  data.insert(data.end(), properties.begin(), properties.end());
  SetLe64(data, 4, uint64_t(data.size() - 32));
  return data;
}

struct Fixture {
  std::vector<uint8_t> retailWpsc;
  std::vector<uint8_t> retailCrsc;
  std::vector<uint8_t> remasteredWpsm;
  std::vector<uint8_t> remasteredCrsm;
};

Fixture MakeFixture(uint32_t crsc = kMissileCrsc, uint32_t part = kMissileDefaultPart,
                    const std::array<uint8_t, 16>& crsm = kMissileCrsmLink,
                    const std::array<uint8_t, 16>& genp = kMissileDefaultGenpLink,
                    bool retailDecoy = true, bool remasteredDecoy = false, bool duplicate = false,
                    bool includeRoleFields = true) {
  Fixture fixture;
  PutFourCC(fixture.retailWpsc, "WPSM");
  if (retailDecoy) {
    PutRetailVectorDecoy(fixture.retailWpsc, kMissileCrsc);
  }
  if (includeRoleFields) {
    PutRetailLink(fixture.retailWpsc, "COLR", crsc);
    if (duplicate) {
      PutRetailLink(fixture.retailWpsc, "COLR", crsc);
    }
  }
  PutFourCC(fixture.retailWpsc, "_END");

  PutFourCC(fixture.retailCrsc, "CRSM");
  if (includeRoleFields) {
    PutRetailLink(fixture.retailCrsc, "DEFS", part);
    if (duplicate) {
      PutRetailLink(fixture.retailCrsc, "DEFS", part);
    }
  }
  PutFourCC(fixture.retailCrsc, "_END");

  std::vector<uint8_t> weaponProperties;
  if (remasteredDecoy) {
    PutPvarDecoy(weaponProperties, crsm);
  }
  if (includeRoleFields) {
    PutReversedGuidLink(weaponProperties, "COLR", 0, crsm);
    if (duplicate) {
      PutReversedGuidLink(weaponProperties, "COLR", 0, crsm);
    }
  }
  PutReversedFourCC(weaponProperties, "_END");
  weaponProperties.push_back(4);
  fixture.remasteredWpsm = RemasteredForm("WPSM", "WPSM", std::move(weaponProperties));
  fixture.remasteredWpsm.insert(fixture.remasteredWpsm.end(), {'R', 'L', 'O', 'C', 0}); // footer decoy

  std::vector<uint8_t> collisionProperties;
  if (remasteredDecoy) {
    PutPvarDecoy(collisionProperties, genp);
  }
  if (includeRoleFields) {
    PutReversedGuidLink(collisionProperties, "DEFS", 4, genp);
    if (duplicate) {
      PutReversedGuidLink(collisionProperties, "DEFS", 4, genp);
    }
  }
  PutReversedFourCC(collisionProperties, "_END");
  collisionProperties.push_back(4);
  fixture.remasteredCrsm = RemasteredForm("CRSM", "CRSM", std::move(collisionProperties));
  return fixture;
}

MissileDefaultRoleInput Input(const Fixture& fixture) {
  MissileDefaultRoleInput input;
  input.weaponName = "missile";
  input.retailWpscId = kMissileWpsc;
  input.retailWpscExists = input.retailCrscExists = input.retailPartExists = true;
  input.remasteredWpsmExists = input.remasteredCrsmExists = input.remasteredGenpExists = true;
  input.retailWpsc = fixture.retailWpsc.data();
  input.retailWpscSize = fixture.retailWpsc.size();
  input.retailCrsc = fixture.retailCrsc.data();
  input.retailCrscSize = fixture.retailCrsc.size();
  input.remasteredWpsm = fixture.remasteredWpsm.data();
  input.remasteredWpsmSize = fixture.remasteredWpsm.size();
  input.remasteredCrsm = fixture.remasteredCrsm.data();
  input.remasteredCrsmSize = fixture.remasteredCrsm.size();
  return input;
}

void ExpectRejected(const Fixture& fixture, const char* what) {
  MissileDefaultRolePair pair;
  CHECK_MSG(!ResolveMissileDefaultRole(Input(fixture), pair), what);
}

Fixture MakePowerBallFixture(uint32_t crsc = kPowerBallCrsc, uint32_t part = kPowerBallDefaultPart,
                             const std::array<uint8_t, 16>& crsm = kPowerBallCrsmLink,
                             const std::array<uint8_t, 16>& genp = kPowerBallDefaultGenpLink,
                             bool retailDecoy = true, bool remasteredDecoy = false, bool duplicate = false,
                             bool includeRoleFields = true) {
  Fixture fixture;
  PutFourCC(fixture.retailWpsc, "WPSM");
  if (retailDecoy) {
    PutRetailVectorDecoy(fixture.retailWpsc, kPowerBallCrsc);
  }
  if (includeRoleFields) {
    PutRetailLink(fixture.retailWpsc, "COLR", crsc);
    if (duplicate) {
      PutRetailLink(fixture.retailWpsc, "COLR", crsc);
    }
  }
  PutFourCC(fixture.retailWpsc, "_END");

  PutFourCC(fixture.retailCrsc, "CRSM");
  if (includeRoleFields) {
    PutRetailLink(fixture.retailCrsc, "DEFS", part);
    if (duplicate) {
      PutRetailLink(fixture.retailCrsc, "DEFS", part);
    }
  }
  PutFourCC(fixture.retailCrsc, "_END");

  std::vector<uint8_t> weaponProperties;
  if (remasteredDecoy) {
    PutPvarDecoy(weaponProperties, crsm);
  }
  if (includeRoleFields) {
    PutReversedGuidLink(weaponProperties, "COLR", 0, crsm);
    if (duplicate) {
      PutReversedGuidLink(weaponProperties, "COLR", 0, crsm);
    }
  }
  PutReversedFourCC(weaponProperties, "_END");
  weaponProperties.push_back(4);
  fixture.remasteredWpsm = RemasteredForm("WPSM", "WPSM", std::move(weaponProperties));

  std::vector<uint8_t> collisionProperties;
  if (remasteredDecoy) {
    PutPvarDecoy(collisionProperties, genp);
  }
  if (includeRoleFields) {
    PutReversedGuidLink(collisionProperties, "DEFS", 4, genp);
    if (duplicate) {
      PutReversedGuidLink(collisionProperties, "DEFS", 4, genp);
    }
  }
  PutReversedFourCC(collisionProperties, "_END");
  collisionProperties.push_back(4);
  fixture.remasteredCrsm = RemasteredForm("CRSM", "CRSM", std::move(collisionProperties));
  return fixture;
}

PowerBallDefaultRoleInput PowerBallInput(const Fixture& fixture) {
  PowerBallDefaultRoleInput input;
  input.weaponName = "powerball";
  input.retailWpscId = kPowerBallWpsc;
  input.retailWpscExists = input.retailCrscExists = input.retailPartExists = true;
  input.remasteredWpsmExists = input.remasteredCrsmExists = input.remasteredGenpExists = true;
  input.retailWpsc = fixture.retailWpsc.data();
  input.retailWpscSize = fixture.retailWpsc.size();
  input.retailCrsc = fixture.retailCrsc.data();
  input.retailCrscSize = fixture.retailCrsc.size();
  input.remasteredWpsm = fixture.remasteredWpsm.data();
  input.remasteredWpsmSize = fixture.remasteredWpsm.size();
  input.remasteredCrsm = fixture.remasteredCrsm.data();
  input.remasteredCrsmSize = fixture.remasteredCrsm.size();
  return input;
}

void ExpectPowerBallRejected(const Fixture& fixture, const char* what) {
  PowerBallDefaultRolePair pair;
  CHECK_MSG(!ResolvePowerBallDefaultRole(PowerBallInput(fixture), pair), what);
}

enum class PlasmaRoleField { None, WpscColr, CrscDefs, WpsmColr, CrsmDefs };

Fixture MakePlasmaBeamFixture(uint32_t crsc = kPlasmaBeamCrsc, uint32_t part = kPlasmaBeamDefaultPart,
                              const std::array<uint8_t, 16>& crsm = kPlasmaBeamCrsmLink,
                              const std::array<uint8_t, 16>& genp = kPlasmaBeamDefaultGenpLink,
                              bool retailDecoy = true, bool duplicate = false, bool includeRoleFields = true,
                              uint8_t wpsmKind = 0, uint8_t crsmKind = 4,
                              PlasmaRoleField omittedField = PlasmaRoleField::None,
                              PlasmaRoleField duplicateField = PlasmaRoleField::None) {
  Fixture fixture;
  PutFourCC(fixture.retailWpsc, "WPSM");
  if (retailDecoy) {
    PutRetailVectorDecoy(fixture.retailWpsc, kPlasmaBeamCrsc);
  }
  if (includeRoleFields && omittedField != PlasmaRoleField::WpscColr) {
    PutRetailLink(fixture.retailWpsc, "COLR", crsc);
    if (duplicate || duplicateField == PlasmaRoleField::WpscColr) {
      PutRetailLink(fixture.retailWpsc, "COLR", crsc);
    }
  }
  PutFourCC(fixture.retailWpsc, "_END");

  PutFourCC(fixture.retailCrsc, "CRSM");
  if (includeRoleFields && omittedField != PlasmaRoleField::CrscDefs) {
    PutRetailLink(fixture.retailCrsc, "DEFS", part);
    if (duplicate || duplicateField == PlasmaRoleField::CrscDefs) {
      PutRetailLink(fixture.retailCrsc, "DEFS", part);
    }
  }
  PutFourCC(fixture.retailCrsc, "_END");

  std::vector<uint8_t> weaponProperties;
  if (includeRoleFields && omittedField != PlasmaRoleField::WpsmColr) {
    PutReversedGuidLink(weaponProperties, "COLR", wpsmKind, crsm);
    if (duplicate || duplicateField == PlasmaRoleField::WpsmColr) {
      PutReversedGuidLink(weaponProperties, "COLR", wpsmKind, crsm);
    }
  }
  const std::array<uint8_t, 16> nilGuid{};
  PutReversedGuidLink(weaponProperties, "APS2", 0, nilGuid);
  PutReversedFourCC(weaponProperties, "INRT");
  weaponProperties.push_back(0);
  PutReversedEulerRotation(weaponProperties);
  PutReversedFourCC(weaponProperties, "_END");
  weaponProperties.push_back(4);
  fixture.remasteredWpsm = RemasteredForm("WPSM", "WPSM", std::move(weaponProperties));

  std::vector<uint8_t> collisionProperties;
  if (includeRoleFields && omittedField != PlasmaRoleField::CrsmDefs) {
    PutReversedGuidLink(collisionProperties, "DEFS", crsmKind, genp);
    if (duplicate || duplicateField == PlasmaRoleField::CrsmDefs) {
      PutReversedGuidLink(collisionProperties, "DEFS", crsmKind, genp);
    }
  }
  constexpr std::array<uint8_t, 16> plasmaDecalGuid = {0x40, 0x3c, 0x04, 0x71, 0x0e, 0x58, 0x05, 0x4c,
                                                       0xb0, 0xb4, 0x4f, 0xe1, 0xf2, 0x0e, 0x11, 0x2d};
  PutReversedGuidLink(collisionProperties, "3MUD", 4, plasmaDecalGuid);
  PutReversedGuidLink(collisionProperties, "3LAV", 4, plasmaDecalGuid);
  PutReversedGuidLink(collisionProperties, "3SAN", 4, plasmaDecalGuid);
  PutReversedFourCC(collisionProperties, "_END");
  collisionProperties.push_back(4);
  fixture.remasteredCrsm = RemasteredForm("CRSM", "CRSM", std::move(collisionProperties));
  return fixture;
}

Fixture MakePlasmaBeamFixtureWithSingleRoleFieldChange(PlasmaRoleField field, bool duplicate) {
  return MakePlasmaBeamFixture(kPlasmaBeamCrsc, kPlasmaBeamDefaultPart, kPlasmaBeamCrsmLink,
                               kPlasmaBeamDefaultGenpLink, true, false, true, 0, 4,
                               duplicate ? PlasmaRoleField::None : field,
                               duplicate ? field : PlasmaRoleField::None);
}

PlasmaBeamDefaultRoleInput PlasmaBeamInput(const Fixture& fixture) {
  PlasmaBeamDefaultRoleInput input;
  input.weaponName = "plasmabeam";
  input.retailWpscId = kPlasmaBeamWpsc;
  input.retailWpscExists = input.retailCrscExists = input.retailPartExists = true;
  input.remasteredWpsmExists = input.remasteredCrsmExists = input.remasteredGenpExists = true;
  input.retailWpsc = fixture.retailWpsc.data();
  input.retailWpscSize = fixture.retailWpsc.size();
  input.retailCrsc = fixture.retailCrsc.data();
  input.retailCrscSize = fixture.retailCrsc.size();
  input.remasteredWpsm = fixture.remasteredWpsm.data();
  input.remasteredWpsmSize = fixture.remasteredWpsm.size();
  input.remasteredCrsm = fixture.remasteredCrsm.data();
  input.remasteredCrsmSize = fixture.remasteredCrsm.size();
  return input;
}

void ExpectPlasmaBeamRejected(const Fixture& fixture, const char* what) {
  PlasmaBeamDefaultRolePair pair;
  CHECK_MSG(!ResolvePlasmaBeamDefaultRole(PlasmaBeamInput(fixture), pair), what);
}

void TestExactPairAndNestedDecoys() {
  const Fixture fixture = MakeFixture();
  MissileDefaultRolePair pair;
  CHECK_MSG(ResolveMissileDefaultRole(Input(fixture), pair), "the exact missile default chain resolves with a retail vector decoy");
  CHECK_MSG(pair.genp == kMissileDefaultGenp && pair.retailPart == kMissileDefaultPart,
            "the pair is the pinned full GENP GUID and retail PART id");
  uint32_t retailLink = 0;
  CHECK_MSG(detail::ReadUniqueRetailAssetId(fixture.retailWpsc.data(), fixture.retailWpsc.size(), "WPSM", "COLR",
                                       retailLink) && retailLink == kMissileCrsc,
        "a nested retail decoy is ignored by the top-level field reader");
  Fixture trailingDecoy = MakeFixture();
  PutRetailLink(trailingDecoy.retailWpsc, "COLR", kMissileCrsc);
  MissileDefaultRolePair trailingPair;
  CHECK_MSG(ResolveMissileDefaultRole(Input(trailingDecoy), trailingPair),
            "retail bytes after the factory's _END do not create a duplicate role link");
  ExpectRejected(MakeFixture(kMissileCrsc, kMissileDefaultPart, kMissileCrsmLink, kMissileDefaultGenpLink, false,
                             false, false, false),
                 "role fields missing from the top-level WPSC and CRSC are rejected");
}

void TestRetailSchemaAndBounds() {
  ExpectRejected(MakeFixture(kMissileCrsc + 1), "wrong retail CRSC id is rejected");
  ExpectRejected(MakeFixture(kMissileCrsc, kMissileDefaultPart + 1), "wrong retail default PART id is rejected");
  ExpectRejected(MakeFixture(kMissileCrsc, kMissileDefaultPart, kMissileCrsmLink, kMissileDefaultGenpLink, false,
                             false, true),
                 "duplicate retail role fields are rejected");

  Fixture fixture = MakeFixture();
  fixture.retailWpsc.pop_back();
  ExpectRejected(fixture, "truncated retail WPSC stream is rejected");
  fixture = MakeFixture();
  fixture.retailCrsc[0] = 'X';
  ExpectRejected(fixture, "wrong retail CRSC class id is rejected");
  fixture = MakeFixture();
  fixture.retailWpsc[0] = 'X';
  ExpectRejected(fixture, "wrong retail WPSC class id is rejected");
  fixture = MakeFixture();
  fixture.retailWpsc.insert(fixture.retailWpsc.end() - 4, {'B', 'A', 'D', '_'});
  MissileDefaultRoleInput input = Input(fixture);
  MissileDefaultRolePair pair;
  std::string error;
  CHECK_MSG(!ResolveMissileDefaultRole(input, pair, &error) && error.find("unknown factory field BAD_") != std::string::npos,
            "unknown retail WPSC fields fail closed with a precise diagnostic");
}

void TestRemasteredSchemaAndBounds() {
  ExpectRejected(MakeFixture(kMissileCrsc, kMissileDefaultPart, kMissileCrsmLink, kMissileDefaultGenpLink,
                             true, false, true),
                 "duplicate Remastered top-level role fields are rejected");
  auto changedCrsm = kMissileCrsmLink;
  changedCrsm[0] ^= 1;
  ExpectRejected(MakeFixture(kMissileCrsc, kMissileDefaultPart, changedCrsm), "wrong full CRSM GUID is rejected");
  auto changedGenp = kMissileDefaultGenpLink;
  changedGenp[0] ^= 1;
  ExpectRejected(MakeFixture(kMissileCrsc, kMissileDefaultPart, kMissileCrsmLink, changedGenp),
                 "wrong full GENP GUID is rejected");
  Fixture fixture = MakeFixture();
  SetLe64(fixture.remasteredWpsm, 4, 0); // form-size no longer covers DESC
  ExpectRejected(fixture, "truncated RFRM form size is rejected");
  fixture = MakeFixture();
  SetLe64(fixture.remasteredWpsm, 4, UINT64_MAX);
  ExpectRejected(fixture, "oversized RFRM form declaration is rejected without offset overflow");
  fixture = MakeFixture();
  fixture.remasteredCrsm[36] ^= 1; // DESC chunk size
  ExpectRejected(fixture, "truncated DESC chunk size is rejected");
  fixture = MakeFixture();
  SetLe64(fixture.remasteredWpsm, 36, UINT64_MAX);
  ExpectRejected(fixture, "oversized DESC chunk declaration is rejected without offset overflow");
  fixture = MakeFixture();
  SetLe64(fixture.remasteredWpsm, 48, UINT64_MAX);
  ExpectRejected(fixture, "oversized DESC skip declaration is rejected without offset overflow");
  fixture = MakeFixture();
  fixture.remasteredWpsm.resize(40);
  ExpectRejected(fixture, "truncated RFRM/DESC header is rejected");
  fixture = MakeFixture();
  fixture.remasteredWpsm[63] = 'X'; // outer WPSM form still declares WPSM, root is wrong
  ExpectRejected(fixture, "wrong top-level nested form type is rejected");
  fixture = MakeFixture();
  fixture.remasteredWpsm[23] = 'X'; // root is WPSM, outer RFRM id is wrong
  ExpectRejected(fixture, "wrong RFRM form id is rejected");
  fixture = MakeFixture(kMissileCrsc, kMissileDefaultPart, kMissileCrsmLink, kMissileDefaultGenpLink,
                        false, false);
  fixture.remasteredWpsm[60] = 'T'; // reverse COLR into an unrelated top-level field
  ExpectRejected(fixture, "wrong top-level WPSM field is rejected");
  fixture = MakeFixture(kMissileCrsc, kMissileDefaultPart, kMissileCrsmLink, kMissileDefaultGenpLink,
                        false, false);
  fixture.remasteredWpsm[68] = 6; // the COLR property tag follows its FourCC
  ExpectRejected(fixture, "invalid top-level COLR tag is rejected");

  // Footer bytes are outside the declared form and do not participate in the
  // allowlisted top-level field visit.
  fixture = MakeFixture();
  PortRemastered::EffectNode node;
  std::string error;
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(fixture.remasteredWpsm.data(), fixture.remasteredWpsm.size(),
                                                      PortRemastered::EffectFourCC("WPSM"), node, error),
            "valid resource parses while ignoring footer bytes beyond the RFRM form");
}

void TestStrictRolePropertySchemas() {
  const auto roleInputWithProperties = [](Fixture& fixture, std::vector<uint8_t> properties) {
    PutReversedFourCC(properties, "_END");
    properties.push_back(4);
    fixture.remasteredWpsm = RemasteredForm("WPSM", "WPSM", std::move(properties));
    fixture.remasteredWpsm.insert(fixture.remasteredWpsm.end(), {'R', 'L', 'O', 'C', 0});
  };

  // This is the regression from review-structured.md: the unknown property's
  // byte fallback consumes 00 and promotes the following reversed COLR bytes.
  Fixture decoy = MakeFixture();
  std::vector<uint8_t> properties;
  PutReversedFourCC(properties, "ZZZZ");
  properties.push_back(0);
  properties.push_back(0);
  PutReversedGuidLink(properties, "COLR", 0, kMissileCrsmLink);
  roleInputWithProperties(decoy, std::move(properties));
  MissileDefaultRolePair pair;
  std::string error;
  CHECK_MSG(!ResolveMissileDefaultRole(Input(decoy), pair, &error),
            "unknown-property payload bytes cannot be promoted into a role link");
  CHECK_MSG(error.find("unknown role property ZZZZ at 0x40") != std::string::npos,
            "unknown role property failure reports its FourCC and absolute offset");

  // Even with a real, correctly typed link after it, an unknown property has
  // no proven boundary and invalidates the complete role asset.
  Fixture beforeRealLink = MakeFixture();
  properties.clear();
  PutReversedFourCC(properties, "ZZZZ");
  properties.push_back(0);
  properties.push_back(0x7f);
  PutReversedGuidLink(properties, "COLR", 0, kMissileCrsmLink);
  roleInputWithProperties(beforeRealLink, std::move(properties));
  error.clear();
  CHECK_MSG(!ResolveMissileDefaultRole(Input(beforeRealLink), pair, &error) &&
                error.find("unknown role property ZZZZ") != std::string::npos,
            "unknown property before a true COLR link fails closed");

  // PVAR is a globally known property but is not a WPSM factory field, even
  // though its payload is length-delimited and contains no real role link.
  Fixture globalPvar = MakeFixture();
  properties.clear();
  PutPvarDecoy(properties, kMissileCrsmLink);
  PutReversedGuidLink(properties, "COLR", 0, kMissileCrsmLink);
  roleInputWithProperties(globalPvar, std::move(properties));
  error.clear();
  CHECK_MSG(!ResolveMissileDefaultRole(Input(globalPvar), pair, &error) &&
            error.find("unknown role property PVAR") != std::string::npos,
        "globally-known PVAR is rejected when it is absent from the WPSM factory allowlist");

  // Strictness is specific to the role-link parser; the general GENP converter
  // still accepts its historical permissive unknown-property fixtures.
  std::vector<uint8_t> effect(0x3c, 0);
  std::memcpy(effect.data(), "RFRM", 4);
  std::memcpy(effect.data() + 0x14, "GENP", 4);
  PutReversedFourCC(effect, "GPSM");
  effect.insert(effect.end(), 17, 0);
  PutLe32(effect, 1); // root generator
  PutReversedFourCC(effect, "ZZZZ");
  effect.push_back(0);
  effect.push_back(0);
  PutReversedFourCC(effect, "_END");
  effect.push_back(4);
  PutLe32(effect, 0); // no child generators
  PortRemastered::EffectNode node;
  error.clear();
  CHECK_MSG(PortRemastered::ParseEffect(effect.data(), effect.size(), node, error),
            "standard ParseEffect retains permissive unknown-property behavior");
}

void TestStrictRoleAllowlistAndBudgets() {
  PortRemastered::EffectNode node;
  std::string error;

  // SIZE is globally known and has a generic byte fallback, but it is not a
  // WPSM factory field. The old path consumed the byte and promoted COLR.
  std::vector<uint8_t> sizeDecoy;
  PutReversedFourCC(sizeDecoy, "SIZE");
  sizeDecoy.push_back(0);
  sizeDecoy.push_back(0);
  PutReversedGuidLink(sizeDecoy, "COLR", 0, kMissileCrsmLink);
  PutReversedFourCC(sizeDecoy, "_END");
  sizeDecoy.push_back(4);
  const std::vector<uint8_t> sizeDecoyAsset = RemasteredForm("WPSM", "WPSM", std::move(sizeDecoy));
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(sizeDecoyAsset.data(), sizeDecoyAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error) &&
                error.find("unknown role property SIZE") != std::string::npos,
            "globally-known SIZE cannot use a generic fallback or promote its COLR decoy");

  Fixture knownGlobalBeforeLink = MakeFixture();
  std::vector<uint8_t> properties;
  PutReversedFourCC(properties, "SIZE");
  properties.push_back(0);
  properties.push_back(0x7f);
  PutReversedGuidLink(properties, "COLR", 0, kMissileCrsmLink);
  PutReversedFourCC(properties, "_END");
  properties.push_back(4);
  knownGlobalBeforeLink.remasteredWpsm = RemasteredForm("WPSM", "WPSM", std::move(properties));
  MissileDefaultRolePair pair;
  error.clear();
  CHECK_MSG(!ResolveMissileDefaultRole(Input(knownGlobalBeforeLink), pair, &error) &&
                error.find("unknown role property SIZE") != std::string::npos,
            "globally-known but factory-unknown root field before a real COLR is rejected");

  const auto parseOneField = [&](const char (&root)[5], const char (&field)[5]) {
    std::vector<uint8_t> stream;
    PutReversedGuidLink(stream, field, 0, kMissileCrsmLink);
    PutReversedFourCC(stream, "_END");
    stream.push_back(4);
    const std::vector<uint8_t> asset = RemasteredForm(root, root, std::move(stream));
    error.clear();
    return PortRemastered::ParseRemasteredRoleAsset(asset.data(), asset.size(),
                                                     PortRemastered::EffectFourCC(root), node, error);
  };
  CHECK_MSG(!parseOneField("WPSM", "DEFS") && error.find("unknown role property DEFS") != std::string::npos,
            "CRSM-only DEFS is rejected at a WPSM root");
  CHECK_MSG(!parseOneField("CRSM", "COLR") && error.find("unknown role property COLR") != std::string::npos,
        "WPSM-only COLR is rejected at a CRSM root");

  std::vector<uint8_t> atLimit;
  for (size_t i = 0; i < 1024; ++i) {
    PutReversedBoolProperty(atLimit, "FC60", uint8_t(i & 1));
  }
  PutReversedFourCC(atLimit, "_END");
  atLimit.push_back(4);
  const std::vector<uint8_t> atLimitAsset = RemasteredForm("WPSM", "WPSM", std::move(atLimit));
  error.clear();
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(atLimitAsset.data(), atLimitAsset.size(),
                                                      PortRemastered::EffectFourCC("WPSM"), node, error) &&
                node.properties.size() == 1024,
            "the inclusive 1024-record boundary parses without semantic link rejection");

  std::vector<uint8_t> tooMany;
  for (size_t i = 0; i < 4097; ++i) {
    PutReversedBoolProperty(tooMany, "FC60", uint8_t(i & 1));
  }
  PutReversedFourCC(tooMany, "_END");
  tooMany.push_back(4);
  const std::vector<uint8_t> tooManyAsset = RemasteredForm("WPSM", "WPSM", std::move(tooMany));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(tooManyAsset.data(), tooManyAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error) &&
                error.find("property-count limit") != std::string::npos,
            "4097 fixed-size root records fail closed at the bounded record limit");

  std::vector<uint8_t> nodeBudget;
  for (size_t i = 0; i < 512; ++i) {
    PutReversedFourCC(nodeBudget, "PSCL");
    nodeBudget.push_back(1);
    PutReversedFourCC(nodeBudget, "MPCB");
    PutReversedFourCC(nodeBudget, "CNST");
    for (size_t component = 0; component < 3; ++component) {
      PutReversedFourCC(nodeBudget, "CNST");
      PutLe32(nodeBudget, 0x3f800000);
    }
  }
  PutReversedFourCC(nodeBudget, "_END");
  nodeBudget.push_back(4);
  const std::vector<uint8_t> nodeBudgetAsset = RemasteredForm("WPSM", "WPSM", std::move(nodeBudget));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(nodeBudgetAsset.data(), nodeBudgetAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error) &&
                error.find("value-node budget") != std::string::npos,
            "typed expression nodes are bounded independently of the root property count");

  std::vector<uint8_t> deep;
  PutReversedFourCC(deep, "PSCL");
  deep.push_back(1);
  constexpr size_t kDeepCones = 80;
  for (size_t i = 0; i < kDeepCones; ++i) {
    PutReversedFourCC(deep, "CONE");
  }
  PutReversedFourCC(deep, "CNST");
  for (size_t i = 0; i < 3; ++i) {
    PutReversedFourCC(deep, "CNST");
    PutLe32(deep, 0x3f800000);
  }
  for (size_t i = 0; i < kDeepCones; ++i) {
    PutReversedFourCC(deep, "CNST");
    PutLe32(deep, 0x3f800000);
  }
  PutReversedFourCC(deep, "_END");
  deep.push_back(4);
  const std::vector<uint8_t> deepAsset = RemasteredForm("WPSM", "WPSM", std::move(deep));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(deepAsset.data(), deepAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error) &&
                error.find("expression-depth limit") != std::string::npos,
            "deep typed expressions reject at the explicit depth limit");
}

void TestStrictRoleKeyframeWidths() {
  PortRemastered::EffectNode node;
  std::string error;

  // Review repro: the old role parser accepted one of several candidate key
  // widths, read only 7 * 4 bytes from a vector KEYE, and promoted the next
  // COLR. Bytes 4..7 also make the alternate 26-byte header's count invalid.
  Fixture fakeVectorLink = MakeFixture();
  std::vector<uint8_t> properties;
  PutReversedFourCC(properties, "IVEC");
  properties.push_back(0);
  PutReversedFourCC(properties, "KEYE");
  properties.insert(properties.end(), 18, 0);
  PutLe32(properties, 7);
  std::vector<uint8_t> fakeVectorKeys(28, 0);
  fakeVectorKeys[4] = 0x01;
  fakeVectorKeys[5] = 0x10; // 4097 little-endian at the alternate count offset
  properties.insert(properties.end(), fakeVectorKeys.begin(), fakeVectorKeys.end());
  PutReversedGuidLink(properties, "COLR", 0, kMissileCrsmLink);
  PutReversedFourCC(properties, "_END");
  properties.push_back(4);
  fakeVectorLink.remasteredWpsm = RemasteredForm("WPSM", "WPSM", std::move(properties));
  MissileDefaultRolePair pair;
  error.clear();
  CHECK_MSG(!ResolveMissileDefaultRole(Input(fakeVectorLink), pair, &error),
        "a 4-byte-per-key IVEC KEYE cannot promote a following COLR");

  // A genuine vector KEYE has seven CVector3f payloads (12 bytes each). A
  // reversed COLR token inside one vector must remain key data; the later real
  // root COLR is still found at the schema-determined boundary.
  Fixture vectorKeys = MakeFixture();
  properties.clear();
  PutRoleKeyframeProperty(properties, "IVEC", "KEYE", 7, 12);
  const size_t vectorKeyData = 5 + 4 + 18 + 4;
  properties[vectorKeyData + 12] = 'R';
  properties[vectorKeyData + 13] = 'L';
  properties[vectorKeyData + 14] = 'O';
  properties[vectorKeyData + 15] = 'C'; // reversed FourCC COLR within key 1
  PutReversedGuidLink(properties, "COLR", 0, kMissileCrsmLink);
  PutReversedFourCC(properties, "_END");
  properties.push_back(4);
  vectorKeys.remasteredWpsm = RemasteredForm("WPSM", "WPSM", std::move(properties));
  error.clear();
  CHECK_MSG(ResolveMissileDefaultRole(Input(vectorKeys), pair, &error) &&
                pair.genp == kMissileDefaultGenp && pair.retailPart == kMissileDefaultPart,
            "a valid 12-byte vector KEYE ignores embedded COLR bytes and resolves the later root link");
  bool vectorKeyWidthParsed = false;
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(vectorKeys.remasteredWpsm.data(), vectorKeys.remasteredWpsm.size(),
                                                     PortRemastered::EffectFourCC("WPSM"), node, error),
            "strict role parser accepts the complete vector KEYE block");
  if (!node.properties.empty() && node.properties.front().value.size() == 1) {
    const PortRemastered::EffectValue& value = node.properties.front().value.front();
    vectorKeyWidthParsed = value.kind == PortRemastered::EffectValue::Kind::Element &&
                           value.fourcc == PortRemastered::EffectFourCC("KEYE") &&
                           value.args.size() == 1 &&
                           value.args.front().kind == PortRemastered::EffectValue::Kind::Keys &&
                           value.args.front().size == 22 + 7 * 12;
  }
  CHECK_MSG(vectorKeyWidthParsed, "vector KEYE records expose their exact factory-sized key payload");

  // Real and integer factory key emitters store one 32-bit value per key.
  Fixture scalarKeys = MakeFixture();
  properties.clear();
  PutRoleKeyframeProperty(properties, "TRAT", "KEYE", 3, 4);
  PutRoleKeyframeProperty(properties, "PSLT", "KEYP", 2, 4);
  PutReversedGuidLink(properties, "COLR", 0, kMissileCrsmLink);
  PutReversedFourCC(properties, "_END");
  properties.push_back(4);
  scalarKeys.remasteredWpsm = RemasteredForm("WPSM", "WPSM", std::move(properties));
  error.clear();
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(scalarKeys.remasteredWpsm.data(), scalarKeys.remasteredWpsm.size(),
                                                      PortRemastered::EffectFourCC("WPSM"), node, error),
            "real KEYE and integer KEYP use their proven four-byte key widths");
  bool scalarKeyWidthsParsed = false;
  bool realKeyWidthParsed = false;
  bool intKeyWidthParsed = false;
  for (const PortRemastered::EffectProperty& property : node.properties) {
    if (property.value.size() != 1 || property.value.front().args.size() != 1 ||
        property.value.front().args.front().kind != PortRemastered::EffectValue::Kind::Keys) {
      continue;
    }
    if (property.fourcc == PortRemastered::EffectFourCC("TRAT")) {
      realKeyWidthParsed = property.value.front().args.front().size == 22 + 3 * 4;
    } else if (property.fourcc == PortRemastered::EffectFourCC("PSLT")) {
      intKeyWidthParsed = property.value.front().args.front().size == 22 + 2 * 4;
    }
  }
  scalarKeyWidthsParsed = realKeyWidthParsed && intKeyWidthParsed;
  CHECK_MSG(scalarKeyWidthsParsed, "real KEYE and integer KEYP expose exact 4-byte key payloads");

  // CColor(CInputStream&) reads four floats, so a vector CTVC carrying a color
  // KEYE has a source-proven 16-byte key width (not an inferred candidate).
  Fixture colorKeys = MakeFixture();
  properties.clear();
  PutReversedFourCC(properties, "PSCL");
  properties.push_back(0);
  PutReversedFourCC(properties, "CTVC");
  PutReversedFourCC(properties, "KEYE");
  properties.insert(properties.end(), 18, 0);
  PutLe32(properties, 2);
  const size_t colorDataAt = properties.size();
  properties.insert(properties.end(), 2 * 16, 0);
  properties[colorDataAt + 4] = 'R';
  properties[colorDataAt + 5] = 'L';
  properties[colorDataAt + 6] = 'O';
  properties[colorDataAt + 7] = 'C';
  PutReversedGuidLink(properties, "COLR", 0, kMissileCrsmLink);
  PutReversedFourCC(properties, "_END");
  properties.push_back(4);
  colorKeys.remasteredWpsm = RemasteredForm("WPSM", "WPSM", std::move(properties));
  error.clear();
  CHECK_MSG(ResolveMissileDefaultRole(Input(colorKeys), pair, &error),
            "source-proven 16-byte color KEYE ignores embedded COLR bytes and preserves the later role link");
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(colorKeys.remasteredWpsm.data(), colorKeys.remasteredWpsm.size(),
                                                      PortRemastered::EffectFourCC("WPSM"), node, error),
            "strict parser accepts the complete color KEYE block");
  bool colorKeyWidthParsed = false;
  if (!node.properties.empty() && node.properties.front().value.size() == 1 &&
      node.properties.front().value.front().args.size() == 1) {
    const PortRemastered::EffectValue& keyEmitter = node.properties.front().value.front().args.front();
    colorKeyWidthParsed = keyEmitter.kind == PortRemastered::EffectValue::Kind::Element &&
                          keyEmitter.fourcc == PortRemastered::EffectFourCC("KEYE") &&
                          keyEmitter.args.size() == 1 &&
                          keyEmitter.args.front().kind == PortRemastered::EffectValue::Kind::Keys &&
                          keyEmitter.args.front().size == 22 + 2 * 16;
  }
  CHECK_MSG(colorKeyWidthParsed, "color KEYE records expose their exact 16-byte factory-sized payload");

  std::vector<uint8_t> truncatedVector;
  PutReversedFourCC(truncatedVector, "IVEC");
  truncatedVector.push_back(0);
  PutReversedFourCC(truncatedVector, "KEYE");
  truncatedVector.insert(truncatedVector.end(), 18, 0);
  PutLe32(truncatedVector, 1);
  PutLe32(truncatedVector, 0x3f800000); // only four bytes, not one CVector3f
  PutReversedFourCC(truncatedVector, "_END");
  truncatedVector.push_back(4);
  const auto truncatedVectorAsset = RemasteredForm("WPSM", "WPSM", std::move(truncatedVector));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(truncatedVectorAsset.data(), truncatedVectorAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error),
            "a vector KEYE truncated to one scalar key fails closed");

  std::vector<uint8_t> excessiveCount;
  PutReversedFourCC(excessiveCount, "IVEC");
  excessiveCount.push_back(0);
  PutReversedFourCC(excessiveCount, "KEYE");
  excessiveCount.insert(excessiveCount.end(), 18, 0);
  PutLe32(excessiveCount, 4097);
  PutReversedFourCC(excessiveCount, "_END");
  excessiveCount.push_back(4);
  const auto excessiveCountAsset = RemasteredForm("WPSM", "WPSM", std::move(excessiveCount));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(excessiveCountAsset.data(), excessiveCountAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error) &&
                error.find("value-node budget") != std::string::npos,
            "keyframe count over the per-asset node budget fails before length arithmetic");

  std::vector<uint8_t> longHeaderOnly;
  PutReversedFourCC(longHeaderOnly, "IVEC");
  longHeaderOnly.push_back(0);
  PutReversedFourCC(longHeaderOnly, "KEYE");
  longHeaderOnly.insert(longHeaderOnly.end(), 18, 0);
  PutLe32(longHeaderOnly, 4097); // no 26-byte KEYE header variant is guessed
  PutLe32(longHeaderOnly, 0);
  PutLe32(longHeaderOnly, 0);
  PutLe32(longHeaderOnly, 1); // would be the long-header count at +26
  PutLe32(longHeaderOnly, 0x3f800000);
  PutReversedFourCC(longHeaderOnly, "_END");
  longHeaderOnly.push_back(4);
  const auto longHeaderAsset = RemasteredForm("WPSM", "WPSM", std::move(longHeaderOnly));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(longHeaderAsset.data(), longHeaderAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error),
            "strict KEYE parsing does not backtrack to a generic 26-byte header candidate");

  std::vector<uint8_t> untypedKey;
  PutReversedFourCC(untypedKey, "PSCL");
  untypedKey.push_back(0);
  PutReversedFourCC(untypedKey, "ANCR"); // its first argument is generic `e`, not a factory-typed slot
  PutReversedFourCC(untypedKey, "KEYE");
  untypedKey.insert(untypedKey.end(), 18, 0);
  PutLe32(untypedKey, 1);
  PutLe32(untypedKey, 0x3f800000);
  PutReversedFourCC(untypedKey, "_END");
  untypedKey.push_back(4);
  const auto untypedKeyAsset = RemasteredForm("WPSM", "WPSM", std::move(untypedKey));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(untypedKeyAsset.data(), untypedKeyAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error),
            "generic untyped KEYE without factory-known value type is rejected");
}

void TestProvenRolePropertySchemas() {
  const std::array<uint8_t, 16> apsmGuid = {0x8e, 0x9f, 0x45, 0x67, 0xda, 0x8f, 0xad, 0x44,
                                            0x90, 0xbc, 0x59, 0xf2, 0xf5, 0x71, 0xd8, 0x99};
  std::vector<uint8_t> wpsmProperties;
  PutReversedGuidLink(wpsmProperties, "APSM", 0, apsmGuid);
  PutReversedBoolProperty(wpsmProperties, "FC60", 1);
  PutReversedBoolProperty(wpsmProperties, "SPS1", 1);
  PutReversedBoolProperty(wpsmProperties, "HOMG", 1);
  PutReversedBoolProperty(wpsmProperties, "AP11", 1);
  PutReversedBoolProperty(wpsmProperties, "AP21", 1);
  PutReversedBoolProperty(wpsmProperties, "APSO", 1);
  PutReversedBoolProperty(wpsmProperties, "SPS2", 0);
  PutReversedBoolProperty(wpsmProperties, "AS11", 0);
  PutReversedBoolProperty(wpsmProperties, "AS12", 0);
  PutReversedBoolProperty(wpsmProperties, "AS13", 0);
  PutReversedBoolProperty(wpsmProperties, "EWTR", 1);
  PutReversedBoolProperty(wpsmProperties, "LWTR", 1);
  PutReversedBoolProperty(wpsmProperties, "SWTR", 1);
  PutReversedGuidLink(wpsmProperties, "COLR", 0, kMissileCrsmLink);
  PutReversedFourCC(wpsmProperties, "_END");
  wpsmProperties.push_back(4);
  const std::vector<uint8_t> wpsm = RemasteredForm("WPSM", "WPSM", std::move(wpsmProperties));
  PortRemastered::EffectNode node;
  std::string error;
  bool apsmParsed = false;
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(wpsm.data(), wpsm.size(), PortRemastered::EffectFourCC("WPSM"),
                                                      node, error),
            "WPSM APSM parses under its factory-backed GUID schema");
  for (const PortRemastered::EffectProperty& property : node.properties) {
    if (property.fourcc == PortRemastered::EffectFourCC("APSM")) {
      apsmParsed = property.value.size() == 1 &&
                   property.value.front().kind == PortRemastered::EffectValue::Kind::Guid &&
                   property.value.front().guid == apsmGuid;
    }
  }
  CHECK_MSG(apsmParsed, "WPSM APSM exposes the exact typed GUID value");
  const std::array<std::pair<uint32_t, uint8_t>, 13> expectedBools = {{
      {PortRemastered::EffectFourCC("FC60"), 1}, {PortRemastered::EffectFourCC("SPS1"), 1},
      {PortRemastered::EffectFourCC("HOMG"), 1}, {PortRemastered::EffectFourCC("AP11"), 1},
      {PortRemastered::EffectFourCC("AP21"), 1}, {PortRemastered::EffectFourCC("APSO"), 1},
      {PortRemastered::EffectFourCC("SPS2"), 0}, {PortRemastered::EffectFourCC("AS11"), 0},
      {PortRemastered::EffectFourCC("AS12"), 0}, {PortRemastered::EffectFourCC("AS13"), 0},
      {PortRemastered::EffectFourCC("EWTR"), 1}, {PortRemastered::EffectFourCC("LWTR"), 1},
      {PortRemastered::EffectFourCC("SWTR"), 1},
  }};
  size_t parsedRoleBools = 0;
  for (const PortRemastered::EffectProperty& property : node.properties) {
    const auto expected = std::find_if(expectedBools.begin(), expectedBools.end(), [&](const auto& entry) {
      return entry.first == property.fourcc;
    });
    if (expected == expectedBools.end()) {
      continue;
    }
    if (property.value.size() == 1 && property.value.front().kind == PortRemastered::EffectValue::Kind::Element &&
        property.value.front().fourcc == PortRemastered::EffectFourCC("CNST") &&
        property.value.front().args.size() == 1 &&
        property.value.front().args.front().kind == PortRemastered::EffectValue::Kind::Byte &&
        property.value.front().args.front().word == expected->second) {
      ++parsedRoleBools;
    }
  }
  CHECK_MSG(parsedRoleBools == expectedBools.size(),
            "all production WPSM boolean fields retain their exact CNST byte values");
  std::vector<uint8_t> truncatedApsm;
  PutReversedFourCC(truncatedApsm, "APSM");
  truncatedApsm.push_back(0);
  truncatedApsm.insert(truncatedApsm.end(), apsmGuid.begin(), apsmGuid.begin() + 8);
  PutReversedFourCC(truncatedApsm, "_END");
  truncatedApsm.push_back(4);
  const std::vector<uint8_t> truncatedApsmAsset = RemasteredForm("WPSM", "WPSM", std::move(truncatedApsm));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(truncatedApsmAsset.data(), truncatedApsmAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error),
            "WPSM APSM's fixed GUID schema rejects a truncated value");

  std::vector<uint8_t> crsmProperties;
  PutReversedGuidLink(crsmProperties, "BHFX", 4, kMissileDefaultGenpLink);
  PutReversedGuidLink(crsmProperties, "DEFS", 4, kMissileDefaultGenpLink);
  PutReversedFourCC(crsmProperties, "_END");
  crsmProperties.push_back(4);
  const std::vector<uint8_t> crsm = RemasteredForm("CRSM", "CRSM", std::move(crsmProperties));
  error.clear();
  bool bhfxParsed = false;
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(crsm.data(), crsm.size(), PortRemastered::EffectFourCC("CRSM"),
                                                      node, error),
            "CRSM BHFX parses under its factory-backed GUID schema");
  for (const PortRemastered::EffectProperty& property : node.properties) {
    if (property.fourcc == PortRemastered::EffectFourCC("BHFX")) {
      bhfxParsed = property.tag == 4 && property.value.size() == 1 &&
                   property.value.front().kind == PortRemastered::EffectValue::Kind::Guid &&
                   property.value.front().guid == kMissileDefaultGenpLink;
    }
  }
  CHECK_MSG(bhfxParsed, "CRSM BHFX exposes the exact typed GUID value");
  std::vector<uint8_t> truncatedBhfx;
  PutReversedFourCC(truncatedBhfx, "BHFX");
  truncatedBhfx.push_back(4);
  truncatedBhfx.insert(truncatedBhfx.end(), kMissileDefaultGenpLink.begin(), kMissileDefaultGenpLink.begin() + 8);
  PutReversedFourCC(truncatedBhfx, "_END");
  truncatedBhfx.push_back(4);
  const std::vector<uint8_t> truncatedBhfxAsset = RemasteredForm("CRSM", "CRSM", std::move(truncatedBhfx));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(truncatedBhfxAsset.data(), truncatedBhfxAsset.size(),
                                                       PortRemastered::EffectFourCC("CRSM"), node, error),
            "CRSM BHFX's fixed GUID schema rejects a truncated value");

  const std::vector<uint8_t> wrongForm = RemasteredForm("WPSM", "WPSM", [&] {
    std::vector<uint8_t> properties;
    PutReversedGuidLink(properties, "BHFX", 4, kMissileDefaultGenpLink);
    PutReversedFourCC(properties, "_END");
    properties.push_back(4);
    return properties;
  }());
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(wrongForm.data(), wrongForm.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error) &&
                error.find("unknown role property BHFX") != std::string::npos,
            "CRSM-only BHFX schema does not make the field generally permissive");

  std::vector<uint8_t> malformedBool;
  PutReversedFourCC(malformedBool, "FC60");
  malformedBool.push_back(0);
  PutReversedFourCC(malformedBool, "CNST");
  PutLe32(malformedBool, 1); // a bool is a CNST byte, not a word
  PutReversedFourCC(malformedBool, "_END");
  malformedBool.push_back(4);
  const std::vector<uint8_t> malformedBoolAsset = RemasteredForm("WPSM", "WPSM", std::move(malformedBool));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(malformedBoolAsset.data(), malformedBoolAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error),
            "factory-backed boolean schema rejects a word-shaped CNST payload");

  std::vector<uint8_t> tratProperties;
  PutReversedFourCC(tratProperties, "TRAT");
  tratProperties.push_back(1);
  PutReversedFourCC(tratProperties, "CNST");
  PutLe32(tratProperties, 0x43b40000); // 360.0f, the pinned asset's TRAT
  PutReversedFourCC(tratProperties, "_END");
  tratProperties.push_back(4);
  const std::vector<uint8_t> tratAsset = RemasteredForm("WPSM", "WPSM", std::move(tratProperties));
  error.clear();
  bool tratParsed = false;
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(tratAsset.data(), tratAsset.size(),
                                                      PortRemastered::EffectFourCC("WPSM"), node, error),
            "WPSM TRAT parses as one factory-typed real element");
  for (const PortRemastered::EffectProperty& property : node.properties) {
    if (property.fourcc == PortRemastered::EffectFourCC("TRAT")) {
      tratParsed = property.tag == 1 && property.value.size() == 1 &&
                   property.value.front().kind == PortRemastered::EffectValue::Kind::Element &&
                   property.value.front().fourcc == PortRemastered::EffectFourCC("CNST") &&
                   property.value.front().args.size() == 1 &&
                   property.value.front().args.front().kind == PortRemastered::EffectValue::Kind::Word &&
                   property.value.front().args.front().word == 0x43b40000;
    }
  }
  CHECK_MSG(tratParsed, "WPSM TRAT retains the exact tag and real CNST word");

  std::vector<uint8_t> malformedTrat;
  PutReversedFourCC(malformedTrat, "TRAT");
  malformedTrat.push_back(1);
  PutLe32(malformedTrat, 0x43b40000); // raw float without the factory's element FourCC
  PutReversedFourCC(malformedTrat, "_END");
  malformedTrat.push_back(4);
  const std::vector<uint8_t> malformedTratAsset = RemasteredForm("WPSM", "WPSM", std::move(malformedTrat));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(malformedTratAsset.data(), malformedTratAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error),
            "WPSM TRAT rejects a raw float instead of the typed real-element shape");

  std::vector<uint8_t> psclProperties;
  PutReversedFourCC(psclProperties, "PSCL");
  psclProperties.push_back(1);
  PutReversedFourCC(psclProperties, "MPCB");
  PutReversedFourCC(psclProperties, "CNST");
  for (int i = 0; i < 3; ++i) {
    PutReversedFourCC(psclProperties, "CNST");
    PutLe32(psclProperties, 0x40400000); // each component is 3.0f in the pinned asset
  }
  PutReversedFourCC(psclProperties, "_END");
  psclProperties.push_back(4);
  const std::vector<uint8_t> psclAsset = RemasteredForm("WPSM", "WPSM", std::move(psclProperties));
  error.clear();
  bool psclParsed = false;
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(psclAsset.data(), psclAsset.size(),
                                                      PortRemastered::EffectFourCC("WPSM"), node, error),
            "WPSM PSCL parses as a factory-typed vector element");
  for (const PortRemastered::EffectProperty& property : node.properties) {
    if (property.fourcc != PortRemastered::EffectFourCC("PSCL")) {
      continue;
    }
    const auto& value = property.value;
    if (property.tag != 1 || value.size() != 1 || value.front().kind != PortRemastered::EffectValue::Kind::Element ||
        value.front().fourcc != PortRemastered::EffectFourCC("MPCB") || value.front().args.size() != 1) {
      continue;
    }
    const auto& vector = value.front().args.front();
    psclParsed = vector.kind == PortRemastered::EffectValue::Kind::Element &&
                 vector.fourcc == PortRemastered::EffectFourCC("CNST") && vector.args.size() == 3;
    for (const PortRemastered::EffectValue& component : vector.args) {
      psclParsed = psclParsed && component.kind == PortRemastered::EffectValue::Kind::Element &&
                   component.fourcc == PortRemastered::EffectFourCC("CNST") && component.args.size() == 1 &&
                   component.args.front().kind == PortRemastered::EffectValue::Kind::Word &&
                   component.args.front().word == 0x40400000;
    }
  }
  CHECK_MSG(psclParsed, "WPSM PSCL retains all three exact vector CNST components");

  std::vector<uint8_t> malformedPscl;
  PutReversedFourCC(malformedPscl, "PSCL");
  malformedPscl.push_back(1);
  PutReversedFourCC(malformedPscl, "CNST");
  PutReversedFourCC(malformedPscl, "CNST");
  PutLe32(malformedPscl, 0x40400000); // one real component cannot satisfy a vector
  PutReversedFourCC(malformedPscl, "_END");
  malformedPscl.push_back(4);
  const std::vector<uint8_t> malformedPsclAsset = RemasteredForm("WPSM", "WPSM", std::move(malformedPscl));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(malformedPsclAsset.data(), malformedPsclAsset.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error),
            "WPSM PSCL rejects a real-shaped CNST instead of a three-component vector");

  std::vector<uint8_t> dshxProperties;
  constexpr std::array<uint8_t, 16> dshxGuid = {0x00, 0x00, 0x00, 0x10, 0x1a, 0x00, 0x20, 0xfd,
                                                 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x31};
  PutReversedGuidLink(dshxProperties, "DSHX", 4, dshxGuid);
  PutReversedFourCC(dshxProperties, "_END");
  dshxProperties.push_back(4);
  const std::vector<uint8_t> dshxAsset = RemasteredForm("CRSM", "CRSM", std::move(dshxProperties));
  error.clear();
  bool dshxParsed = false;
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(dshxAsset.data(), dshxAsset.size(),
                                                      PortRemastered::EffectFourCC("CRSM"), node, error),
            "CRSM DSHX parses as one sound-response GUID");
  for (const PortRemastered::EffectProperty& property : node.properties) {
    if (property.fourcc == PortRemastered::EffectFourCC("DSHX")) {
      dshxParsed = property.tag == 4 && property.value.size() == 1 &&
                   property.value.front().kind == PortRemastered::EffectValue::Kind::Guid &&
                   property.value.front().guid == dshxGuid;
    }
  }
  CHECK_MSG(dshxParsed, "CRSM DSHX preserves the exact sound-response GUID");

  std::vector<uint8_t> truncatedDshx;
  PutReversedFourCC(truncatedDshx, "DSHX");
  truncatedDshx.push_back(4);
  truncatedDshx.insert(truncatedDshx.end(), dshxGuid.begin(), dshxGuid.begin() + 8);
  PutReversedFourCC(truncatedDshx, "_END");
  truncatedDshx.push_back(4);
  const std::vector<uint8_t> truncatedDshxAsset = RemasteredForm("CRSM", "CRSM", std::move(truncatedDshx));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(truncatedDshxAsset.data(), truncatedDshxAsset.size(),
                                                       PortRemastered::EffectFourCC("CRSM"), node, error),
            "CRSM DSHX's fixed GUID schema rejects a truncated value");

  const std::vector<uint8_t> wrongDshxForm = RemasteredForm("WPSM", "WPSM", [&] {
    std::vector<uint8_t> properties;
    PutReversedGuidLink(properties, "DSHX", 4, kMissileDefaultGenpLink);
    PutReversedFourCC(properties, "_END");
    properties.push_back(4);
    return properties;
  }());
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(wrongDshxForm.data(), wrongDshxForm.size(),
                                                       PortRemastered::EffectFourCC("WPSM"), node, error) &&
                error.find("unknown role property DSHX") != std::string::npos,
            "CRSM-only DSHX schema does not make the field generally permissive");

  constexpr std::array<uint8_t, 16> ddclGuid = {0x9d, 0x85, 0xcd, 0xe4, 0x29, 0x49, 0x43, 0x45,
                                                 0x95, 0xfa, 0x19, 0x99, 0x6c, 0xe4, 0x98, 0x2e};
  std::vector<uint8_t> ddclProperties;
  PutReversedGuidLink(ddclProperties, "DDCL", 4, ddclGuid);
  PutReversedFourCC(ddclProperties, "_END");
  ddclProperties.push_back(4);
  const std::vector<uint8_t> ddclAsset = RemasteredForm("CRSM", "CRSM", std::move(ddclProperties));
  error.clear();
  bool ddclParsed = false;
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(ddclAsset.data(), ddclAsset.size(),
                                                      PortRemastered::EffectFourCC("CRSM"), node, error),
            "CRSM DDCL parses as a decal asset GUID");
  for (const PortRemastered::EffectProperty& property : node.properties) {
    if (property.fourcc == PortRemastered::EffectFourCC("DDCL")) {
      ddclParsed = property.tag == 4 && property.value.size() == 1 &&
                   property.value.front().kind == PortRemastered::EffectValue::Kind::Guid &&
                   property.value.front().guid == ddclGuid;
    }
  }
  CHECK_MSG(ddclParsed, "CRSM DDCL preserves the exact decal GUID");

  std::vector<uint8_t> truncatedDdcl;
  PutReversedFourCC(truncatedDdcl, "DDCL");
  truncatedDdcl.push_back(4);
  truncatedDdcl.insert(truncatedDdcl.end(), ddclGuid.begin(), ddclGuid.begin() + 8);
  PutReversedFourCC(truncatedDdcl, "_END");
  truncatedDdcl.push_back(4);
  const std::vector<uint8_t> truncatedDdclAsset = RemasteredForm("CRSM", "CRSM", std::move(truncatedDdcl));
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(truncatedDdclAsset.data(), truncatedDdclAsset.size(),
                                                       PortRemastered::EffectFourCC("CRSM"), node, error),
            "CRSM DDCL's fixed GUID schema rejects a truncated value");

  const std::vector<uint8_t> wrongDdclForm = RemasteredForm("WPSM", "WPSM", [&] {
    std::vector<uint8_t> properties;
    PutReversedGuidLink(properties, "DDCL", 4, ddclGuid);
    PutReversedFourCC(properties, "_END");
    properties.push_back(4);
    return properties;
  }());
  error.clear();
  CHECK_MSG(!PortRemastered::ParseRemasteredRoleAsset(wrongDdclForm.data(), wrongDdclForm.size(),
                                                        PortRemastered::EffectFourCC("WPSM"), node, error) &&
                error.find("unknown role property DDCL") != std::string::npos,
            "CRSM-only DDCL schema does not make the field generally permissive");
}

void TestPlasmaRoleSchemaExtensions() {
  using PortRemastered::EffectFourCC;
  using PortRemastered::EffectValue;

  PortRemastered::EffectNode node;
  std::string error;
  const auto parseRoot = [&](const char (&root)[5], std::vector<uint8_t> properties) {
    PutReversedFourCC(properties, "_END");
    properties.push_back(4);
    const std::vector<uint8_t> asset = RemasteredForm(root, root, std::move(properties));
    error.clear();
    return PortRemastered::ParseRemasteredRoleAsset(asset.data(), asset.size(), EffectFourCC(root), node, error);
  };
  const auto hasGuidProperty = [&](uint32_t fourcc, uint8_t tag, const std::array<uint8_t, 16>& expected) {
    for (const PortRemastered::EffectProperty& property : node.properties) {
      if (property.fourcc == fourcc && property.tag == tag && property.value.size() == 1 &&
          property.value.front().kind == EffectValue::Kind::Guid && property.value.front().guid == expected) {
        return true;
      }
    }
    return false;
  };

  const std::array<uint8_t, 16> nilGuid{};
  std::vector<uint8_t> aps2;
  PutReversedGuidLink(aps2, "APS2", 0, nilGuid);
  CHECK_MSG(parseRoot("WPSM", std::move(aps2)), "raw WPSM APS2 accepts its source-proven nil generator GUID");
  CHECK_MSG(hasGuidProperty(EffectFourCC("APS2"), 0, nilGuid), "WPSM APS2 exposes the exact nil GUID value");

  std::vector<uint8_t> truncatedAps2;
  PutReversedFourCC(truncatedAps2, "APS2");
  truncatedAps2.push_back(0);
  truncatedAps2.insert(truncatedAps2.end(), nilGuid.begin(), nilGuid.begin() + 8);
  CHECK_MSG(!parseRoot("WPSM", std::move(truncatedAps2)), "WPSM APS2 rejects a truncated fixed GUID");

  std::vector<uint8_t> wrongKindAps2;
  PutReversedGuidLink(wrongKindAps2, "APS2", 4, nilGuid);
  CHECK_MSG(!parseRoot("WPSM", std::move(wrongKindAps2)) && error.find("unexpected input-header tag") != std::string::npos,
            "WPSM APS2 rejects a valid GUID under the wrong source input-header kind");

  std::vector<uint8_t> aps2InCrsm;
  PutReversedGuidLink(aps2InCrsm, "APS2", 0, nilGuid);
  CHECK_MSG(!parseRoot("CRSM", std::move(aps2InCrsm)) && error.find("unknown role property APS2") != std::string::npos,
            "WPSM-only APS2 is not admitted by the CRSM role schema");

  std::vector<uint8_t> validInrt;
  PutReversedFourCC(validInrt, "INRT");
  validInrt.push_back(0);
  PutReversedEulerRotation(validInrt, 5);
  CHECK_MSG(parseRoot("WPSM", std::move(validInrt)),
            "raw WPSM INRT accepts exact REUL with three typed reals and order byte");
  bool inrtParsed = false;
  for (const PortRemastered::EffectProperty& property : node.properties) {
    if (property.fourcc != EffectFourCC("INRT") || property.tag != 0 || property.value.size() != 1) {
      continue;
    }
    const EffectValue& rotation = property.value.front();
    inrtParsed = rotation.kind == EffectValue::Kind::Element && rotation.fourcc == EffectFourCC("REUL") &&
                 rotation.size == 29 && rotation.args.size() == 4 &&
                 rotation.args[3].kind == EffectValue::Kind::Byte && rotation.args[3].word == 5;
    for (size_t i = 0; i < 3 && inrtParsed; ++i) {
      const EffectValue& real = rotation.args[i];
      inrtParsed = real.kind == EffectValue::Kind::Element && real.fourcc == EffectFourCC("CNST") &&
                   real.args.size() == 1 && real.args.front().kind == EffectValue::Kind::Word &&
                   real.args.front().word == 0;
    }
  }
  CHECK_MSG(inrtParsed, "WPSM INRT retains three typed real constants plus its exact order byte");

  std::vector<uint8_t> truncatedInrt;
  PutReversedFourCC(truncatedInrt, "INRT");
  truncatedInrt.push_back(0);
  PutReversedEulerRotation(truncatedInrt, 0, 2, false);
  CHECK_MSG(!parseRoot("WPSM", std::move(truncatedInrt)), "WPSM INRT rejects a truncated three-real REUL expression");

  std::vector<uint8_t> unknownRotation;
  PutReversedFourCC(unknownRotation, "INRT");
  unknownRotation.push_back(0);
  PutReversedFourCC(unknownRotation, "ZZZZ");
  PutReversedRealConstant(unknownRotation);
  PutReversedRealConstant(unknownRotation);
  PutReversedRealConstant(unknownRotation);
  unknownRotation.push_back(0);
  CHECK_MSG(!parseRoot("WPSM", std::move(unknownRotation)), "WPSM INRT rejects unproved rotation opcodes without fallback");

  std::vector<uint8_t> unknownReal;
  PutReversedFourCC(unknownReal, "INRT");
  unknownReal.push_back(0);
  PutReversedFourCC(unknownReal, "REUL");
  PutReversedFourCC(unknownReal, "ZZZZ");
  PutReversedRealConstant(unknownReal);
  PutReversedRealConstant(unknownReal);
  unknownReal.push_back(0);
  CHECK_MSG(!parseRoot("WPSM", std::move(unknownReal)), "WPSM INRT rejects an unproved typed-real opcode");

  std::vector<uint8_t> ambiguousReal;
  PutReversedFourCC(ambiguousReal, "INRT");
  ambiguousReal.push_back(0);
  PutReversedFourCC(ambiguousReal, "REUL");
  PutReversedFourCC(ambiguousReal, "MPRD"); // the general table has multiple arities; role parsing refuses to guess
  PutReversedRealConstant(ambiguousReal);
  PutReversedRealConstant(ambiguousReal);
  ambiguousReal.push_back(0);
  CHECK_MSG(!parseRoot("WPSM", std::move(ambiguousReal)),
            "WPSM INRT rejects a typed-real opcode with ambiguous arities rather than backtracking");

  std::vector<uint8_t> malformedReal;
  PutReversedFourCC(malformedReal, "INRT");
  malformedReal.push_back(0);
  PutReversedFourCC(malformedReal, "REUL");
  PutReversedFourCC(malformedReal, "CNST");
  malformedReal.insert(malformedReal.end(), 3, 0); // a real CNST requires four payload bytes
  PutReversedRealConstant(malformedReal);
  PutReversedRealConstant(malformedReal);
  malformedReal.push_back(0);
  CHECK_MSG(!parseRoot("WPSM", std::move(malformedReal)), "WPSM INRT rejects a malformed/truncated typed real");

  std::vector<uint8_t> invalidOrder;
  PutReversedFourCC(invalidOrder, "INRT");
  invalidOrder.push_back(0);
  PutReversedEulerRotation(invalidOrder, 6);
  CHECK_MSG(!parseRoot("WPSM", std::move(invalidOrder)), "WPSM INRT rejects an out-of-range Euler-order enum byte");

  constexpr std::array<uint8_t, 16> plasmaDecalGuid = {0x40, 0x3c, 0x04, 0x71, 0x0e, 0x58, 0x05, 0x4c,
                                                        0xb0, 0xb4, 0x4f, 0xe1, 0xf2, 0x0e, 0x11, 0x2d};
  const auto checkDecalField = [&](const char (&field)[5]) {
    std::vector<uint8_t> valid;
    PutReversedGuidLink(valid, field, 4, plasmaDecalGuid);
    CHECK_MSG(parseRoot("CRSM", std::move(valid)), "CRSM source decal field parses as its fixed decal GUID");
    CHECK_MSG(hasGuidProperty(EffectFourCC(field), 4, plasmaDecalGuid), "CRSM decal field retains its exact GUID");

    std::vector<uint8_t> truncated;
    PutReversedFourCC(truncated, field);
    truncated.push_back(4);
    truncated.insert(truncated.end(), plasmaDecalGuid.begin(), plasmaDecalGuid.begin() + 8);
    CHECK_MSG(!parseRoot("CRSM", std::move(truncated)), "CRSM source decal field rejects a truncated GUID");
  };
  checkDecalField("3MUD");
  checkDecalField("3LAV");
  checkDecalField("3SAN");

  std::vector<uint8_t> decalInWpsm;
  PutReversedGuidLink(decalInWpsm, "3MUD", 4, plasmaDecalGuid);
  CHECK_MSG(!parseRoot("WPSM", std::move(decalInWpsm)) && error.find("unknown role property 3MUD") != std::string::npos,
            "CRSM-only decal fields are not admitted by the WPSM role schema");

  std::vector<uint8_t> unknownCrsmField;
  PutReversedGuidLink(unknownCrsmField, "ZZZZ", 4, plasmaDecalGuid);
  CHECK_MSG(!parseRoot("CRSM", std::move(unknownCrsmField)) &&
                error.find("unknown role property ZZZZ") != std::string::npos,
            "CRSM still rejects unproved properties rather than accepting a GUID fallback");
}

void TestPlasmaSyntheticSourceShape() {
  const Fixture fixture = MakePlasmaBeamFixture();
  PortRemastered::EffectNode wpsm;
  PortRemastered::EffectNode crsm;
  std::string error;
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(fixture.remasteredWpsm.data(), fixture.remasteredWpsm.size(),
                                                      PortRemastered::EffectFourCC("WPSM"), wpsm, error),
            "complete synthetic Plasma WPSM with APS2, INRT, and COLR passes strict role parsing");
  error.clear();
  CHECK_MSG(PortRemastered::ParseRemasteredRoleAsset(fixture.remasteredCrsm.data(), fixture.remasteredCrsm.size(),
                                                      PortRemastered::EffectFourCC("CRSM"), crsm, error),
            "complete synthetic Plasma CRSM with DEFS and all three decals passes strict role parsing");
  PlasmaBeamDefaultRolePair pair;
  error.clear();
  CHECK_MSG(ResolvePlasmaBeamDefaultRole(PlasmaBeamInput(fixture), pair, &error) &&
                pair.genp == kPlasmaBeamDefaultGenp && pair.retailPart == kPlasmaBeamDefaultPart,
            "complete source-shaped Remastered and retail Plasma chain resolves to its pinned GENP/PART");
}

void TestIdentityAndTypedGates() {
  Fixture fixture = MakeFixture();
  MissileDefaultRoleInput input = Input(fixture);
  MissileDefaultRolePair pair;
  input.weaponName = "supermissile";
  CHECK_MSG(!ResolveMissileDefaultRole(input, pair), "other weapon name is rejected");
  input = Input(fixture);
  input.retailWpscId++;
  CHECK_MSG(!ResolveMissileDefaultRole(input, pair), "wrong WPSC identity is rejected");
  input = Input(fixture);
  input.retailWpscExists = false;
  CHECK_MSG(!ResolveMissileDefaultRole(input, pair), "missing typed WPSC is rejected");
  input = Input(fixture);
  input.retailCrscExists = false;
  CHECK_MSG(!ResolveMissileDefaultRole(input, pair), "missing typed CRSC is rejected");
  input = Input(fixture);
  input.retailPartExists = false;
  CHECK_MSG(!ResolveMissileDefaultRole(input, pair), "missing typed PART is rejected");
  input = Input(fixture);
  input.remasteredWpsmExists = false;
  CHECK_MSG(!ResolveMissileDefaultRole(input, pair), "missing unique WPSM is rejected");
  input = Input(fixture);
  input.remasteredCrsmExists = false;
  CHECK_MSG(!ResolveMissileDefaultRole(input, pair), "missing unique CRSM is rejected");
  input = Input(fixture);
  input.remasteredGenpExists = false;
  CHECK_MSG(!ResolveMissileDefaultRole(input, pair), "missing unique GENP is rejected");
}

void TestPowerBallExactChainAndConflicts() {
  const Fixture fixture = MakePowerBallFixture();
  PowerBallDefaultRolePair pair;
  CHECK_MSG(ResolvePowerBallDefaultRole(PowerBallInput(fixture), pair),
            "the exact USA PowerBall role chain resolves despite nested retail decoys");
  CHECK_MSG(pair.genp == kPowerBallDefaultGenp && pair.retailPart == kPowerBallDefaultPart,
            "PowerBall resolves only to the pinned full GENP GUID and retail PART id");
  uint32_t retailLink = 0;
  CHECK_MSG(detail::ReadUniqueRetailAssetId(fixture.retailWpsc.data(), fixture.retailWpsc.size(), "WPSM", "COLR",
                                        retailLink) && retailLink == kPowerBallCrsc,
        "PowerBall WPSC COLR is read as a top-level typed link");

  CHECK_MSG(!ConflictsWithPowerBallDefaultRolePair(pair, pair.genp, pair.retailPart),
            "an identical existing PowerBall pair is not a conflict");
  auto otherGenp = pair.genp;
  otherGenp[0] ^= 1;
  CHECK_MSG(ConflictsWithPowerBallDefaultRolePair(pair, otherGenp, pair.retailPart),
            "a different effect claiming the pinned retail PART is a conflict");
  CHECK_MSG(ConflictsWithPowerBallDefaultRolePair(pair, pair.genp, pair.retailPart + 1),
            "the pinned effect claiming another retail id is a conflict");
  CHECK_MSG(!ConflictsWithPowerBallDefaultRolePair(pair, otherGenp, pair.retailPart + 1),
            "an unrelated effect/id pair does not conflict");
}

void TestPowerBallWrongLinksAndMalformedResources() {
  ExpectPowerBallRejected(MakePowerBallFixture(kPowerBallCrsc + 1),
                          "a wrong retail WPSC COLR link is rejected");
  ExpectPowerBallRejected(MakePowerBallFixture(kPowerBallCrsc, kPowerBallDefaultPart + 1),
                          "a wrong retail CRSC DEFS link is rejected");
  ExpectPowerBallRejected(MakePowerBallFixture(kPowerBallCrsc, kPowerBallDefaultPart,
                                               [] { auto id = kPowerBallCrsmLink; id[0] ^= 1; return id; }()),
                          "a wrong complete Remastered CRSM GUID is rejected");
  ExpectPowerBallRejected(MakePowerBallFixture(kPowerBallCrsc, kPowerBallDefaultPart, kPowerBallCrsmLink,
                                               [] { auto id = kPowerBallDefaultGenpLink; id[0] ^= 1; return id; }()),
                          "a wrong complete Remastered GENP GUID is rejected");
  ExpectPowerBallRejected(MakePowerBallFixture(kPowerBallCrsc, kPowerBallDefaultPart, kPowerBallCrsmLink,
                                               kPowerBallDefaultGenpLink, false, false, true),
                          "duplicate retail and Remastered role fields are ambiguous and rejected");
  ExpectPowerBallRejected(MakePowerBallFixture(kPowerBallCrsc, kPowerBallDefaultPart, kPowerBallCrsmLink,
                                               kPowerBallDefaultGenpLink, false, false, false, false),
                          "absent role links in any member of the chain are rejected");

  Fixture fixture = MakePowerBallFixture();
  fixture.retailWpsc.pop_back();
  ExpectPowerBallRejected(fixture, "a malformed retail WPSC stream is rejected");
  fixture = MakePowerBallFixture();
  fixture.retailCrsc[0] = 'X';
  ExpectPowerBallRejected(fixture, "a resource with the wrong retail CRSC type is rejected");
  fixture = MakePowerBallFixture();
  fixture.remasteredWpsm[0x44] = 4; // COLR's allowed link kind is zero.
  ExpectPowerBallRejected(fixture, "a Remastered WPSM COLR with the wrong typed-link kind is rejected");
  fixture = MakePowerBallFixture();
  fixture.remasteredCrsm[0x44] = 0; // DEFS is a GENP link (kind four).
  ExpectPowerBallRejected(fixture, "a Remastered CRSM DEFS with the wrong typed-link kind is rejected");
  fixture = MakePowerBallFixture();
  SetLe64(fixture.remasteredCrsm, 4, UINT64_MAX);
  ExpectPowerBallRejected(fixture, "an overflowing Remastered form declaration is rejected");
}

void TestPowerBallIdentityAndTypedGates() {
  const Fixture fixture = MakePowerBallFixture();
  PowerBallDefaultRoleInput input = PowerBallInput(fixture);
  PowerBallDefaultRolePair pair;
  input.weaponName = "superpowerball";
  CHECK_MSG(!ResolvePowerBallDefaultRole(input, pair), "a different WPSM name is rejected");
  input = PowerBallInput(fixture);
  input.retailWpscId++;
  CHECK_MSG(!ResolvePowerBallDefaultRole(input, pair), "a different regional WPSC identity is rejected");
  input = PowerBallInput(fixture);
  input.retailWpscExists = false;
  CHECK_MSG(!ResolvePowerBallDefaultRole(input, pair), "a missing or ambiguous typed WPSC is rejected");
  input = PowerBallInput(fixture);
  input.retailCrscExists = false;
  CHECK_MSG(!ResolvePowerBallDefaultRole(input, pair), "a missing or ambiguous typed CRSC is rejected");
  input = PowerBallInput(fixture);
  input.retailPartExists = false;
  CHECK_MSG(!ResolvePowerBallDefaultRole(input, pair), "a missing or ambiguous typed PART is rejected");
  input = PowerBallInput(fixture);
  input.remasteredWpsmExists = false;
  CHECK_MSG(!ResolvePowerBallDefaultRole(input, pair), "a missing or ambiguous named WPSM is rejected");
  input = PowerBallInput(fixture);
  input.remasteredCrsmExists = false;
  CHECK_MSG(!ResolvePowerBallDefaultRole(input, pair), "a missing or ambiguous typed CRSM is rejected");
  input = PowerBallInput(fixture);
  input.remasteredGenpExists = false;
  CHECK_MSG(!ResolvePowerBallDefaultRole(input, pair), "a missing, ambiguous, or wrong-type GENP is rejected");
}

void TestPlasmaBeamExactChainAndConflicts() {
  const Fixture fixture = MakePlasmaBeamFixture(kPlasmaBeamCrsc, kPlasmaBeamDefaultPart,
                                                kPlasmaBeamCrsmLink, kPlasmaBeamDefaultGenpLink,
                                                true);
  PlasmaBeamDefaultRolePair pair;
  std::string roleError;
  const bool resolved = ResolvePlasmaBeamDefaultRole(PlasmaBeamInput(fixture), pair, &roleError);
  if (!resolved) {
    std::fprintf(stderr, "PlasmaBeam role resolver: %s\n", roleError.c_str());
  }
  CHECK_MSG(resolved,
            "the exact PlasmaBeam role chain resolves despite a nested retail decoy");
  CHECK_MSG(pair.genp == kPlasmaBeamDefaultGenp && pair.retailPart == kPlasmaBeamDefaultPart,
            "PlasmaBeam resolves only to the pinned full GENP GUID and retail PART id");
  uint32_t retailLink = 0;
  CHECK_MSG(detail::ReadUniqueRetailAssetId(fixture.retailWpsc.data(), fixture.retailWpsc.size(), "WPSM", "COLR",
                                        retailLink) && retailLink == kPlasmaBeamCrsc,
        "PlasmaBeam WPSC COLR is read as a top-level typed link");

  CHECK_MSG(!ConflictsWithPlasmaBeamDefaultRolePair(pair, pair.genp, pair.retailPart),
            "an identical existing PlasmaBeam pair is idempotent");
  auto otherGenp = pair.genp;
  otherGenp[0] ^= 1;
  CHECK_MSG(ConflictsWithPlasmaBeamDefaultRolePair(pair, otherGenp, pair.retailPart),
            "a different effect claiming the pinned PlasmaBeam retail PART is a conflict");
  CHECK_MSG(ConflictsWithPlasmaBeamDefaultRolePair(pair, pair.genp, pair.retailPart + 1),
            "the pinned PlasmaBeam effect claiming another retail id is a conflict");
  CHECK_MSG(!ConflictsWithPlasmaBeamDefaultRolePair(pair, otherGenp, pair.retailPart + 1),
            "an unrelated PlasmaBeam effect/id pair does not conflict");

  Fixture trailing = MakePlasmaBeamFixture();
  PutRetailLink(trailing.retailWpsc, "COLR", kPlasmaBeamCrsc); // after retail _END
  PutReversedGuidLink(trailing.remasteredWpsm, "COLR", 0, kPlasmaBeamCrsmLink); // after declared RFRM form
  PlasmaBeamDefaultRolePair trailingPair;
  CHECK_MSG(ResolvePlasmaBeamDefaultRole(PlasmaBeamInput(trailing), trailingPair),
            "bytes beyond retail _END and the declared RFRM form do not add duplicate role links");
}

void TestPlasmaBeamWrongLinksAndMalformedResources() {
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixture(kPlasmaBeamCrsc + 1),
                           "a wrong retail PlasmaBeam WPSC COLR link is rejected");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixture(kPlasmaBeamCrsc, kPlasmaBeamDefaultPart + 1),
                           "a wrong retail PlasmaBeam CRSC DEFS link is rejected");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixture(kPlasmaBeamCrsc, kPlasmaBeamDefaultPart,
                                                 [] { auto id = kPlasmaBeamCrsmLink; id[0] ^= 1; return id; }()),
                           "a wrong complete PlasmaBeam Remastered CRSM GUID is rejected");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixture(kPlasmaBeamCrsc, kPlasmaBeamDefaultPart,
                                                 kPlasmaBeamCrsmLink,
                                                 [] { auto id = kPlasmaBeamDefaultGenpLink; id[15] ^= 1; return id; }()),
                           "a wrong complete PlasmaBeam Remastered GENP GUID is rejected");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixture(kPlasmaBeamCrsc, kPlasmaBeamDefaultPart,
                                                 kPlasmaBeamCrsmLink, kPlasmaBeamDefaultGenpLink,
                                                 true, true),
                           "duplicate retail and Remastered PlasmaBeam role fields are rejected");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixture(kPlasmaBeamCrsc, kPlasmaBeamDefaultPart,
                                                 kPlasmaBeamCrsmLink, kPlasmaBeamDefaultGenpLink,
                                                 true, false, false),
                           "absent PlasmaBeam role fields in any chain member are rejected");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixture(kPlasmaBeamCrsc, kPlasmaBeamDefaultPart,
                                                 kPlasmaBeamCrsmLink, kPlasmaBeamDefaultGenpLink,
                                                 true, false, true, 4, 4),
                           "a WPSM COLR with the wrong typed-link kind is rejected");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixture(kPlasmaBeamCrsc, kPlasmaBeamDefaultPart,
                                                 kPlasmaBeamCrsmLink, kPlasmaBeamDefaultGenpLink,
                                                 true, false, true, 0, 0),
                           "a CRSM DEFS with the wrong typed-link kind is rejected");

  Fixture fixture = MakePlasmaBeamFixture();
  fixture.retailWpsc.pop_back();
  ExpectPlasmaBeamRejected(fixture, "a malformed PlasmaBeam retail WPSC stream is rejected");
  fixture = MakePlasmaBeamFixture();
  fixture.retailCrsc[0] = 'X';
  ExpectPlasmaBeamRejected(fixture, "a retail PlasmaBeam CRSC with the wrong form type is rejected");
  fixture = MakePlasmaBeamFixture();
  fixture.retailWpsc.insert(fixture.retailWpsc.end() - 4, {'B', 'A', 'D', '_'});
  ExpectPlasmaBeamRejected(fixture, "an unknown top-level retail PlasmaBeam WPSC field is rejected");
  fixture = MakePlasmaBeamFixture();
  fixture.remasteredWpsm.pop_back();
  ExpectPlasmaBeamRejected(fixture, "a truncated PlasmaBeam RFRM resource is rejected");
  fixture = MakePlasmaBeamFixture();
  fixture.remasteredWpsm[64] = 'X'; // COLR's reversed FourCC is the first top-level property.
  ExpectPlasmaBeamRejected(fixture, "an unknown top-level Remastered PlasmaBeam WPSM field is rejected");
  fixture = MakePlasmaBeamFixture();
  fixture.remasteredWpsm[23] = 'X'; // Corrupt only the outer RFRM form id.
  ExpectPlasmaBeamRejected(fixture, "a Remastered PlasmaBeam WPSM with the wrong outer form type is rejected");
  fixture = MakePlasmaBeamFixture();
  fixture.remasteredCrsm[23] = 'X'; // Corrupt only the outer RFRM form id.
  ExpectPlasmaBeamRejected(fixture, "a Remastered PlasmaBeam CRSM with the wrong outer form type is rejected");
  fixture = MakePlasmaBeamFixture();
  SetLe64(fixture.remasteredCrsm, 4, UINT64_MAX);
  ExpectPlasmaBeamRejected(fixture, "an overflowing PlasmaBeam CRSM form declaration is rejected");
}

void TestPlasmaBeamIndividualRoleFieldPresence() {
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixtureWithSingleRoleFieldChange(PlasmaRoleField::WpscColr, false),
                           "a missing WPSC COLR alone is rejected while the other three role links remain valid");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixtureWithSingleRoleFieldChange(PlasmaRoleField::CrscDefs, false),
                           "a missing CRSC DEFS alone is rejected while the other three role links remain valid");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixtureWithSingleRoleFieldChange(PlasmaRoleField::WpsmColr, false),
                           "a missing WPSM COLR alone is rejected while the other three role links remain valid");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixtureWithSingleRoleFieldChange(PlasmaRoleField::CrsmDefs, false),
                           "a missing CRSM DEFS alone is rejected while the other three role links remain valid");

  ExpectPlasmaBeamRejected(MakePlasmaBeamFixtureWithSingleRoleFieldChange(PlasmaRoleField::WpscColr, true),
                           "a duplicate WPSC COLR alone is rejected while the other three role links remain valid");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixtureWithSingleRoleFieldChange(PlasmaRoleField::CrscDefs, true),
                           "a duplicate CRSC DEFS alone is rejected while the other three role links remain valid");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixtureWithSingleRoleFieldChange(PlasmaRoleField::WpsmColr, true),
                           "a duplicate WPSM COLR alone is rejected while the other three role links remain valid");
  ExpectPlasmaBeamRejected(MakePlasmaBeamFixtureWithSingleRoleFieldChange(PlasmaRoleField::CrsmDefs, true),
                           "a duplicate CRSM DEFS alone is rejected while the other three role links remain valid");
}

// A WPSC the disc gives no ASW field gets one (the plasma shots' Remastered swooshes), a NONE field
// takes the id, and a WPSC that already links it, has no _END or reads back differently is refused.
void TestAddRetailSwooshField() {
  constexpr uint32_t kId = 0xFE123456;
  std::vector<uint8_t> missing;
  PutFourCC(missing, "WPSM");
  PutRetailLink(missing, "COLR", 0x11223344);
  PutRetailLink(missing, "APSM", 0x55667788);
  PutFourCC(missing, "_END");
  std::vector<uint8_t> out;
  std::string error;
  CHECK_MSG(AddRetailLink(missing, "ASW1", kId, out, &error), error.c_str());
  uint32_t id = 0;
  CHECK_MSG(PortRemastered::ReadRetailRoleAssetId(out.data(), out.size(), PortRemastered::EffectFourCC("WPSM"),
                                                  PortRemastered::EffectFourCC("ASW1"), id) &&
                id == kId,
            "the added ASW1 reads back");
  CHECK_MSG(PortRemastered::ReadRetailRoleAssetId(out.data(), out.size(), PortRemastered::EffectFourCC("WPSM"),
                                                  PortRemastered::EffectFourCC("COLR"), id) &&
                id == 0x11223344,
            "the other fields stay");
  CHECK_MSG(out.size() == missing.size() + 12, "a field is 12 bytes");
  CHECK_MSG(std::memcmp(out.data() + out.size() - 4, "_END", 4) == 0, "_END stays last");
  std::vector<uint8_t> twice;
  CHECK_MSG(!AddRetailLink(out, "ASW1", kId + 1, twice), "a field the WPSC links is not changed");
  CHECK_MSG(AddRetailLink(out, "ASW2", kId + 1, twice), "a second field is added");
  CHECK_MSG(PortRemastered::ReadRetailRoleAssetId(twice.data(), twice.size(), PortRemastered::EffectFourCC("WPSM"),
                                                  PortRemastered::EffectFourCC("ASW1"), id) &&
                id == kId,
            "the first added field stays");

  std::vector<uint8_t> none;
  PutFourCC(none, "WPSM");
  PutFourCC(none, "ASW1");
  PutFourCC(none, "NONE");
  PutRetailLink(none, "COLR", 0x11223344);
  PutFourCC(none, "_END");
  CHECK_MSG(AddRetailLink(none, "ASW1", kId, out, &error), error.c_str());
  CHECK_MSG(out.size() == none.size() + 4, "NONE becomes CNST and an id");
  CHECK_MSG(PortRemastered::ReadRetailRoleAssetId(out.data(), out.size(), PortRemastered::EffectFourCC("WPSM"),
                                                  PortRemastered::EffectFourCC("ASW1"), id) &&
                id == kId,
            "the NONE field's id reads back");

  std::vector<uint8_t> noEnd(missing.begin(), missing.end() - 4);
  CHECK_MSG(!AddRetailLink(noEnd, "ASW1", kId, out), "a WPSC without _END is refused");
  std::vector<uint8_t> wrongClass = missing;
  std::memcpy(wrongClass.data(), "CRSM", 4);
  CHECK_MSG(!AddRetailLink(wrongClass, "ASW1", kId, out), "another class is refused");
}

void TestPlasmaBeamIdentityAndTypedGates() {
  const Fixture fixture = MakePlasmaBeamFixture();
  PlasmaBeamDefaultRoleInput input = PlasmaBeamInput(fixture);
  PlasmaBeamDefaultRolePair pair;
  input.weaponName = "plasma-beam";
  CHECK_MSG(!ResolvePlasmaBeamDefaultRole(input, pair), "a different PlasmaBeam FrameKey is rejected");
  input = PlasmaBeamInput(fixture);
  input.retailWpscId++;
  CHECK_MSG(!ResolvePlasmaBeamDefaultRole(input, pair), "a different regional PlasmaBeam WPSC identity is rejected");
  input = PlasmaBeamInput(fixture);
  input.retailWpscExists = false;
  CHECK_MSG(!ResolvePlasmaBeamDefaultRole(input, pair), "a missing or ambiguous PlasmaBeam typed WPSC is rejected");
  input = PlasmaBeamInput(fixture);
  input.retailCrscExists = false;
  CHECK_MSG(!ResolvePlasmaBeamDefaultRole(input, pair), "a missing or ambiguous PlasmaBeam typed CRSC is rejected");
  input = PlasmaBeamInput(fixture);
  input.retailPartExists = false;
  CHECK_MSG(!ResolvePlasmaBeamDefaultRole(input, pair), "a missing or ambiguous PlasmaBeam typed PART is rejected");
  input = PlasmaBeamInput(fixture);
  input.remasteredWpsmExists = false;
  CHECK_MSG(!ResolvePlasmaBeamDefaultRole(input, pair), "a missing or ambiguous named PlasmaBeam WPSM is rejected");
  input = PlasmaBeamInput(fixture);
  input.remasteredCrsmExists = false;
  CHECK_MSG(!ResolvePlasmaBeamDefaultRole(input, pair), "a missing or ambiguous typed PlasmaBeam CRSM is rejected");
  input = PlasmaBeamInput(fixture);
  input.remasteredGenpExists = false;
  CHECK_MSG(!ResolvePlasmaBeamDefaultRole(input, pair), "a missing, ambiguous, or wrong-type PlasmaBeam GENP is rejected");
}
} // namespace

int main() {
  TestExactPairAndNestedDecoys();
  TestRetailSchemaAndBounds();
  TestRemasteredSchemaAndBounds();
  TestStrictRolePropertySchemas();
  TestStrictRoleAllowlistAndBudgets();
  TestStrictRoleKeyframeWidths();
  TestProvenRolePropertySchemas();
  TestPlasmaRoleSchemaExtensions();
  TestPlasmaSyntheticSourceShape();
  TestIdentityAndTypedGates();
  TestPowerBallExactChainAndConflicts();
  TestPowerBallWrongLinksAndMalformedResources();
  TestPowerBallIdentityAndTypedGates();
  TestPlasmaBeamExactChainAndConflicts();
  TestPlasmaBeamWrongLinksAndMalformedResources();
  TestPlasmaBeamIndividualRoleFieldPresence();
  TestPlasmaBeamIdentityAndTypedGates();
  TestAddRetailSwooshField();
  if (port_test::Failures() == 0) {
    std::puts("missile, PowerBall, and PlasmaBeam import pairing tests passed");
  }
  return PORT_TEST_RESULT();
}
