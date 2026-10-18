"""Production wiring guards for the map side file (port_map_side.h).

The seen pickups and the room marks of a save live in the card slot's own
"PKSN"/"RMMK" trailers where they fit, and in a small side file where they do
not: a USA slot with an Archipelago trailer holds room for PKSN alone, and the
"RMMK" trailer carries 20 marks of the 64 the save's list keeps. The file is
keyed on the card's identity (its resolved directory, the game id and maker its
files are created with, and the card port) and holds a fingerprint of the exact
slot bytes, so it is only used for the save, on the card, it was written for.

What this checks, none of which any unit test can see:

  * the four call sites in CMemoryCardDriver.cpp - a slot's buffer being rebuilt
    from the live game (Stage), a finished card write (Flush), a slot being read
    back and loaded (Apply) and a slot being erased (StageErase);
  * Flush is called from the success path alone: never from a write that failed,
    which would leave the side file ahead of the card;
  * the identity comes from the card the driver has mounted - the resolved
    channel path, the CARDInit game id and maker, the channel - and never from
    the emulator's folder or the card itself;
  * the file layer replaces a file in one step (POSIX rename, MoveFileExW on
    Windows) after a close that worked, and never removes the old file first;
  * the trailer keeps the fields the first build wrote (5-bit count, cap 20) so
    an older build reads what this one writes, and the side file carries 64;
  * the save state carries the same data in the blob's tail, so a state round
    trip does not shorten the list.

It is a source check: no engine code runs and no card is touched.
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, between, braced, count, index, root_argument  # noqa: E402

root = root_argument()
DRIVER = root / "src/MetroidPrime/CMemoryCardDriver.cpp"
PATHS = root / "platform/port_map_side_paths.cpp"
SIDE = root / "platform/port_map_side.cpp"
HEADER = root / "platform/include/port_map_side.h"
MARKS = root / "platform/include/port_map_marks.h"
SAVESTATE = root / "platform/port_savestate.cpp"
CARD_SYS = root / "src/Kyoto/DolphinCMemoryCardSys.cpp"
CMAKE = root / "CMakeLists.txt"


class Wiring(WiringCase):
    def test_slot_stages_on_rebuild_and_applies_on_load(self):
        driver = DRIVER.read_text()
        stage = between(driver, "void SGameFileSlot::InitializeFromGameState() {",
                        "\nvoid ", DRIVER)
        self.assertIn("PortMapSide::Stage(", stage)
        # The fingerprint must be over the buffer SGameFileSlot::PutTo writes,
        # not a copy of it: the read side hashes what came back from the card.
        self.assertIn("x0_saveBuffer.data(), x0_saveBuffer.size(), data", stage)
        apply = between(driver, "void SGameFileSlot::LoadGameState(int idx) {",
                        "\nconst CGameState::GameFileStateInfo*", DRIVER)
        self.assertIn("gpMain->StreamNewGameState(r, idx);", apply)
        self.assertIn("PortMapSide::Apply(idx, x0_saveBuffer.data(), x0_saveBuffer.size()", apply)

    def test_erase_and_flush_are_the_side_of_the_card_write(self):
        driver = DRIVER.read_text()
        erase = between(driver, "void CMemoryCardDriver::EraseFileSlot(int saveIdx) {", "\nvoid ",
                        DRIVER)
        self.assertIn("xe4_fileSlots[saveIdx] = nullptr;", erase)
        self.assertIn("PortMapSide::StageErase(saveIdx);", erase)
        write = between(driver, "void CMemoryCardDriver::WriteBackupBuf() {", "\nvoid ", DRIVER)
        self.assertIn("PortMapSide::Flush(", write)
        # Every slot the file just written holds, so a save that was built and
        # then abandoned leaves nothing behind.
        self.assertIn("x0_saveBuffer.data()", write)
        # Flush is only reached once the write finished: WriteBackupBuf is
        # called from the two completion paths, and from no failure handler.
        # (The driver's own method; gpGameState->WriteBackupBuf() is another one.)
        own_calls = count(driver, "WriteBackupBuf();") - count(driver, "->WriteBackupBuf();")
        self.assertEqual(own_calls, 2, "one call per completion path")
        for failed in ("UpdateFileWrite(ECardResult result)",
                       "UpdateFileWriteTransactional(ECardResult result)"):
            body = between(driver, failed + " {", "\nvoid ", DRIVER)
            self.assertNotIn("WriteBackupBuf", body, failed)
            self.assertIn("Failed", body, failed)
        self.assertEqual(count(driver, "PortMapSide::Flush("), 1)

    def test_staging_is_scoped_to_the_card_it_was_taken_on(self):
        side = SIDE.read_text()
        staged = between(side, "struct Staged {", "};", SIDE)
        # The card, and the very file to write, are taken at Stage: a card that
        # changes in between cannot redirect the write.
        self.assertIn("std::string identity;", staged)
        self.assertIn("fs::path path;", staged)
        stage = between(side, "void Stage(int slot,", "\n}", SIDE)
        self.assertIn("StagedFor(slot, false)", stage)
        erase = braced(side, "void StageErase(int slot)", SIDE)
        self.assertIn("StagedFor(slot, true)", erase)
        flush = between(side, "int Flush(const SlotBytes* slots, int slotCount) {", "\n} // namespace",
                        SIDE)
        # Another card's staging is dropped, not applied where it landed.
        self.assertIn("entry.second.identity != identity", flush)
        self.assertIn("continue;", flush)
        # An erase only removes the file once the written file has no such slot:
        # a failed or cancelled erase keeps the map data of the slot that is
        # still there.
        erase_branch = between(flush, "if (entry.second.erase) {", "continue;\n    }", SIDE)
        self.assertIn("if (inFile) {", erase_branch)
        self.assertIn("if (inFile) {\n        continue;\n      }", erase_branch)
        self.assertLess(index(erase_branch, "if (inFile) {"),
                        index(erase_branch, "RemoveFile("), "the absence is checked first")
        # A slot the written file does not hold is dropped, and the bytes have
        # to be the staged ones.
        self.assertIn("if (!inFile) {", flush)
        self.assertIn("Fingerprint(slots[slot].bytes, slots[slot].size) != entry.second.fingerprint",
                      flush)
        self.assertIn("sStaged.clear();", flush)

    def test_the_file_layer_replaces_in_one_step(self):
        side = SIDE.read_text()
        # POSIX rename replaces; Windows needs MoveFileExW, which replaces an
        # existing file in one step - removing the old file first would leave a
        # moment with neither.
        replace = between(side, "bool ReplaceWithTemporary(", "\n}", SIDE)
        self.assertIn("std::rename(", replace)
        self.assertIn("#ifdef _WIN32", replace)
        self.assertIn("MoveFileExW(", replace)
        self.assertIn("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH", replace)
        self.assertIn("#include <windows.h>", side)
        write = between(side, "bool WriteFile(", "\n}", SIDE)
        self.assertNotIn("fs::remove(path", write)
        # The close is checked before the replacement, not only the flush.
        self.assertIn("out.close();", write)
        self.assertLess(index(write, "out.flush();"), index(write, "out.close();"))
        self.assertLess(index(write, "out.close();"), index(write, "ReplaceWithTemporary(temp, path)"))
        self.assertIn("if (!out)", write)
        self.assertIn("ReplaceWithTemporary(temp, path)", write)
        # Nothing of ours is written into the card's own folder.
        self.assertIn('userFolder + "mapdata"', side)

    def test_the_identity_comes_from_the_card_in_use(self):
        paths = PATHS.read_text()
        self.assertIn("PortPaths::UserFolder()", paths)
        self.assertIn("CMemoryCardSys::PortCardResolvedDirectory()", paths)
        self.assertIn("CMemoryCardSys::PortCardIdMaker()", paths)
        self.assertIn("CMemoryCardSys::PortCardChannel()", paths)
        self.assertIn("CardIdentity(", paths)
        self.assertIn("idMaker.empty()", paths)
        self.assertNotIn("CardTargetDirectory", paths)
        self.assertNotIn("CardFolder", paths)
        # The identity is the name's only input, and the directory in it is
        # normalised so two spellings of one directory agree.
        header = HEADER.read_text()
        self.assertIn("weakly_canonical", header)
        self.assertIn("identity += idMaker;", header)
        self.assertIn("identity += std::to_string(cardPort);", header)
        self.assertIn("uint64_t CardKey(const std::string& identity)", header)
        # The pieces come from where CARDInit is called with them, and the
        # channel is the one the save driver uses.
        card = CARD_SYS.read_text()
        self.assertIn("CARDInit(PortGci::GameCode(), kCardMaker)", card)
        self.assertIn("sCardIdMaker = std::string(PortGci::GameCode()) + kCardMaker;", card)
        self.assertIn("constexpr int kCardChannel = CMemoryCardSys::kCS_SlotA;", card)
        self.assertIn("aurora_card_get_path(", card)
        resolved = between(card, "const char* CMemoryCardSys::PortCardResolvedDirectory() {", "\n}",
                           CARD_SYS)
        self.assertIn("PortCardChannel()", resolved)
        self.assertIn("sResolved = size != 0 && size <= sizeof(buffer)", resolved)

    def test_record_is_fingerprinted_and_the_load_falls_back(self):
        header = HEADER.read_text()
        # A wrong fingerprint, a longer or shorter file and a count above the
        # cap are all refused, so the caller keeps what the save's trailers gave.
        self.assertIn("count > PortMapMarks::kMaxMarks", header)
        self.assertIn("(lo | (hi << 32)) != fingerprint", header)
        self.assertIn("bytes.size() > kMaxBytes", SIDE.read_text())
        # Reading is the only thing Apply does; nothing is written on a load.
        apply = between(SIDE.read_text(), "bool Apply(int slot", "\n#endif", SIDE)
        self.assertNotIn("WriteFile", apply)
        self.assertNotIn("RemoveFile", apply)
        # And the docs do not claim more than there is: the front end has no
        # slot-copy feature, and an import or a conversion brings trailers only.
        self.assertIn("no slot-copy", header)
        self.assertIn("import or a save conversion", header)

    def test_trailer_keeps_its_first_shape_and_the_side_file_holds_more(self):
        marks = MARKS.read_text()
        self.assertIn("constexpr unsigned kCountBits = 5;", marks)
        self.assertIn("constexpr size_t kTrailerMarks = 20;", marks)
        self.assertIn("constexpr size_t kMaxMarks = 64;", marks)
        self.assertIn("count > kTrailerMarks", marks)
        # Fit never hands the trailer more than its count field holds, and the
        # writer still writes only what fits.
        fit = between(marks, "inline size_t Fit(", "\n}", MARKS)
        self.assertIn("fit < kTrailerMarks ? fit : kTrailerMarks", fit)
        self.assertIn("PortMapMarks::Fit(",
                      (root / "src/MetroidPrime/Player/CGameState.cpp").read_text())

    def test_save_state_carries_the_whole_list(self):
        state = SAVESTATE.read_text()
        capture = between(state, "bool Capture(CStateManager& mgr, Header& header,", "\nvoid ",
                          SAVESTATE)
        self.assertIn("gpGameState->PutTo(out);", capture)
        self.assertIn("gameBits = out.PortBitsWritten();", capture)
        # The record is written after the stream is gone: COutputStream flushes
        # its last byte in the destructor, so a fingerprint taken inside the
        # scope covers bytes that change at once and never matches on load.
        self.assertIn("StoreMapData(blob, gameBits);", capture)
        self.assertIn("}\n", between(capture, "gpGameState->PutTo(out);",
                                     "StoreMapData(blob, gameBits);", SAVESTATE))
        install = between(state, "void InstallPending() {", "\n}\n", SAVESTATE)
        self.assertIn("gpMain->PortLoadGameState(in);", install)
        self.assertIn("LoadMapData(sPendingBlob);", install)
        self.assertIn("constexpr size_t kMapOffset = kBlobSize - 256;", state)
        load = between(state, "bool LoadMapData(const std::vector< uint8_t >& blob) {", "\n}", state)
        self.assertIn("PortMapSide::Decode(", load)
        self.assertIn("PortMapSide::Fingerprint(blob.data(), kMapOffset)", load)

    def test_sources_are_built(self):
        cmake = CMAKE.read_text()
        for source in ("platform/port_map_side.cpp", "platform/port_map_side_paths.cpp"):
            self.assertIn(source, cmake)
        self.assertIn("port_map_side_tests", cmake)
        self.assertIn("port_map_side_wiring", cmake)


if __name__ == "__main__":
    unittest.main(verbosity=2)
