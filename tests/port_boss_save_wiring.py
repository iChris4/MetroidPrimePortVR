"""Production wiring guards for save states taken in a boss fight.

A state load rebuilds the room from disc and then applies the saved room state.
Three things the room script did once are not in that data, and a fight saved
mid-way softlocked or crashed without them:

  * Activate/Deactivate is delivered, not just flagged: Flaahgra builds its body
    controller in its kSM_Activate handler (a bare SetActive crashed in Think).
  * The enemies a script generator made (Hive Totem's wasps) are made again from
    their template; the counters waiting for their deaths waited forever.
  * The command messages the room script's connections sent an enemy are
    journalled and replayed (Thardus waits for the trigger's Action, and the
    trigger has already deactivated itself).

The pure journal encode/decode is checked in tests/port_savestate.cpp; this checks
that CStateManager feeds the journal, that the restore uses it in the right order
and that regenerated copies go through GenerateObject with the generating flag.

It is a source check: no engine code runs here.
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, between, braced, root_argument  # noqa: E402

root = root_argument()
MGR = root / "src/MetroidPrime/CStateManager.cpp"
ROOM = root / "platform/port_savestate_room.cpp"


class Wiring(WiringCase):
    def test_state_manager_hooks(self):
        mgr = MGR.read_text()
        # Every connection delivery is seen, before it is delivered.
        send = braced(mgr, "void CStateManager::SendScriptMsg(TUniqueId uid, TEditorId target,", str(MGR))
        self.assertIn("PortSaveState::NoteScriptMsg(*this, ent->GetUniqueId().value, uid.value, msg);",
                      send)
        self.assertLess(send.index("NoteScriptMsg"), send.index("DeliverScriptMsg(ent, uid, msg);"))
        # Only a generator's copies are marked (the flag CScriptGenerator sets).
        gen = braced(mgr, "rstl::pair< TEditorId, TUniqueId > CStateManager::GenerateObject(", str(MGR))
        self.assertIn("if (xf94_26_generatingObject && made.second != kInvalidUniqueId)", gen)
        self.assertIn("PortSaveState::NoteGenerated(*this, made.second.value);", gen)
        # A removed object's notes go with it (the destructor removes every object too).
        remove = braced(mgr, "void CStateManager::RemoveObject(TUniqueId id) {", str(MGR))
        self.assertIn("PortSaveState::NoteRemoved(*this, id.value);", remove)

    def test_activation_is_delivered(self):
        room = ROOM.read_text()
        body = braced(room, "void RestoreActive(CStateManager& mgr, CEntity& ent, bool active) {", str(ROOM))
        self.assertIn("DeliverUnlinked(mgr, ent, kInvalidUniqueId, active ? kSM_Activate : kSM_Deactivate);",
                      body)
        # A cinematic camera only gets the flag: its handler starts the cinematic.
        self.assertIn("dynamic_cast< CCinematicCamera* >(&ent) != nullptr", body)
        unlinked = braced(room, "void DeliverUnlinked(", str(ROOM))
        self.assertIn("ent.ConnectionList().clear();", unlinked)
        self.assertIn("ent.ConnectionList() = conns;", unlinked)

    def test_replay_before_saved_state(self):
        room = ROOM.read_text()
        body = braced(room, "void ApplyObject(CStateManager& mgr, CEntity& ent, const RoomObject& obj,", str(ROOM))
        active = body.index("RestoreActive(mgr, ent, obj.active);")
        replay = body.index("stats.replayed += ReplayScriptMsgs(mgr, ent, obj, !generic);")
        health = body.index("FindBlock(obj, kTagHealth)")
        self.assertLess(active, replay)
        self.assertLess(replay, health)
        # An enemy its fields block covers skips the replay and the per-boss restore.
        self.assertLess(body.index("EntityFieldsCover(ent, fields->data)"), replay)
        self.assertIn("if (!generic) {", body)
        # The replayed journal is kept for the next save.
        replayer = braced(room, "int ReplayScriptMsgs(", str(ROOM))
        self.assertIn("NoteFor(mgr, uid).msgs = journal;", replayer)
        self.assertIn("DeliverUnlinked(mgr, ent, sender,", replayer)

    def test_boss_fight_state(self):
        room = ROOM.read_text()
        save = braced(room, "void CaptureObject(CStateManager& mgr, CEntity& ent, RoomObject& obj) {", str(ROOM))
        self.assertIn("if (CaptureBossState(mgr, ent, boss))", save)
        self.assertIn("obj.blocks.push_back({kTagBoss, boss});", save)
        body = braced(room, "void ApplyObject(CStateManager& mgr, CEntity& ent, const RoomObject& obj,", str(ROOM))
        # After the saved health and pose, which a hook reads when the save has no block.
        restore = body.index("RestoreBossState(mgr, ent, boss != nullptr ? &boss->data : nullptr);")
        self.assertLess(body.index("FindBlock(obj, kTagHealth)"), restore)
        self.assertLess(body.index("FindBlock(obj, kTagTransform)"), restore)

    def test_generated_copies(self):
        room = ROOM.read_text()
        capture = between(room, "void CaptureRoomState(", "\nint ApplyRoomState(", str(ROOM))
        # A copy never decides whether its template's editor id is present.
        self.assertIn("if (!generated)\n      out.present.push_back(id);", capture)
        # Enemy, pickup and debris copies are saved; other copies (maze walls, sounds) are not.
        self.assertIn("if (generated && !SavesGeneratedCopy(ent))", capture)
        keeps = braced(room, "bool SavesGeneratedCopy(CEntity& ent) {", str(ROOM))
        self.assertIn("CPatterned", keeps)
        self.assertNotIn("CScriptSound", keeps)
        apply = braced(room, "int ApplyRoomState(", str(ROOM))
        self.assertIn("pickup->SetWasGenerated();", apply)
        self.assertIn("if (FindBlock(obj, kTagGenerated) != nullptr) {", apply)
        self.assertIn("mgr.SetIsGeneratingObject(true);", apply)
        self.assertIn("mgr.GenerateObject(TEditorId(obj->editorId)).second;", apply)
        self.assertIn("mgr.SetIsGeneratingObject(wasGenerating);", apply)
        # Copies are made after every template record was applied.
        self.assertLess(apply.index("ApplyObject(mgr, *ent, obj, stats);"),
                        apply.index("mgr.GenerateObject("))
        save = braced(room, "void CaptureObject(CStateManager& mgr, CEntity& ent, RoomObject& obj) {", str(ROOM))
        self.assertIn("obj.blocks.push_back({kTagGenerated, std::string()});", save)
        self.assertIn("obj.blocks.push_back({kTagMessages, EncodeScriptMsgs(n->msgs)});", save)


if __name__ == "__main__":
    unittest.main(verbosity=2)
