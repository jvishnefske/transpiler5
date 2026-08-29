#!/usr/bin/env python3
"""FR-144 -- the improvement harness's frozen-population invariants.

WHAT THIS PINS AND WHY. The epoch mechanism exists so that a clippy delta
between two emitter revisions is attributable to the emitter ALONE: a PAIRED
comparison over a pinned population. On 2026-08-28 both consumers of that
premise were found violating it and NOTHING detected either -- epochs 1, 2 and
3 had silently drifted (files edited after the freeze) while `ledger_append`
kept recording trajectories against them, and the committed clippy ratchet was
comparing a 170-crate measurement against a 164-crate baseline and calling the
difference an emitter regression. A wrong number that nobody flags is worse
than no number, so every guard below turns silent drift into a LOUD non-zero
refusal:

  * a drifted or closed epoch must be REFUSED by `ledger_append`, by
    `assert_comparable`, and by anything measuring against it;
  * closing an epoch is history: it must never rewrite the epoch document and
    must never be silently re-closed over;
  * the ratchet must refuse an UNPINNED baseline and refuse a pinned baseline
    whose `corpus_hash` no longer matches the files on disk;
  * the three consumers of "which baseline is authoritative" (clippy_eval's
    default, signals.py's DEFAULT_CLIPPY, controller.py's CLIPPY_BASELINE)
    must name the SAME pinned document -- if they diverge the controller
    commits a file the ratchet does not read, which is how (2) happened.

Hermetic: every mutating case runs against a synthetic corpus in a tempdir
with the module's HERE/LEDGER/STATUS paths redirected. The only assertions
against the real tree are read-only and stable (the retired epochs recorded
closed, the three consumer constants agreeing, exactly one LIVE epoch and the
ratchet pinned to it). It deliberately does NOT assert that the live epoch's
`verify` passes: legitimately editing a pinned EndToEnd test would then fail
the lit gate, whereas the correct response to that is to freeze a NEW epoch --
which is exactly what happened on 2026-08-29, when FR-61f-c edited pinned
`test/EndToEnd/deferred-mut-slice-index.c`, epoch-4 was closed as drifted and
epoch-5 (244 files) took over. The assertions below are therefore written
against "the epoch the authoritative baseline names" rather than a hard-coded
number, so rolling an epoch does not require rewriting them.
"""
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CLIPPY_EVAL_DIR = os.path.join(REPO, "nix", "clippy-eval")
for p in (HERE, CLIPPY_EVAL_DIR):
    if p not in sys.path:
        sys.path.insert(0, p)

import epoch as epoch_mod          # noqa: E402
import signals as signals_mod      # noqa: E402
import controller as controller_mod  # noqa: E402
import clippy_eval as clippy_mod   # noqa: E402


