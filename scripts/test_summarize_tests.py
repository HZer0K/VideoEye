import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import summarize_tests


class TestSummaryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        tests = self.root / "tests"
        (tests / "unit").mkdir(parents=True)
        (tests / "unit" / "sample.cpp").write_text(
            "TEST(Sample, First) {}\nTEST_F(Sample, Second) {}\n", encoding="utf-8"
        )
        (tests / "quality.cpp").write_text("TEST(Quality, RootCase) {}\n", encoding="utf-8")
        (tests / "CMakeLists.txt").write_text(
            "videoeye_add_test(test_sample unit/sample.cpp)\n"
            "add_test(NAME SampleTests COMMAND test_sample)\n"
            "add_test(NAME ScriptTests COMMAND ${VIDEOEYE_PYTHON_EXE} check.py --check)\n",
            encoding="utf-8",
        )
        patcher = mock.patch.multiple(
            summarize_tests,
            ROOT=str(self.root),
            TESTS_DIR=str(tests),
            CMAKE=str(tests / "CMakeLists.txt"),
        )
        patcher.start()
        self.addCleanup(patcher.stop)
        (self.root / "README.md").write_text(
            "prefix\n" + summarize_tests.SUMMARY_BEGIN + "\nstale\n"
            + summarize_tests.SUMMARY_END + "\nsuffix\n",
            encoding="utf-8",
        )

    def run_summary(self, *arguments):
        with mock.patch.object(sys, "argv", ["summarize_tests.py", *arguments]):
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                return summarize_tests.main()

    def test_counts_root_and_unit_sources(self):
        data = summarize_tests.summarize()
        self.assertEqual(data["gtest_cases"], 3)
        self.assertIn("quality.cpp", data["per_file_cases"])

    def test_counts_python_ctest_groups(self):
        data = summarize_tests.summarize()
        self.assertEqual(data["test_executables"], 1)
        self.assertEqual(data["script_ctest_groups"], 1)
        self.assertEqual(data["total_ctest_groups"], 2)

    def test_stale_readme_fails(self):
        self.assertEqual(self.run_summary("--check-readme"), 1)

    def test_update_then_check_succeeds_without_changing_surrounding_text(self):
        self.assertEqual(self.run_summary("--update-readme"), 0)
        self.assertEqual(self.run_summary("--check-readme"), 0)
        text = (self.root / "README.md").read_text(encoding="utf-8")
        self.assertTrue(text.startswith("prefix\n"))
        self.assertTrue(text.endswith("\nsuffix\n"))

    def test_source_change_invalidates_readme(self):
        self.assertEqual(self.run_summary("--update-readme"), 0)
        with (self.root / "tests" / "quality.cpp").open("a", encoding="utf-8") as source:
            source.write("TEST(Quality, AddedCase) {}\n")
        self.assertEqual(self.run_summary("--check-readme"), 1)

    def test_missing_or_duplicate_markers_fail(self):
        readme = self.root / "README.md"
        readme.write_text("no markers", encoding="utf-8")
        self.assertEqual(self.run_summary("--update-readme"), 1)
        readme.write_text(
            summarize_tests.SUMMARY_BEGIN * 2 + summarize_tests.SUMMARY_END, encoding="utf-8"
        )
        self.assertEqual(self.run_summary("--update-readme"), 1)

    def test_orphan_target_fails(self):
        with (self.root / "tests" / "CMakeLists.txt").open("a", encoding="utf-8") as cmake:
            cmake.write("videoeye_add_test(test_orphan orphan.cpp)\n")
        self.assertEqual(self.run_summary(), 1)


if __name__ == "__main__":
    unittest.main()
