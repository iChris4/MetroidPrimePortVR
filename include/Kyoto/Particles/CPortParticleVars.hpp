#ifndef _CPORTPARTICLEVARS
#define _CPORTPARTICLEVARS

#ifdef TARGET_PC

#include "types.h"

#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/IElement.hpp"

#include <array>
#include <cstring>
#include <utility>
#include <vector>

// Port-only Remastered particle variables (PVAR). A converted generator or swoosh description
// carries the table (PVRT); each CElementGen/CParticleSwoosh holds the live values, which start at
// the defaults. VARF/VARI/VARV/VARC elements read the slot of the system being updated or rendered.

struct PortGuid {
  std::array< u8, 16 > bytes{};
  bool operator==(const PortGuid& o) const { return bytes == o.bytes; }
  bool operator!=(const PortGuid& o) const { return bytes != o.bytes; }
};

enum class EPortVarType : u32 { Real = 0, Int = 1, Color = 2, Vector = 3, Rotation = 4 };

// A bind handle: the variable's index in the description's table, or kPortNoVar.
constexpr u16 kPortNoVar = 0xffff;

struct SPortVar {
  PortGuid guid;
  EPortVarType type = EPortVarType::Real;
  float def[4] = {};
};

class CPortVarTable {
public:
  std::vector< SPortVar > vars;

  u16 Find(const PortGuid& guid, EPortVarType type) const {
    for (size_t i = 0; i < vars.size(); ++i) {
      if (vars[i].type == type && vars[i].guid == guid) {
        return static_cast< u16 >(i);
      }
    }
    return kPortNoVar;
  }
};

// A generator's system variables (CAddVarSystem; the port-only PSYS): SVRn/SVIn/SVCn/SVVn and
// the hidden init-only reals their KPIN/IRND become, in evaluation order. Slots below 10 are
// the variables PSAn and a child's SPA* read.
struct SPortSysItem {
  EPortVarType type = EPortVarType::Real;
  u32 slot = 0;
  bool initOnly = false;
  CRealElement* real = nullptr;
  CIntElement* intE = nullptr;
  CColorElement* color = nullptr;
  CVectorElement* vec = nullptr;
};

class CPortSys {
public:
  std::vector< SPortSysItem > items;
  u32 counts[4] = {};   // slots a type, hidden ones included
  u32 defined[4] = {};  // the variables 0-9 the description declares, a bit each

  CPortSys() = default;
  CPortSys(const CPortSys&) = delete;
  CPortSys& operator=(const CPortSys&) = delete;
  ~CPortSys() {
    for (size_t i = 0; i < items.size(); ++i) {
      delete items[i].real;
      delete items[i].intE;
      delete items[i].color;
      delete items[i].vec;
    }
  }
};

// The live values of one system. Without a table nothing is stored and every bind is a no-op.
class CPortVarMemory {
public:
  // The system variables start at zero.
  void InitSys(const CPortSys* sys) {
    x_sysDesc = sys;
    for (int t = 0; t < 4; ++t) {
      x_sys[t].assign(sys != nullptr ? sys->counts[t] : 0, std::array< float, 4 >{});
    }
  }
  void SetSys(EPortVarType type, u32 slot, const std::array< float, 4 >& v) {
    const u32 t = static_cast< u32 >(type);
    if (t < 4 && slot < x_sys[t].size()) {
      x_sys[t][slot] = v;
    }
  }
  // Variable n as a child's SPA* resolves it against this system's description, or null.
  const std::array< float, 4 >* SysVar(EPortVarType type, u32 n) const {
    const u32 t = static_cast< u32 >(type);
    if (x_sysDesc == nullptr || t >= 4 || n >= 10 || (x_sysDesc->defined[t] >> n & 1) == 0 ||
        n >= x_sys[t].size()) {
      return nullptr;
    }
    return &x_sys[t][n];
  }
  float SysReal(u32 slot) const { return slot < x_sys[0].size() ? x_sys[0][slot][0] : 0.f; }
  int SysInt(u32 slot) const {
    return slot < x_sys[1].size() ? static_cast< int >(x_sys[1][slot][0]) : 0;
  }
  // The system that built this one (CVarContext 0x24f06c: ctx+0x48 = the parent's), or null.
  void SetParent(const CPortVarMemory* parent) { x_parent = parent != this ? parent : nullptr; }
  const CPortVarMemory* Parent() const { return x_parent; }
  // PVAR value h, or null.
  const std::array< float, 4 >* Value(u16 h, EPortVarType type) const {
    return Valid(h, type) ? &x_values[h] : nullptr;
  }