class SyntheticEpoch(unittest.TestCase):
    """Each test gets its own tree + its own epoch/ledger/status documents."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fr144-")
        self.corpus = os.path.join(self.tmp, "corpus")
        os.makedirs(self.corpus)
        for name in ("a.c", "b.c", "c.c"):
            with open(os.path.join(self.corpus, name), "w") as f:
                f.write(f"int {name[0]}(void) {{ return 0; }}\n")
        self.saved = (epoch_mod.HERE, epoch_mod.LEDGER, epoch_mod.STATUS)
        epoch_mod.HERE = self.tmp
        epoch_mod.LEDGER = os.path.join(self.tmp, "ledger.json")
        epoch_mod.STATUS = os.path.join(self.tmp, "epoch-status.json")
        self.metrics = os.path.join(self.tmp, "metrics.json")
        with open(self.metrics, "w") as f:
            json.dump({"total_warnings": 7}, f)

    def tearDown(self):
        epoch_mod.HERE, epoch_mod.LEDGER, epoch_mod.STATUS = self.saved
        shutil.rmtree(self.tmp, ignore_errors=True)

    def freeze(self, epoch_id=9):
        with redirect_stdout(io.StringIO()):
            return epoch_mod.freeze(self.corpus, epoch_id)

    def touch(self, name, text="int extra(void) { return 1; }\n"):
        with open(os.path.join(self.corpus, name), "a") as f:
            f.write(text)

    def run_cmd(self, argv):
        """Run a subcommand through main(), capturing stdout + exit code."""
        buf = io.StringIO()
        try:
            with redirect_stdout(buf):
                rc = epoch_mod.main(argv)
        except SystemExit as e:
            rc = e.code if isinstance(e.code, int) else 1
            buf.write(str(e) + "\n")
        return rc, buf.getvalue()


class TestVerify(SyntheticEpoch):
    def test_clean_epoch_verifies(self):
        self.freeze()
        rc, out = self.run_cmd(["verify", "--id", "9"])
        self.assertEqual(rc, 0, out)
        self.assertIn("verified: 3 files", out)

    def test_edited_file_is_content_changed(self):
        self.freeze()
        self.touch("b.c")
        rc, out = self.run_cmd(["verify", "--id", "9"])
        self.assertEqual(rc, 1, out)
        self.assertIn("DRIFTED", out)
        self.assertIn("CONTENT-CHANGED", out)
        self.assertIn("b.c", out)

    def test_deleted_file_is_missing(self):
        self.freeze()
        os.remove(os.path.join(self.corpus, "c.c"))
        rc, out = self.run_cmd(["verify", "--id", "9"])
        self.assertEqual(rc, 1, out)
        self.assertIn("MISSING", out)


class TestLedgerDriftGuard(SyntheticEpoch):
    """(a) the drift guard: ledger_append must refuse a moved population."""

    def test_append_records_on_a_clean_epoch(self):
        self.freeze()
        with redirect_stdout(io.StringIO()):
            rc = epoch_mod.ledger_append(9, "deadbeef" * 5, self.metrics)
        self.assertEqual(rc, 0)
        with open(epoch_mod.LEDGER) as f:
            ledger = json.load(f)
        self.assertEqual(len(ledger["epochs"]["9"]["revisions"]), 1)

    def test_append_refuses_a_drifted_epoch(self):
        self.freeze()
        self.touch("a.c")
        with self.assertRaises(SystemExit) as cm:
            with redirect_stdout(io.StringIO()):
                epoch_mod.ledger_append(9, "cafe" * 10, self.metrics)
        msg = str(cm.exception)
        self.assertIn("DRIFTED", msg)
        self.assertIn("a.c", msg)
        # and nothing was half-recorded
        self.assertFalse(os.path.exists(epoch_mod.LEDGER))

    def test_append_refuses_after_drift_even_with_prior_history(self):
        """The pre-FR-144 hole exactly: the stored hash still matches the
        document, so the old check passed while the files had moved."""
        self.freeze()
        with redirect_stdout(io.StringIO()):
            epoch_mod.ledger_append(9, "aa" * 20, self.metrics)
        self.touch("b.c")
        with self.assertRaises(SystemExit):
            with redirect_stdout(io.StringIO()):
                epoch_mod.ledger_append(9, "bb" * 20, self.metrics)
        with open(epoch_mod.LEDGER) as f:
            ledger = json.load(f)
        self.assertEqual(len(ledger["epochs"]["9"]["revisions"]), 1)


class TestClose(SyntheticEpoch):
    """(c) closing an epoch is a fresh document, never a rewrite."""

    def test_close_does_not_touch_the_epoch_document(self):
        self.freeze()
        path = epoch_mod.epoch_path(9)
        with open(path, "rb") as f:
            before = f.read()
        self.touch("a.c")
        with redirect_stdout(io.StringIO()):
            epoch_mod.close_epoch(9, "drifted")
        with open(path, "rb") as f:
            self.assertEqual(f.read(), before)

    def test_close_records_the_drift_and_when(self):
        self.freeze()
        self.touch("a.c")
        with redirect_stdout(io.StringIO()):
            epoch_mod.close_epoch(9, "drifted: files edited after the freeze")
        st = epoch_mod.epoch_state(9)
        self.assertTrue(st["drifted"])
        self.assertIn("closed_at", st)
        self.assertIn("closed_at_rev", st)
        self.assertEqual([d["path"] for d in st["drifted_files"]],
                         [os.path.join(self.corpus, "a.c")])
        self.assertEqual(st["drifted_files"][0]["state"], "CONTENT-CHANGED")

    def test_reclose_is_refused(self):
        self.freeze()
        with redirect_stdout(io.StringIO()):
            epoch_mod.close_epoch(9, "first")
        with self.assertRaises(SystemExit) as cm:
            with redirect_stdout(io.StringIO()):
                epoch_mod.close_epoch(9, "second")
        self.assertIn("already closed", str(cm.exception))

    def test_closed_epoch_fails_verify_even_when_files_match(self):
        self.freeze()
        with redirect_stdout(io.StringIO()):
            epoch_mod.close_epoch(9, "superseded by epoch-10")
        rc, out = self.run_cmd(["verify", "--id", "9"])
        self.assertEqual(rc, 1, out)
        self.assertIn("CLOSED", out)

    def test_ledger_append_refuses_a_closed_epoch(self):
        self.freeze()
        with redirect_stdout(io.StringIO()):
            epoch_mod.close_epoch(9, "superseded by epoch-10")
        with self.assertRaises(SystemExit) as cm:
            with redirect_stdout(io.StringIO()):
                epoch_mod.ledger_append(9, "cc" * 20, self.metrics)
        self.assertIn("CLOSED", str(cm.exception))


class TestRatchetPinGuard(SyntheticEpoch):
    """(a)+(b): the ratchet refuses anything that is not a paired comparison."""

    def pinned_baseline(self, epoch_id=9, **over):
        doc = epoch_mod.load_epoch(epoch_id)
        base = {"epoch_id": epoch_id, "corpus_hash": doc["corpus_hash"],
                "crates_linted": 3, "crates_skipped": 0,
                "total_warnings": 5, "by_lint": {}}
        base.update(over)
        return base

    def test_unpinned_baseline_is_not_pinned(self):
        self.assertFalse(clippy_mod.is_pinned(
            {"corpus": "test/EndToEnd", "total_warnings": 68}))
        self.freeze()
        self.assertTrue(clippy_mod.is_pinned(self.pinned_baseline()))

    def test_pinned_population_is_the_epoch_file_list(self):
        self.freeze()
        _, files = clippy_mod.pinned_population(self.pinned_baseline())
        self.assertEqual([os.path.basename(f) for f in files],
                         ["a.c", "b.c", "c.c"])

    def test_drifted_corpus_is_refused(self):
        self.freeze()
        self.touch("c.c")
        with self.assertRaises(clippy_mod.PinError) as cm:
            clippy_mod.pinned_population(self.pinned_baseline())
        self.assertIn("DRIFTED", str(cm.exception))

    def test_closed_epoch_is_refused(self):
        self.freeze()
        with redirect_stdout(io.StringIO()):
            epoch_mod.close_epoch(9, "history")
        with self.assertRaises(clippy_mod.PinError) as cm:
            clippy_mod.pinned_population(self.pinned_baseline())
        self.assertIn("CLOSED", str(cm.exception))

    def test_baseline_pinned_to_another_hash_is_refused(self):
        """The baseline was measured over a DIFFERENT sample of the same id."""
        self.freeze()
        with self.assertRaises(clippy_mod.PinError) as cm:
            clippy_mod.pinned_population(
                self.pinned_baseline(corpus_hash="sha256:" + "0" * 64))
        self.assertIn("corpus_hash", str(cm.exception))


class TestCrateDirCollision(unittest.TestCase):
    """The latent hazard found in the same sweep: a shared tempdir keyed by the
    basename stem would make a union corpus's foo.c and foo.cpp overwrite each
    other, and one file's warnings would vanish from the tally silently."""

    def test_c_and_cpp_of_the_same_stem_get_distinct_crate_dirs(self):
        self.assertNotEqual(clippy_mod.crate_dir_name("test/EndToEnd/foo.c"),
                            clippy_mod.crate_dir_name("test/EndToEnd/foo.cpp"))

    def test_crate_name_is_unchanged_by_the_dir_fix(self):
        """The crate NAME still comes from the stem, so emitted Rust (and the
        goldens over it) do not move."""
        self.assertEqual(clippy_mod.crate_name("test/EndToEnd/range-for.cpp"),
                         "range_for")


