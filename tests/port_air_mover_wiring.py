"""Production wiring guards for AUDIT-3, alongside executable geometry tests.

Accepts another revision's mover source (platform/port_remastered_movers.cpp,
formerly inline in src/MetroidPrime/CGameCollision.cpp) so the guards can be
checked against 018ee83ce without rebuilding or modifying that revision's worktree.
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, between, partition, root_argument  # noqa: E402

source = root_argument().read_text()
air = between(source, "void CGameCollision::MoveAirborneStepped(", "#endif", "port_remastered_movers.cpp")


class Wiring(WiringCase):
    def test_f1_full_target_gate(self):
        self.assertIn("fullTarget = startPos + motion.GetTranslation()", air)
        self.assertIn("PortAirMoverContact::RemainingDisplacement(", air)
        self.assertIn("{startPos.GetX(),startPos.GetY(),startPos.GetZ()}", air)
        self.assertNotIn("{current.GetX(),current.GetY(),current.GetZ()}", air)
        self.assertNotIn("const CVector3f disp = actor.GetTranslation() - entryPos", air)

    def test_f2_no_native_terrain_or_time_override(self):
        self.assertNotIn("DetectStaticCollision_Cached(", air)
        self.assertNotIn("DetectCollisionBoolean_Cached(", air)
        self.assertNotIn("DetectCollision_Cached(", air)
        self.assertNotIn("if (!anyActing)", air)
        self.assertIn("PortAirMoverContact::HasImpacts(terrainContacts.size(), list.GetCount())", air)
        self.assertIn("QueryTerrain(query, tris.data(), tris.size(), produceImpacts)", air)

    def test_f3_dynamic_response_only_accepted(self):
        response = partition(air, "for (int i = 0; i < acceptedDynamic.GetCount(); ++i)")
        self.assertIsNotNone(response, "response must iterate accepted actors, not raw contacts")
        self.assertIn("acceptedDynamic.Add(info, false)", response[0])
        self.assertIn("const CCollisionInfo& info = acceptedDynamic[i]", response[1])
        self.assertNotIn("for (int i = staticCount; i < list.GetCount(); ++i)", air)

    def test_flag100_response_wiring(self):
        self.assertIn("PortAirMoverContact::SkipFlatFloor(", air)
        self.assertIn("PortAirMoverContact::PushDistance(flag100)", air)
        self.assertIn("tc.minVertical, mi, tc.flag100,", air)

    def test_split_integrator(self):
        # Pre 0xc59814 once before the first step, Post 0xc5994c once after the failsafe and
        # the tunnel check; the step prediction folds no force.
        pre = air.index("PortRemasteredGround::PreMomentum(")
        first_step = air.index("float remaining = dt - stepper.Step(dt);")
        failsafe = air.index("const bool resolved = failsafe.Collision();")
        tunnel = air.index("failsafe.Tunnel(entryPos, actor.GetTranslation(), tunnelOut)")
        post = air.index("PortRemasteredGround::PostMomentum(")
        clear = air.rindex("actor.ClearForcesAndTorques();")
        self.assertLess(pre, first_step)
        self.assertLess(failsafe, tunnel)
        self.assertLess(tunnel, post)
        self.assertLess(post, clear)
        self.assertEqual(air.count("PortRemasteredGround::PreMomentum("), 1)
        self.assertEqual(air.count("PortRemasteredGround::PostMomentum("), 1)
        self.assertIn("PortRemasteredGround::PredictTranslation(", air)
        # The only PredictMotion_Internal left is the angular base inside Stepper::Predict.
        self.assertEqual(air.count("PredictMotion_Internal("), 1)

    def test_platforms_through_the_producer(self):
        # Admitted platforms join the terrain query in every airborne query; the native
        # dynamic half only sees the remaining actors.
        self.assertIn("PortAdmitPlatform(*platform, simTick)", air)
        self.assertIn("const Test test = {mgr, cache, actor, materialFilter, nativeList, platforms};", air)
        self.assertIn("terrainContacts, true, &platforms);", air)
        self.assertIn("Ground::Check(mgr, cache, player, filter, nearList, platforms,", air)
        # The rider offset is the controller's own contacts' (0xf948d4 354-362).
        self.assertIn("PortAirMoverContact::RiderOffsetFor(tc.ownerUid, riderOwner, riderActive,", air)

    def test_landing_follows_mover_state(self):
        # Terrain events keep the retail convention: matLeft is the touched surface.
        self.assertIn("actor.GetMaterialList(), material, primary)", air)
        # 0xc545bc lands on the mover's grounded state; no second 0.85 IsFloor test.
        self.assertNotIn("SendScriptMessages(", air)
        landing = partition(air, "if (grounded) { for (int i = 0; i < groundList.GetCount(); ++i)")
        self.assertIsNotNone(landing, "grounded must land the player")
        self.assertIn("mgr.DeliverScriptMsg(&actor, kInvalidUniqueId, kSM_OnFloor)",
                      landing[1].split("kSM_Falling", 1)[0])

    def test_platform_rider_on_landing(self):
        # 0xcf1a2c: landing on the rider controller's platform adds the player as a
        # rider (0x1f to the platform) and records the platform (0x1f to the player).
        landing = partition(air, "if (grounded) { for (int i = 0; i < groundList.GetCount(); ++i)")
        self.assertIsNotNone(landing, "grounded must land the player")
        grounded = landing[1].split("kSM_Falling", 1)[0]
        self.assertIn("TCastToPtr< CScriptPlatform >(mgr.ObjectById(riderId))", grounded)
        self.assertIn("mgr.DeliverScriptMsg(platform, actor.GetUniqueId(), kSM_AddPlatformRider)",
                      grounded)
        self.assertIn("mgr.DeliverScriptMsg(&actor, platform->GetUniqueId(), kSM_AddPlatformRider)",
                      grounded)

    def test_ground_check_reads_dynamic_contact_extent(self):
        # A native box contact's GetPoint() is its clipped polygon's low corner, which
        # sits on the probe's open lower bound; the high corner must be read too.
        check = between(air, "struct Ground {", "struct Stepper {", "MoveAirborneStepped")
        self.assertIn("c.GetExtreme().GetZ()", check)

    def test_remastered_failsafe_replaces_last_free_state(self):
        # 0xf94750 DoCollisionFailsafe 0xc1fa68 behind the gate, then DoTunnelingFailsafe
        # 0xc20214 on every tick; Remastered keeps no last non-colliding state.
        self.assertNotIn("AirMoverIsFree", source)
        self.assertNotIn("SetLastNonCollidingState", air)
        self.assertNotIn("CollisionFailsafe(mgr, cache, actor", air)
        tail = partition(air, "if (!(fabsf(remaining) < 1e-5f && !stalledFlag)) {")
        self.assertIsNotNone(tail, "failsafe gate must stay")
        self.assertIn("const bool resolved = failsafe.Collision();", tail[1])
        tunnel = partition(tail[1], "failsafe.Tunnel(entryPos, actor.GetTranslation(), tunnelOut)")
        self.assertIsNotNone(tunnel, "tunneling must run after the gate")
        self.assertIn("actor.ClearForcesAndTorques();", tunnel[1])
        self.assertIn("PortCollisionFailsafe::DoCollisionFailsafe(", air)
        self.assertIn("PortCollisionFailsafe::DoTunnelingFailsafe(", air)


if __name__ == "__main__":
    unittest.main()