  void Init(const CPortVarTable* table) {
    x_table = table;
    x_values.clear();
    if (table != nullptr) {
      x_values.resize(table->vars.size());
      for (size_t i = 0; i < table->vars.size(); ++i) {
        std::memcpy(x_values[i].data(), table->vars[i].def, sizeof(float) * 4);
      }
    }
  }
  bool Empty() const { return x_values.empty(); }
  u16 Find(const PortGuid& guid, EPortVarType type) const {
    return x_table != nullptr ? x_table->Find(guid, type) : kPortNoVar;
  }
  bool Valid(u16 h, EPortVarType type) const {
    return h < x_values.size() && x_table->vars[h].type == type;
  }

  void BindReal(u16 h, float v) {
    if (Valid(h, EPortVarType::Real)) {
      x_values[h][0] = v;
    }
  }
  void BindInt(u16 h, int v) {
    if (Valid(h, EPortVarType::Int)) {
      x_values[h][0] = static_cast< float >(v);
    }
  }
  void BindColor(u16 h, const CColor& c) {
    if (Valid(h, EPortVarType::Color)) {
      x_values[h] = {c.GetRed(), c.GetGreen(), c.GetBlue(), c.GetAlpha()};
    }
  }
  void BindVector(u16 h, const CVector3f& v) {
    if (Valid(h, EPortVarType::Vector)) {
      x_values[h] = {v.GetX(), v.GetY(), v.GetZ(), 0.f};
    }
  }

  // Index-based reads for the elements; out of range reads zero.
  float Real(u32 i) const { return i < x_values.size() ? x_values[i][0] : 0.f; }
  int Int(u32 i) const { return i < x_values.size() ? static_cast< int >(x_values[i][0]) : 0; }
  CColor Color(u32 i) const {
    if (i >= x_values.size()) {
      return CColor(0.f, 0.f, 0.f, 0.f);
    }
    const auto& v = x_values[i];
    return CColor(v[0], v[1], v[2], v[3]);
  }
  // The stored floats, unclamped (null out of range).
  const std::array< float, 4 >* ColorHdr(u32 i) const {
    return i < x_values.size() ? &x_values[i] : nullptr;
  }
  CVector3f Vector(u32 i) const {
    if (i >= x_values.size()) {
      return CVector3f::Zero();
    }
    const auto& v = x_values[i];
    return CVector3f(v[0], v[1], v[2]);
  }

private:
  const CPortVarTable* x_table = nullptr;
  std::vector< std::array< float, 4 > > x_values;
  const CPortSys* x_sysDesc = nullptr;
  std::vector< std::array< float, 4 > > x_sys[4];
  const CPortVarMemory* x_parent = nullptr;
};

// The memory of the system being updated/rendered (null outside one), so a child generator reads
// its own variables. Set with CPortVarScope, which restores the previous one.
struct CPortVarGlobals {
  static const CPortVarMemory* sCurrent;
};

class CPortVarScope {
  const CPortVarMemory* x_prev;

public:
  explicit CPortVarScope(const CPortVarMemory* mem) : x_prev(CPortVarGlobals::sCurrent) {
    CPortVarGlobals::sCurrent = mem;
  }
  ~CPortVarScope() { CPortVarGlobals::sCurrent = x_prev; }
  CPortVarScope(const CPortVarScope&) = delete;
  CPortVarScope& operator=(const CPortVarScope&) = delete;
};