# Epochs retired to history, each closed BECAUSE its pinned population had
# moved: 1-3 by FR-144's audit (2026-08-28), 4 when FR-61f-c legitimately
# edited pinned test/EndToEnd/deferred-mut-slice-index.c (2026-08-29). Listed
# explicitly because each is a recorded historical fact, not a derived one.
DRIFTED_CLOSED_EPOCHS = (1, 2, 3, 4)


def _live_epoch_id():
    """The epoch the authoritative ratchet baseline is pinned to."""
    with open(clippy_mod.DEFAULT_BASELINE) as f:
        return json.load(f)["epoch_id"]


class TestCommittedState(unittest.TestCase):
    """Read-only assertions over the real tree."""

    def test_retired_epochs_are_recorded_closed_as_drifted(self):
        for eid in DRIFTED_CLOSED_EPOCHS:
            st = epoch_mod.epoch_state(eid)
            self.assertIsNotNone(st, f"epoch-{eid} must be closed history")
            self.assertTrue(st["drifted"], f"epoch-{eid} closed as drifted")
            self.assertTrue(st["drifted_files"])

    def test_the_live_epoch_is_open(self):
        self.assertIsNone(epoch_mod.epoch_state(_live_epoch_id()))

    def test_every_earlier_epoch_is_closed(self):
        """Exactly one epoch may be live: the one the ratchet is pinned to.
        A superseded epoch left open would let a stale baseline keep
        measuring against a population the loop has already moved past."""
        live = _live_epoch_id()
        for eid in range(1, live):
            self.assertIsNotNone(
                epoch_mod.epoch_state(eid),
                f"epoch-{eid} predates the live epoch-{live} but is not closed")

    def test_closed_epochs_are_refused_for_comparison(self):
        for eid in DRIFTED_CLOSED_EPOCHS:
            with self.assertRaises(SystemExit) as cm:
                epoch_mod.assert_comparable(eid)
            self.assertIn("CLOSED", str(cm.exception))

    def test_the_three_baseline_consumers_agree(self):
        """clippy_eval's default, signals.py's DEFAULT_CLIPPY and
        controller.py's CLIPPY_BASELINE must be the SAME file: the controller
        `--update`s and COMMITS that path, so a divergence commits a document
        the ratchet never reads (exactly defect (2) of FR-144)."""
        paths = {
            "clippy_eval": os.path.realpath(clippy_mod.DEFAULT_BASELINE),
            "signals": os.path.realpath(signals_mod.DEFAULT_CLIPPY),
            "controller": os.path.realpath(controller_mod.CLIPPY_BASELINE),
        }
        self.assertEqual(len(set(paths.values())), 1, paths)
        self.assertTrue(os.path.exists(paths["clippy_eval"]), paths)

    def test_the_authoritative_baseline_is_pinned_to_a_live_epoch(self):
        with open(clippy_mod.DEFAULT_BASELINE) as f:
            base = json.load(f)
        self.assertTrue(clippy_mod.is_pinned(base))
        doc = epoch_mod.load_epoch(base["epoch_id"])
        self.assertEqual(doc["corpus_hash"], base["corpus_hash"])
        self.assertIsNone(epoch_mod.epoch_state(base["epoch_id"]),
                          "the ratchet must not be pinned to a CLOSED epoch")
        self.assertIn("by_lint", base)  # signals.py ranks this
        self.assertEqual(base["crates_linted"], doc["file_count"])

    def test_closed_epochs_were_not_re_frozen(self):
        """A closure records the hash the epoch had when it was closed. If a
        later re-freeze rewrote the document, that hash would no longer match
        -- which would destroy the historical record the closure preserves."""
        for eid in DRIFTED_CLOSED_EPOCHS:
            st = epoch_mod.epoch_state(eid)
            doc = epoch_mod.load_epoch(eid)
            self.assertEqual(st["corpus_hash"], doc["corpus_hash"],
                             f"epoch-{eid}.json was re-frozen after closure")


if __name__ == "__main__":
    unittest.main(verbosity=2)
