"""Equivalence test: the current engine against the reference engine.

The corpus source is compiled twice, once with tests/reference on the include path and once with
src/engine, into two separate executables. Nothing is linked together, so no public symbol exists
twice. Every field of all 64 rendered steps is compared.

One difference is expected: the twelve scale rows the OS 1.44 table leaves empty now read as
chromatic. A corpus case whose key scale is one of those may differ from the reference, and then
only in its note field; every other case and
every other field must match exactly. The literals behind the exception live in tests/test_engine.c.
"""
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REFERENCE = REPO / "tests" / "reference"
CORPUS = REPO / "tests" / "engine_corpus.c"
CFLAGS = ["-std=c99", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
          "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build(output, include, corpus=CORPUS, extra=()):
    cmd = [os.environ.get("CC") or "cc", *CFLAGS, *extra, "-I", str(include), str(corpus),
           str(Path(include) / "engine.c"), "-o", str(output)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("compiler failed:\n" + result.stdout + result.stderr)
    return output


def run(binary):
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    return result.stdout


def check_corpus_output(text):
    """Every case must carry one header and 64 complete five-field step records."""
    cases = steps = 0
    ended = False
    for line in text.splitlines():
        if line.startswith("C "):
            cases += 1
            steps = 0
        elif line.startswith("S "):
            fields = line.split()
            assert len(fields) == 7, "malformed step record: %r" % line
            step = int(fields[1])
            assert step == steps, "step order broken in case %d: %d != %d" % (cases, step, steps)
            trig, note, vel, slide, hold = (int(v) for v in fields[2:])
            assert trig in (0, 1) and 0 <= note <= 127 and 0 <= vel <= 127
            assert slide in (0, 1, 2) and hold in (0, 1)
            steps += 1
        elif line.startswith("END "):
            assert steps == 64, "last case has %d steps" % steps
            ended = True
    assert cases >= 1000, "corpus shrank unexpectedly: %d cases" % cases
    assert ended, "corpus output has no END record"
    return cases


# The scale rows the OS 1.44 table at 0x401d42a0 leaves empty (0xff): MUTINY reads them as
# chromatic instead of the conventional intervals the reference still holds.
CHROMATIC_SCALES = frozenset({7, 8, 12, 13, 14, 15, 16, 17, 18, 20, 21, 22})


def case_scale(line):
    return int(line.split("scale=")[1].split()[0])


def records(text):
    """Split corpus output into one list of lines per C record (header + 64 step lines)."""
    out = []
    current = None
    for line in text.splitlines():
        if line.startswith("C "):
            current = [line]
            out.append(current)
        elif line.startswith("S "):
            assert current is not None, "step record before its header"
            current.append(line)
        elif line.startswith("END "):
            assert current is None or len(current) == 1 + 64
    for record in out:
        assert len(record) == 1 + 64, "record has %d lines" % len(record)
    return out


class EngineEquivalenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = Path(tempfile.mkdtemp(prefix="mutiny-equivalence-"))
        cls.reference = build(cls.work / "reference", REFERENCE)
        cls.current = build(cls.work / "current", REPO / "src" / "engine")
        # The cached production path, and the T4 build mode, against the same reference.
        cls.cached = build(cls.work / "cached", REPO / "src" / "engine",
                           extra=("-DMU_USE_CACHE",))
        cls.reference_t4 = build(cls.work / "reference-t4", REFERENCE,
                                 extra=("-DMU_ACID_T4=1",))
        cls.current_t4 = build(cls.work / "current-t4", REPO / "src" / "engine",
                               extra=("-DMU_ACID_T4=1",))
        cls.cached_t4 = build(cls.work / "cached-t4", REPO / "src" / "engine",
                              extra=("-DMU_ACID_T4=1", "-DMU_USE_CACHE"))
        cls.reference_text = run(cls.reference)
        cls.current_text = run(cls.current)
        cls.cached_text = run(cls.cached)
        cls.reference_t4_text = run(cls.reference_t4)
        cls.current_t4_text = run(cls.current_t4)
        cls.cached_t4_text = run(cls.cached_t4)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def test_reference_snapshot_is_unchanged(self):
        manifest = json.loads((REFERENCE / "manifest.json").read_text())
        self.assertEqual(sha256(REFERENCE / "engine.c"), manifest["sha256"])
        self.assertEqual(sha256(REFERENCE / "engine.h"), manifest["header_sha256"])

    def test_corpus_is_complete(self):
        cases = check_corpus_output(self.reference_text)
        self.assertGreaterEqual(cases, 1000)

    def compare(self, actual_text, label, reference_text=None):
        """Every line must match, except a dropped scale row's notes (the listed exception)."""
        reference_text = self.reference_text if reference_text is None else reference_text
        ref = reference_text.splitlines()
        cur = actual_text.splitlines()
        self.assertEqual(len(ref), len(cur),
                         "%s output structure differs from the reference" % label)
        scale = None
        for index, (expected, got) in enumerate(zip(ref, cur)):
            if expected.startswith("C "):
                scale = case_scale(expected)
            if expected == got:
                if expected.startswith("C "):
                    self.assertEqual(got, expected, "%s header %d differs" % (label, index))
                continue
            self.assertTrue(expected.startswith("S ") and got.startswith("S "),
                            "%s header %d differs:\nreference: %s\nactual:    %s"
                            % (label, index + 1, expected, got))
            fields_ref, fields_got = expected.split(), got.split()
            self.assertEqual(len(fields_ref), len(fields_got),
                             "%s step record %d is malformed" % (label, index + 1))
            self.assertIn(scale, CHROMATIC_SCALES,
                          "%s differs at output line %d for a scale row outside the listed "
                          "exception:\nreference: %s\nactual:    %s"
                          % (label, index + 1, expected, got))
            # the exception is the interval table: only the note may move
            self.assertEqual([fields_ref[i] for i in (0, 1, 2, 4, 5, 6)],
                             [fields_got[i] for i in (0, 1, 2, 4, 5, 6)],
                             "%s changed more than the note at line %d" % (label, index + 1))

    def compare_cached_pairs(self, actual_text, label, reference_text):
        """The cached corpus prints each case twice: cold, then an immediate hit."""
        reference = records(reference_text)
        cached = records(actual_text)
        self.assertEqual(len(cached), 2 * len(reference),
                         "%s did not print one cold and one hit record per case" % label)
        for case, expected in enumerate(reference):
            for half, what in ((0, "cold"), (1, "hit")):
                got = cached[2 * case + half]
                if got == expected:
                    continue
                self.assertIn(case_scale(expected[0]), CHROMATIC_SCALES,
                              "%s %s record for case %d differs from the reference:\n"
                              "reference: %s\nactual:    %s"
                              % (label, what, case + 1, expected[0], got[0]))
                for line_ref, line_got in zip(expected, got):
                    if line_ref == line_got:
                        continue
                    fields_ref, fields_got = line_ref.split(), line_got.split()
                    self.assertEqual([fields_ref[i] for i in (0, 1, 2, 4, 5, 6)],
                                     [fields_got[i] for i in (0, 1, 2, 4, 5, 6)],
                                     "%s changed more than the note in case %d" % (label, case + 1))

    def test_every_field_of_every_step_matches(self):
        self.compare(self.current_text, "current")

    def test_cached_production_path_matches_every_field(self):
        self.compare_cached_pairs(self.cached_text, "cached", self.reference_text)

    def test_t4_mode_matches_every_field(self):
        self.compare(self.current_t4_text, "current T4", self.reference_t4_text)

    def test_cached_t4_mode_matches_every_field(self):
        self.compare_cached_pairs(self.cached_t4_text, "cached T4", self.reference_t4_text)

    def test_a_single_mutated_reference_field_is_detected(self):
        mutated = self.work / "mutated"
        mutated.mkdir()
        shutil.copy(REFERENCE / "engine.h", mutated / "engine.h")
        source = (REFERENCE / "engine.c").read_text()
        marker = "MU_VEL_FLAT - acnt * 60u / 100u"
        self.assertIn(marker, source)
        (mutated / "engine.c").write_text(source.replace(marker, "MU_VEL_FLAT - acnt * 61u / 100u"))
        binary = build(self.work / "mutated-bin", mutated)
        self.assertNotEqual(run(binary), self.reference_text,
                            "a mutated velocity field produced identical corpus output")


if __name__ == "__main__":
    unittest.main()
