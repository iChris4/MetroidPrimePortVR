#include "port_remastered_ground_world.h"

#include "port_remastered_collision_material.h"

#include <limits>

namespace PortRemasteredGroundWorld {
namespace {

// The adapter's former fixed capacity. Kept only as an observability signal: the
// growth path past it must stay visible, never become a failure.
constexpr std::size_t kLegacyCapacity = 64;

using PortRemasteredGroundContacts::AABB;
// The contacts module names its own output `Result`; qualify it everywhere so
// this module's `Result` is never ambiguous.
using GeometryResult = PortRemasteredGroundContacts::Result;

// port -> Remastered canonical, and its own inverse: the change is symmetric.
// See platform/port_remastered_room_internal.h (kR2G and MulR2G).
inline float RemFromPortX(float x) { return -x; }

inline Vec3 ToRemastered(const Vec3f& v) { return {RemFromPortX(v.x), v.z, v.y}; }
inline Vec3f FromRemastered(const Vec3& v) { return {-v.x, v.z, v.y}; }

inline AABB BoxToRemastered(const Aabb& box) {
  // X is negated, so the transformed corners swap their X extremes.
  return AABB{{RemFromPortX(box.max.x), box.min.z, box.min.y},
              {RemFromPortX(box.min.x), box.max.z, box.max.y}};
}

// Vertical extremes of a contact's inclusive point range.
void VerticalRange(const GeometryResult& r, std::size_t first, std::size_t last, float& minOut,
                   float& maxOut) {
  float lo = std::numeric_limits< float >::max();
  float hi = -std::numeric_limits< float >::max();
  for (std::size_t i = first; i <= last && i < r.pointCount; ++i) {
    const float v = r.points[i].y; // Remastered vertical
    if (v < lo) {
      lo = v;
    }
    if (v > hi) {
      hi = v;
    }
  }
  if (lo > hi) { // an empty range cannot occur, but never invent a number
    lo = hi = 0.f;
  }
  minOut = lo;
  maxOut = hi;
}

Contact FromGeometry(const PortRemasteredGroundContacts::Contact& c, bool self,
                     std::uint64_t materialBits, float minVertical, float maxVertical,
                     std::size_t triangle) {
  Contact out;
  out.self = self;
  out.orientation = Orientation(materialBits);
  out.materialBits = materialBits;
  const Vec3f n = FromRemastered(c.primary);
  out.normalX = n.x;
  out.normalY = n.y;
  out.normalZ = n.z;
  out.flag100 = Flag100(materialBits);
  out.hasSecondary = c.hasSecondary;
  if (c.hasSecondary) {
    const Vec3f s = FromRemastered(c.secondary);
    out.secondaryX = s.x;
    out.secondaryY = s.y;
    out.secondaryZ = s.z;
  }
  out.minVertical = minVertical;
  out.maxVertical = maxVertical;
  out.D = c.D;
  out.H = c.H;
  out.triangle = triangle;
  return out;
}

} // namespace

int Orientation(std::uint64_t materialBits) {
  // TranslatePositiveMP1Material, 0xc59c20 (orientation stores 0xc59e04-0xc59e1c).
  return PortRemasteredCollisionMaterial::TranslatePositive(materialBits).orientation;
}

bool Flag100(std::uint64_t materialBits) {
  // Actor behavior bit 0x100 (NoPlayerCollision, or Character && !SolidCharacter,
  // 0xc53ccc): the only behavior values carrying 0x100 are 0x110.
  return (PortRemasteredCollisionMaterial::TranslateActor(materialBits).behavior & 0x100u) != 0;
}

bool BuildTriangleContacts(const Query& query, const Triangle& tri,
                           std::array< Contact, 2 >& contacts, std::size_t& count) {
  count = 0;
  const AABB box = BoxToRemastered(query.box);
  const std::array< Vec3, 3 > verts = {ToRemastered(tri.verts[0]), ToRemastered(tri.verts[1]),
                                       ToRemastered(tri.verts[2])};
  const Vec3 n = ToRemastered(tri.normal);
  const GeometryResult r = PortRemasteredGroundContacts::Build(box, verts, n);
  if (!r.hit) {
    return false;
  }

  // The box's own points are the single last intersection; the triangle's are
  // the clipped polygon (0xc0b7fc-0xc0b888).
  float boxLo = 0.f, boxHi = 0.f, triLo = 0.f, triHi = 0.f;
  VerticalRange(r, r.box.firstPoint, r.box.lastPoint, boxLo, boxHi);
  VerticalRange(r, r.triangle.firstPoint, r.triangle.lastPoint, triLo, triHi);

  // The box's material comes from the box object (the actor); the triangle's is
  // the triangle surface merged with the tree object's, which the caller has
  // already merged into `materialBits`.
  contacts[count++] = FromGeometry(r.box, true, query.actorMaterialBits, boxLo, boxHi, 0);
  contacts[count++] = FromGeometry(r.triangle, false, tri.materialBits, triLo, triHi, 0);
  contacts[count - 1].ownerUid = tri.ownerUid;
  for (std::size_t j = 0; j < count; ++j) {
    const auto& source = j == 0 ? r.box : r.triangle;
    for (std::size_t p = source.firstPoint; p <= source.lastPoint && p < r.pointCount; ++p) {
      contacts[j].points[contacts[j].pointCount] = FromRemastered(r.points[p]);
      contacts[j].pointVertical[contacts[j].pointCount++] = r.points[p].y;
    }
  }
  return true;
}

Result BuildContacts(const Query& query, const Triangle* tris, std::size_t count) {
  Result out;
  std::array< Contact, 2 > pair{};
  std::size_t pairCount = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (!BuildTriangleContacts(query, tris[i], pair, pairCount)) {
      continue;
    }
    ++out.hits;
    for (std::size_t j = 0; j < pairCount; ++j) {
      Contact c = pair[j];
      c.triangle = i;
      out.contacts.push_back(c);
    }
    // The shipped list holds the first pair inline and grows on the heap without a
    // failure path; past the adapter's former fixed capacity this is recorded only.
    if (out.contacts.size() > kLegacyCapacity) {
      out.grewBeyondInlineCapacity = true;
    }
  }
  return out;
}

void ConsumerContacts(const Result& result, std::vector< const Contact* >& out) {
  for (std::size_t i = 0; i < result.contacts.size(); ++i) {
    if (!result.contacts[i].self) {
      out.push_back(&result.contacts[i]);
    }
  }
}

TerrainResult QueryTerrain(const Query& query, const Triangle* tris, std::size_t count,
                           bool produceImpacts) {
  TerrainResult out;
  const AABB box = BoxToRemastered(query.box);
  for (std::size_t i = 0; i < count; ++i) {
    const Triangle& tri = tris[i];
    const std::array<Vec3, 3> verts = {ToRemastered(tri.verts[0]),
                                      ToRemastered(tri.verts[1]), ToRemastered(tri.verts[2])};
    if (!PortRemasteredGroundContacts::Intersects(box, verts)) continue;
    out.booleanHit = true;
    if (!produceImpacts) return out;
    std::array<Contact, 2> pair{};
    std::size_t pairCount = 0;
    if (!BuildTriangleContacts(query, tri, pair, pairCount)) continue;
    ++out.impacts.hits;
    for (std::size_t j = 0; j < pairCount; ++j) {
      pair[j].triangle = i;
      out.impacts.contacts.push_back(pair[j]);
    }
  }
  out.impacts.grewBeyondInlineCapacity = out.impacts.contacts.size() > kLegacyCapacity;
  return out;
}

} // namespace PortRemasteredGroundWorld
