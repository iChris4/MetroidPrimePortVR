"""The debug console's command table against its documentation.

platform/port_console.cpp dispatches every command through one table,
kCommands, which also holds the `help` lines. This checks, from the source
alone, that:

  * every command in the table is named in docs/CONSOLE.md, as `name` or
    `name <args...>` in code quotes;
  * a command's first row is the one with a handler (later rows of the same
    name only carry more of its help), so the dispatch finds it;
  * no command has two rows with handlers.

It is a source check: no engine code runs and no game is started.
"""
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, between, root_argument  # noqa: E402

root = root_argument()
CONSOLE = root / "platform/port_console.cpp"
CONSOLE_DOC = root / "docs/CONSOLE.md"


def ordered_rows():
    """(name, has a handler) for each row of kCommands, in table order."""
    table = between(CONSOLE.read_text(), "const CommandSpec kCommands[] = {", "\n};\n", CONSOLE)
    rows = []
    for m in re.finditer(r'^    \{"([a-z]+)", ([^,]+), ([^,]+)', table, re.M):
        frame, tick = m.group(2), m.group(3)
        rows.append((m.group(1), frame != "nullptr" or tick != "nullptr"))
    return rows


class ConsoleDocs(WiringCase):
    def test_the_table_has_rows(self):
        names = {name for name, _ in ordered_rows()}
        # A sample, so a table the pattern no longer reads fails here.
        for name in ("status", "warp", "fx", "roomgeo", "press", "help", "quit"):
            self.assertIn(name, names)

    def test_first_row_runs_the_command(self):
        seen = set()
        for name, handled in ordered_rows():
            if name not in seen:
                self.assertTrue(handled, f"the first kCommands row of {name!r} has no handler")
                seen.add(name)
            else:
                self.assertFalse(handled, f"{name!r} has a second kCommands row with a handler")

    def test_every_command_is_documented(self):
        doc = CONSOLE_DOC.read_text()
        missing = []
        for name in dict.fromkeys(name for name, _ in ordered_rows()):
            if not re.search(r"`%s[\s`]" % re.escape(name), doc):
                missing.append(name)
        self.assertEqual(missing, [], "commands not in docs/CONSOLE.md")


if __name__ == "__main__":
    unittest.main(verbosity=2)