// VARF / VARI / VARV / VARC: the current system's variable at a table index.
class CREPortVar : public CRealElement {
  u32 x4_index;

public:
  explicit CREPortVar(u32 index) : x4_index(index) {}
  ~CREPortVar() override {}
  bool GetValue(int, float& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->Real(x4_index) : 0.f;
    return false;
  }
};

class CIEPortVar : public CIntElement {
  u32 x4_index;

public:
  explicit CIEPortVar(u32 index) : x4_index(index) {}
  ~CIEPortVar() override {}
  bool GetValue(int, int& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->Int(x4_index) : 0;
    return false;
  }
};

class CVEPortVar : public CVectorElement {
  u32 x4_index;

public:
  explicit CVEPortVar(u32 index) : x4_index(index) {}
  ~CVEPortVar() override {}
  bool GetValue(int, CVector3f& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->Vector(x4_index)
                                               : CVector3f::Zero();
    return false;
  }
};

class CCEPortVar : public CColorElement {
  u32 x4_index;

public:
  explicit CCEPortVar(u32 index) : x4_index(index) {}
  ~CCEPortVar() override {}
  bool GetValue(int, CColor& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->Color(x4_index)
                                               : CColor(0.f, 0.f, 0.f, 0.f);
    return false;
  }
  bool GetValueHdr(int, float out[4]) const override {
    const std::array< float, 4 >* v = CPortVarGlobals::sCurrent != nullptr
                                          ? CPortVarGlobals::sCurrent->ColorHdr(x4_index)
                                          : nullptr;
    for (int i = 0; i < 4; ++i) {
      out[i] = v != nullptr ? (*v)[i] : 0.f;
    }
    return false;
  }
};

// PSYR (port-only; PSAn and the hidden KPIN/IRND reals): the current system's real slot.
class CREPortSysVar : public CRealElement {
  u32 x4_slot;

public:
  explicit CREPortSysVar(u32 slot) : x4_slot(slot) {}
  ~CREPortSysVar() override {}
  bool GetValue(int, float& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->SysReal(x4_slot) : 0.f;
    return false;
  }
};

// PSYI (port-only; a hidden init-only int, such as a PEOD IRND): the current system's int slot.
class CIEPortSysVar : public CIntElement {
  u32 x4_slot;

public:
  explicit CIEPortSysVar(u32 slot) : x4_slot(slot) {}
  ~CIEPortSysVar() override {}
  bool GetValue(int, int& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->SysInt(x4_slot) : 0;
    return false;
  }
};

// Where a parent read finds its value, or null for the fallback.
// SPA*: CElementParentAddlVar, the parent's system variable n, resolved against the parent's
// description (CREParentAdditionalValue::GetValue 0x296824). DPV*: the direct parent's PVAR
// variable (AddDirectParentExtVarRef 0x25584c). TPV*: the topmost parent's
// (AddTopmostParentExtVarRef 0x255bf4). [I] gen flag 0xa0 bit 7, which also gates the
// fallback, is not modelled; nor is a CParticleElectric parent (its children see the
// generator above it).
enum class EPortParentRead : u8 { System, Direct, Topmost };

struct SPortParentRead {
  EPortParentRead kind = EPortParentRead::System;
  u32 n = 0;
  PortGuid guid;

  const std::array< float, 4 >* Find(EPortVarType type) const {
    const CPortVarMemory* parent =
        CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->Parent() : nullptr;
    if (parent == nullptr) {
      return nullptr;
    }
    if (kind == EPortParentRead::System) {
      return parent->SysVar(type, n);
    }
    if (kind == EPortParentRead::Topmost) {
      while (parent->Parent() != nullptr) {
        parent = parent->Parent();
      }
    }
    return parent->Value(parent->Find(guid, type), type);
  }
};

