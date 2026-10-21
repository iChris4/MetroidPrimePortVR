"""MSVC string literal limit guard (issue #170).

MSVC's cl.exe rejects a single string literal piece over 16380 bytes (C2026);
adjacent literals still join, up to about 64 KB. clang-cl, which CI builds
Windows with, has no such limit, so a long shader source can grow past it
unnoticed. Every raw string literal in the C/C++ sources stays under it.

It is a source check: nothing is compiled here.
"""
import pathlib
import re
import sys
import unittest

LIMIT = 16380
DIRS = ("src", "platform", "include", "extern/aurora/lib", "extern/aurora/include")
SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".inc"}
RAW = re.compile(r'R"([^()\\\s]{0,16})\((.*?)\)\1"', re.S)

root = pathlib.Path(sys.argv.pop())


class Limit(unittest.TestCase):
    def test_raw_literals_fit_msvc(self):
        long = []
        for d in DIRS:
            for path in sorted((root / d).rglob("*")):
                if path.suffix not in SUFFIXES or not path.is_file():
                    continue
                text = path.read_text(encoding="utf-8", errors="replace")
                for match in RAW.finditer(text):
                    size = len(match.group(2).encode("utf-8"))
                    if size > LIMIT:
                        line = text.count("\n", 0, match.start()) + 1
                        long.append(f"{path.relative_to(root)}:{line}: {size} bytes")
        self.assertEqual(long, [], "split these into adjacent pieces under 16 KB")


if __name__ == "__main__":
    unittest.main()
