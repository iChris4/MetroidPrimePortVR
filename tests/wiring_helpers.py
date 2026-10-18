"""Shared helpers for the tests/port_*_wiring.py source checks.

The wiring tests read C++ sources and assert that particular lines are, or are
not, present. Written against exact text, a clang-format reflow (an argument
list wrapped onto the next line, a re-indented block) breaks them without any
change in what the code does. So the checks here compare a canonical form:

  * a whitespace run between two identifier characters (`return x`,
    `const bool`) must still be whitespace, of any length or kind;
  * any other whitespace (around punctuation: `foo(\\n    a, b)`,
    `TCastToPtr< T >`) is ignored.

No non-whitespace character is ever ignored, so a check still fails on any
change to the tokens it names. (So a check on comment text still breaks when
the comment is re-wrapped: the `//` that starts the next line is a token.)

Markers that spell out a line break (`"\\n}"`, `"\\n    }"`, `"\\nvoid "`) are
the exception for `find`, `partition` and `between`: their line structure is
how they pick the end of a function or a block at one nesting level, so they
are matched exactly. The assertions themselves (`assertIn`, `assertNotIn`,
`count`, `index`) always compare canonical forms.

Each test finds this module through its own directory, since CTest runs them
with `python3 -I`, which leaves the script's directory off sys.path.
"""
import pathlib
import re
import sys
import unittest

_WORD = re.compile(r"\w")
_SPACE = re.compile(r"\s+")


def _is_word(character):
    return bool(character) and bool(_WORD.match(character))


def squash(text):
    """The canonical form: whitespace kept, as one space, only between two word characters."""
    out = []
    for part in _SPACE.split(text):
        if not part:
            continue
        if out and _is_word(out[-1][-1]) and _is_word(part[0]):
            out.append(" ")
        out.append(part)
    return "".join(out)


def _pattern(marker):
    """A regex matching marker in source whatever its whitespace, per squash's rule."""
    tokens = squash(marker.strip())
    out = []
    for i, character in enumerate(tokens):
        if character == " ":  # only ever between two word characters
            out.append(r"\s+")
            continue
        if i and tokens[i - 1] != " " and not (_is_word(tokens[i - 1]) and _is_word(character)):
            out.append(r"\s*")
        out.append(re.escape(character))
    return re.compile("".join(out))


def find(text, marker, start=0):
    """(begin, end) of the first marker in text at or after start, or None."""
    if "\n" in marker:
        begin = text.find(marker, start)
        return None if begin < 0 else (begin, begin + len(marker))
    match = _pattern(marker).search(text, start)
    return None if match is None else match.span()


def partition(text, marker):
    """(text before marker, text after it), or None when marker is absent."""
    span = find(text, marker)
    return None if span is None else (text[:span[0]], text[span[1]:])


def between(text, start, end, where):
    """The text after the first start and before the next end; both must exist."""
    head = find(text, start)
    assert head is not None, f"{where}: no {start!r}"
    tail = find(text, end, head[1])
    assert tail is not None, f"{where}: no {end!r} after {start!r}"
    return text[head[1]:tail[0]]


def braced(text, signature, where):
    """The braced body of the function at signature, nested blocks included."""
    span = find(text, signature)
    assert span is not None, f"{where}: no {signature!r}"
    i = text.index("{", span[0])
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    raise AssertionError(f"{where}: {signature!r} has no closing brace")


def joined(text):
    """Source with adjacent string literals ("a" "b", split across lines) joined."""
    return re.sub(r'"\s*\n\s*"', "", text)


def count(text, needle):
    """How often needle occurs in text, comparing canonical forms."""
    return squash(text).count(squash(needle))


def index(text, needle):
    """needle's position in squash(text); compare it only with other index() results."""
    return squash(text).index(squash(needle))


def root_argument():
    """The path CMake passes as the test's last argument."""
    return pathlib.Path(sys.argv.pop())


class WiringCase(unittest.TestCase):
    """assertIn/assertNotIn on two strings compare their canonical forms."""

    def assertIn(self, member, container, msg=None):
        if isinstance(member, str) and isinstance(container, str):
            if squash(member) not in squash(container):
                self.fail(self._formatMessage(msg, f"{member!r} not found in source"))
            return
        super().assertIn(member, container, msg)

    def assertNotIn(self, member, container, msg=None):
        if isinstance(member, str) and isinstance(container, str):
            if squash(member) in squash(container):
                self.fail(self._formatMessage(msg, f"{member!r} unexpectedly found in source"))
            return
        super().assertNotIn(member, container, msg)