class CREPortParentVar : public CRealElement {
  SPortParentRead x4_read;
  CRealElement* x_fallback;

public:
  CREPortParentVar(const SPortParentRead& read, CRealElement* fallback)
  : x4_read(read), x_fallback(fallback) {}
  ~CREPortParentVar() override { delete x_fallback; }
  bool GetValue(int frame, float& out) const override {
    if (const std::array< float, 4 >* v = x4_read.Find(EPortVarType::Real)) {
      out = (*v)[0];
      return false;
    }
    return x_fallback->GetValue(frame, out);
  }
};

class CIEPortParentVar : public CIntElement {
  SPortParentRead x4_read;
  CIntElement* x_fallback;

public:
  CIEPortParentVar(const SPortParentRead& read, CIntElement* fallback)
  : x4_read(read), x_fallback(fallback) {}
  ~CIEPortParentVar() override { delete x_fallback; }
  bool GetValue(int frame, int& out) const override {
    if (const std::array< float, 4 >* v = x4_read.Find(EPortVarType::Int)) {
      out = static_cast< int >((*v)[0]);
      return false;
    }
    return x_fallback->GetValue(frame, out);
  }
};

class CVEPortParentVar : public CVectorElement {
  SPortParentRead x4_read;
  CVectorElement* x_fallback;

public:
  CVEPortParentVar(const SPortParentRead& read, CVectorElement* fallback)
  : x4_read(read), x_fallback(fallback) {}
  ~CVEPortParentVar() override { delete x_fallback; }
  bool GetValue(int frame, CVector3f& out) const override {
    if (const std::array< float, 4 >* v = x4_read.Find(EPortVarType::Vector)) {
      out = CVector3f((*v)[0], (*v)[1], (*v)[2]);
      return false;
    }
    return x_fallback->GetValue(frame, out);
  }
};

class CCEPortParentVar : public CColorElement {
  SPortParentRead x4_read;
  CColorElement* x_fallback;

public:
  CCEPortParentVar(const SPortParentRead& read, CColorElement* fallback)
  : x4_read(read), x_fallback(fallback) {}
  ~CCEPortParentVar() override { delete x_fallback; }
  bool GetValue(int frame, CColor& out) const override {
    if (const std::array< float, 4 >* v = x4_read.Find(EPortVarType::Color)) {
      auto clamp = [](float f) { return f < 0.f ? 0.f : f > 1.f ? 1.f : f; };
      out = CColor(clamp((*v)[0]), clamp((*v)[1]), clamp((*v)[2]), clamp((*v)[3]));
      return false;
    }
    return x_fallback->GetValue(frame, out);
  }
  bool GetValueHdr(int frame, float out[4]) const override {
    if (const std::array< float, 4 >* v = x4_read.Find(EPortVarType::Color)) {
      for (int i = 0; i < 4; ++i) {
        out[i] = (*v)[i];
      }
      return false;
    }
    return x_fallback->GetValueHdr(frame, out);
  }
};

// Remastered colour combinators (GetColorElement 0x25ece4). Remastered keeps half floats with no
// clamp; CColor is bytes, so results are clamped to [0, 1].
// MULT: CCEMultiply 0x2c7704, a * b per channel (CColor4f::Modulate).
class CCEPortMultiply : public CColorElement {
  CColorElement* x4_a;
  CColorElement* x8_b;

public:
  CCEPortMultiply(CColorElement* a, CColorElement* b) : x4_a(a), x8_b(b) {}
  ~CCEPortMultiply() override {
    delete x4_a;
    delete x8_b;
  }
  bool GetValue(int frame, CColor& out) const override {
    CColor a, b;
    x4_a->GetValue(frame, a);
    x8_b->GetValue(frame, b);
    out = CColor(a.GetRed() * b.GetRed(), a.GetGreen() * b.GetGreen(), a.GetBlue() * b.GetBlue(),
                 a.GetAlpha() * b.GetAlpha());
    return false;
  }
  bool GetValueHdr(int frame, float out[4]) const override {
    float a[4], b[4];
    x4_a->GetValueHdr(frame, a);
    x8_b->GetValueHdr(frame, b);
    for (int i = 0; i < 4; ++i) {
      out[i] = a[i] * b[i];
    }
    return false;
  }
};

