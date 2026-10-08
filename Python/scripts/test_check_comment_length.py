#!/usr/bin/env python3
"""Unit tests for check_comment_length.py.

The rule is "no comment block longer than 3 lines", so what matters is that the checker counts
blocks the way a reader sees them: consecutive comment-only lines are one block, a trailing
comment on a code line is not, and neither a ``//`` inside a literal nor a quote inside a
comment can confuse it. That last one is a regression test: the first version treated the ``"``
in ``the "World memory leaks" ensure`` as a string start and blanked the following line's ``//``,
so it under-counted real violations by 16.
"""

from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_comment_length",
                                              SCRIPT_DIR / "check_comment_length.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

PLUGIN_REL = "MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Fake.cpp"


class CommentCountingTests(unittest.TestCase):
    def spans(self, text: str) -> list[tuple[int, int, int]]:
        return checker.long_comments(text)

    def test_three_line_block_passes(self):
        text = "int x;\n// one\n// two\n// three\nint y;\n"
        self.assertEqual(self.spans(text), [])

    def test_four_line_block_fails_with_its_range(self):
        text = "int x;\n// one\n// two\n// three\n// four\nint y;\n"
        self.assertEqual(self.spans(text), [(2, 5, 4)])

    def test_quote_inside_a_comment_does_not_hide_lines(self):
        """Regression: an unmatched quote in comment text must not blank the next line's //."""
        text = ('    // see the "World memory\n'
                '    // leaks" ensure\n'
                '    // third\n'
                '    // fourth\n'
                '    int y;\n')
        self.assertEqual(self.spans(text), [(1, 4, 4)])

    def test_apostrophe_in_a_comment_is_not_a_char_literal(self):
        text = "// the editor's selection set\n// second\n// third\n// fourth\n"
        self.assertEqual(self.spans(text), [(1, 4, 4)])

    def test_block_comment_spanning_lines(self):
        text = "int x;\n/* one\n two\n three\n four */\nint y;\n"
        self.assertEqual(self.spans(text), [(2, 5, 4)])

    def test_three_line_block_comment_passes(self):
        self.assertEqual(self.spans("/**\n * one\n */\n"), [])

    def test_double_slash_inside_a_literal_is_not_a_comment(self):
        self.assertEqual(self.spans('auto U = TEXT("http://127.0.0.1:55557");\n'), [])

    def test_trailing_comments_do_not_merge(self):
        text = "".join(f"int v{i};  // field {i}\n" for i in range(6))
        self.assertEqual(self.spans(text), [])

    def test_blank_line_keeps_blocks_separate(self):
        text = "// one\n// two\n// three\n\n// four\n// five\n// six\n"
        self.assertEqual(self.spans(text), [])

    def test_unterminated_literal_does_not_swallow_the_file(self):
        text = 'const char* s = "oops\n// one\n// two\n// three\n// four\n'
        self.assertEqual(self.spans(text), [(2, 5, 4)])


class CliTests(unittest.TestCase):
    def _write(self, root: pathlib.Path, body: str) -> None:
        path = root / PLUGIN_REL
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")

    def test_clean_tree_exits_zero(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            self._write(tmp, "// short\nint x;\n")
            self.assertEqual(checker.main(["--root", str(tmp), "--quiet"]), 0)

    def test_long_comment_exits_one(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            self._write(tmp, "// a\n// b\n// c\n// d\n")
            self.assertEqual(checker.main(["--root", str(tmp), "--quiet"]), 1)

    def test_scope_is_cpp_and_h_only(self):
        """A long comment in a non-C++ file under the plugin tree is out of scope."""
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            python_file = tmp / "MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/thing.py"
            python_file.parent.mkdir(parents=True, exist_ok=True)
            python_file.write_text("# a\n# b\n# c\n# d\n", encoding="utf-8")
            self.assertEqual(checker.main(["--root", str(tmp), "--quiet"]), 0)

    def test_max_lines_is_configurable(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            self._write(tmp, "// a\n// b\n// c\n// d\n")
            self.assertEqual(checker.main(["--root", str(tmp), "--quiet", "--max-lines", "4"]), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
