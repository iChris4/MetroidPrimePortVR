"""Production wiring guards for the "Extra map UI" setting (map_extra_ui).

The port adds lines to the world map screen's legend and an "X Mark room" prompt
the disc has no place for. That is extra UI, so it is a setting, off by default,
and it must gate exactly those two things: the legend entries (with the pane
scaled to fit them) and the prompt with the pointer click that marks a room.
Pickup dots and their kinds, the saved marks, the cyan drawing of them and the
physical X button are not part of it.

What this checks, none of which a unit test can see (the legend fit and the
prompt live in CAutoMapper.cpp and platform/port_automapper.cpp behind the
running map screen):

  * the setting's default is false, and the key is in the settings table with
    two sample values, which is what the `port_settings_roundtrip` self-test
    (metroid_prime_port --settings-selftest) applies and reads back;
  * the F1 Tracker row and the settings search index row exist, on the page and
    sub-tab the other map rows use;
  * the legend's cache key carries the setting, so switching it while the map
    screen is open rebuilds the text (no stale extended legend);
  * the legend text appends nothing while it is off, the legend's injected
    textures are not even asked for, and the fit's scale is 1 (the disc's own
    dimensions and pivot) whenever it is off;
  * the mark prompt and its pointer click are gated by the setting alone, and
    the hint pane is not a click target without the prompt (no invisible
    target);
  * the pickup dots, the kinds and the room marks do not read the setting;
  * the console command, the docs and the source registration are in place.

It is a source check: no engine code runs and no game is started.
"""
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import (  # noqa: E402
    WiringCase, between, braced, count, index, joined, root_argument, squash)

root = root_argument()
DEBUG_UI = root / "platform/debug_ui.cpp"
DEBUG_UI_INTERNAL = root / "platform/debug_ui/debug_ui_internal.h"
SETTINGS = root / "platform/debug_ui/settings.cpp"
OPTIONS = root / "platform/debug_ui/options.cpp"
TRACKER = root / "platform/debug_ui/tracker.cpp"
DEBUG_HEADER = root / "platform/include/port_debug.h"
INDEX = root / "platform/port_settings_index.inc"
MAPPER = root / "src/MetroidPrime/CAutoMapper.cpp"
MAPPER_PORT = root / "platform/port_automapper.cpp"
CONSOLE = root / "platform/port_console.cpp"
PICKUPS = root / "platform/port_map_pickups.cpp"
MARKS = root / "platform/port_map_marks.cpp"
SETTINGS_DOC = root / "docs/SETTINGS.md"
CONSOLE_DOC = root / "docs/CONSOLE.md"
CMAKE = root / "CMakeLists.txt"


def mapper_text():
    """The map screen's code: the decomp file's hooks and the port file they call."""
    return MAPPER.read_text() + "\n" + MAPPER_PORT.read_text()