// MDAO: CCEModifyAlphaOnly 0x2c8678, the colour's RGB with alpha max(real, 0).
class CCEPortModifyAlpha : public CColorElement {
  CColorElement* x4_color;
  CRealElement* x8_alpha;

public:
  CCEPortModifyAlpha(CColorElement* color, CRealElement* alpha) : x4_color(color), x8_alpha(alpha) {}
  ~CCEPortModifyAlpha() override {
    delete x4_color;
    delete x8_alpha;
  }
  bool GetValue(int frame, CColor& out) const override {
    x4_color->GetValue(frame, out);
    float alpha = 1.f;
    if (x8_alpha != nullptr) {
      x8_alpha->GetValue(frame, alpha);
    }
    out.SetAlpha(alpha < 0.f ? 0.f : alpha > 1.f ? 1.f : alpha);
    return false;
  }
  bool GetValueHdr(int frame, float out[4]) const override {
    x4_color->GetValueHdr(frame, out);
    float alpha = 1.f;
    if (x8_alpha != nullptr) {
      x8_alpha->GetValue(frame, alpha);
    }
    out[3] = alpha < 0.f ? 0.f : alpha;
    return false;
  }
};

// SLCT: CElementArray<T>::GetValue (float 0x27121c, CVector3f 0x27186c, CColor4f16 0x271ee4),
// the entry at index % count (C remainder), the index evaluated on every read. IRND indices
// are latched per particle (CIEInitialRandom). Remastered does not wrap a negative index; the
// port does rather than read outside the array.
template < class Element >
class TPortSelect : public Element {
  CIntElement* x4_index;
  std::vector< Element* > x8_entries;

protected:
  const Element* Entry(int frame) const {
    int index = 0;
    x4_index->GetValue(frame, index);
    const int count = static_cast< int >(x8_entries.size());
    index %= count;
    return x8_entries[index < 0 ? index + count : index];
  }

public:
  TPortSelect(CIntElement* index, std::vector< Element* > entries)
  : x4_index(index), x8_entries(std::move(entries)) {}
  ~TPortSelect() override {
    delete x4_index;
    for (size_t i = 0; i < x8_entries.size(); ++i) {
      delete x8_entries[i];
    }
  }
};

class CREPortSelect : public TPortSelect< CRealElement > {
public:
  using TPortSelect::TPortSelect;
  bool GetValue(int frame, float& out) const override { return Entry(frame)->GetValue(frame, out); }
};

class CVEPortSelect : public TPortSelect< CVectorElement > {
public:
  using TPortSelect::TPortSelect;
  bool GetValue(int frame, CVector3f& out) const override { return Entry(frame)->GetValue(frame, out); }
};

class CCEPortSelect : public TPortSelect< CColorElement > {
public:
  using TPortSelect::TPortSelect;
  bool GetValue(int frame, CColor& out) const override { return Entry(frame)->GetValue(frame, out); }
  bool GetValueHdr(int frame, float out[4]) const override {
    return Entry(frame)->GetValueHdr(frame, out);
  }
};

class CInputStream;
// Reads a PVRT body (after the CNST class id): u32 count, then per variable 16 guid bytes, u32
// type and 4 float bit patterns. Null on a malformed table.
CPortVarTable* PortReadVarTable(CInputStream& in);

#endif // TARGET_PC
#endif // _CPORTPARTICLEVARS