class Wiring(WiringCase):
    def test_default_is_off_and_the_key_is_persisted(self):
        ui = DEBUG_UI.read_text()
        self.assertIn("bool sMapExtraUi = false;", ui)
        self.assertNotIn("bool sMapExtraUi = true;", ui)
        settings = SETTINGS.read_text()
        # A plain bool key: BoolKey reads it with ParseBool and writes 1 or 0.
        self.assertIn('BoolKey< &sMapExtraUi >("map_extra_ui")', settings)
        bool_key = braced(settings, "constexpr SettingKey BoolKey(const char* key) {", SETTINGS)
        self.assertIn("*Flag = ParseBool(value);", bool_key)
        self.assertIn("WriteBool(out, *Flag);", bool_key)
        # Two sample values: the round-trip self-test applies each and expects it
        # back, so the key is covered by port_settings_roundtrip.
        self.assertIn('false, "1", "0"}', bool_key)
        self.assertIn("port_settings_roundtrip", CMAKE.read_text())

    def test_accessors_are_declared_and_save_the_setting(self):
        header = DEBUG_HEADER.read_text()
        self.assertIn("bool MapExtraUi();", header)
        self.assertIn("void SetMapExtraUi(bool enabled);", header)
        options = OPTIONS.read_text()
        getter = braced(options, "bool MapExtraUi() {", OPTIONS)
        self.assertIn("return Loaded(sMapExtraUi);", getter)
        setter = braced(options, "void SetMapExtraUi(bool enabled) {", OPTIONS)
        self.assertIn("StoreSetting(sMapExtraUi, enabled);", setter)
        # StoreSetting is the plain setter's body: it marks the settings for saving.
        store = braced(DEBUG_UI_INTERNAL.read_text(), "void StoreSetting(T& setting, const V& value) {",
                       DEBUG_UI_INTERNAL)
        self.assertIn("setting = value;", store)
        self.assertIn("MarkDirty();", store)

    def test_f1_row_and_search_index(self):
        tracker = TRACKER.read_text()
        row = between(tracker, 'if (ImGui::Checkbox("Extra map UI", &extraUi)) {', "\n  }", TRACKER)
        self.assertIn("SetMapExtraUi(extraUi);", row)
        index = INDEX.read_text()
        # The Tracker page with no sub-tab, as the rows of the map settings.
        self.assertIn('{"Tracker", "", "", "Extra map UI", ', index)
        # The label the index points at is the widget's, and the help text says
        # what it does and that it is off by default.
        help_text = joined(between(tracker, 'ImGui::Checkbox("Extra map UI"', "DrawTrackerLogic",
                                  TRACKER))
        self.assertIn("Off by default", help_text)
        self.assertIn("Room marks still work with X", help_text)

    def test_the_legend_key_carries_the_setting(self):
        mapper = mapper_text()
        key = braced(mapper, "uint PortLegendKey() {", MAPPER)
        self.assertIn("PortDebug::MapPickupKinds() ? 8u : 0u", key)
        self.assertIn("PortDebug::MapExtraUi() ? 16u : 0u", key)
        # It is the only key of the legend's text, so switching the setting in an
        # open map screen rebuilds it.
        self.assertIn("sPortLegendKey != PortLegendKey()", mapper)

    def test_legend_text_and_assets_are_gated(self):
        mapper = mapper_text()
        text = braced(mapper, "rstl::wstring PortLegendText() {", MAPPER)
        gate = index(text, "if (!PortDebug::MapExtraUi()) {")
        # Nothing is appended before the gate: the disc's own text alone.
        self.assertLess(gate, index(text, "entry("), "an entry is appended first")
        ready = squash(braced(mapper, "bool PortLegendReady() {", MAPPER))
        gate = index(ready, "if (!PortDebug::MapExtraUi()) {")
        self.assertIn("return true;", ready[gate:ready.index("}", gate) + 1])
        # The injected legend textures are not created before that early return.
        self.assertLess(gate, index(ready, "new TToken< CTexture >"), "textures loaded first")
        # Off, the pane keeps the disc's own dimensions and pivot: the fit's
        # scale is 1 and the pivot compensation is a no-op.
        fit = between(mapper, "void PortSetLegend(", "legend->SetDimensions(", MAPPER)
        self.assertIn("rows > base ? static_cast< float >(base) / static_cast< float >(rows) : 1.f",
                      fit)

    def test_prompt_and_click_are_gated_and_nothing_else_is(self):
        mapper = mapper_text()
        prompt = braced(mapper, "bool CAutoMapper::PortShowsMarkPrompt() {", MAPPER)
        self.assertTrue(prompt.lstrip().startswith("{"), prompt[:40])
        self.assertTrue(squash(prompt).startswith(squash("{ if (!PortDebug::MapExtraUi()")),
                        "the prompt's first gate is the setting")
        self.assertIn("PortPlaceMarkHint(x2fc_textpane_hint, PortShowsMarkPrompt());", mapper)
        # The click on the hint pane is the prompt's alone: gated, and with the
        # glyph test that keeps the pane from being a wide invisible target.
        self.assertEqual(count(mapper, "PortPointerMenu::TakeClickOn(x2fc_textpane_hint)"), 1)
        click = between(mapper, "// A click on the mark prompt presses X",
                        "PortPointerMenu::TakeClickOn(x2fc_textpane_hint)", MAPPER)
        self.assertIn("PortShowsMarkPrompt() &&", click)
        self.assertIn("PortMarkPromptUnderPointer(x2fc_textpane_hint) &&", click)
        under = braced(mapper, "static bool PortMarkPromptUnderPointer(", MAPPER)
        self.assertIn("PortGuiHit::LastBounds(pane, glyphs)", under)
        # The marks and the dots are not part of the setting.
        self.assertIn("PortMapMarks::Toggle(", mapper)
        for path in (PICKUPS, MARKS):
            self.assertNotIn("MapExtraUi", path.read_text(), path)

    def test_pane_caches_are_reset_with_the_frame(self):
        # The frame's widgets are new objects each time it is loaded, but a freed
        # pane can come back at the same address: a cache left over from the old
        # frame would keep the disc's geometry from being taken again, would skip
        # the fit on the reopened frame and would leave the prompt unplaced.
        mapper = mapper_text()
        forget = braced(mapper, "void PortForgetMapPanes() {", MAPPER)
        for reset in ("sPortLegendFit.pane = nullptr;", "sPortLegendFit.rows = 0;",
                      "sPortLegendFit.retailRows = 0;", "sPortHintPane.pane = nullptr;",
                      "sPortHintPane.placed = false;"):
            self.assertIn(reset, forget)
        # Once when the frame is loaded and once when it is dropped, and the load
        # one comes before the frame's legend pane is read.
        self.assertEqual(count(mapper, "PortForgetMapPanes();"), 2, "one call each way")
        load = between(mapper, "if (x28_frmeMapScreen.get() != NULL && x2c_frmeInitialized == NULL) {",
                       'FindWidget("textpane_mapLegend")', MAPPER)
        self.assertIn("PortForgetMapPanes();", load)
        drop = between(mapper, "if (x1c0_nextState == kAMS_MiniMap) {", "x28_frmeMapScreen = NULL;",
                       MAPPER)
        self.assertIn("PortForgetMapPanes();", drop)

    def test_legend_labels_stay_within_the_disc_legend_width(self):
        # The left pane slides out by a fixed distance to hide the legend (key
        # state 2), so the port's entries may not draw the block wider than the
        # disc's own: with the labels below the appended lines are 233 text px and
        # the row ratio (11/13) draws them 197 px wide against the disc's 198.
        # Longer labels would leave a tail on screen while the panel is away.
        mapper = mapper_text()
        labels = re.findall(r'entry\(\s*PortCustomRes::kLegend\w+Txtr\s*,\s*L"([^"]+)"\s*\)',
                            mapper)
        self.assertEqual(sorted(labels), ["Collected", "Major", "Marked", "Minor", "Pickup"])
        self.assertLessEqual(max(len(l) for l in labels), 9, labels)
        paired = sorted((len(l) for l in labels), reverse=True)
        self.assertLessEqual(sum(paired[:2]), 15, "the widest two-per-line pair is 15 chars")

    def test_console_docs_and_registration(self):
        console = CONSOLE.read_text()
        cmd = braced(console, "void CmdMapUi()", CONSOLE)
        self.assertIn('{"mapui", CmdMapUi,', console)
        self.assertIn("PortDebug::SetMapExtraUi(v == \"1\");", cmd)
        self.assertIn('Out(PortDebug::MapExtraUi() ? "mapui 1" : "mapui 0");', cmd)
        self.assertIn("mapui", CONSOLE_DOC.read_text())
        doc = SETTINGS_DOC.read_text()
        self.assertIn("`map_extra_ui`, off by default", doc)
        # Touch marking needs the setting: the prompt is what is tapped.
        self.assertIn("touch needs the setting on", doc)
        cmake = CMAKE.read_text()
        self.assertIn("port_map_extra_ui_wiring", cmake)


if __name__ == "__main__":
    unittest.main(verbosity=2)
