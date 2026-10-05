"""The reusable acceptance driver runs real stages with the test scorer."""

import argparse
import contextlib
import io
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock

from .acceptance import check_publication_store, run
from .common import llm_cc, read_json
from .fixtures import fake_scoring
from .store import FilesystemStore


class NonAtomicCreateStore(FilesystemStore):
    """Replacement is atomic, but creation checks then writes without a lock."""

    def __init__(self, root):
        super().__init__(root)
        self.creators = threading.Barrier(8)

    def put_if(self, key, value, expected):
        if key == "publications/conditional.json" and expected is None:
            existing, _ = self.get_versioned(key)
            if existing is None:
                self.creators.wait(timeout=10)
                self.put(key, value)
                return self.get_versioned(key)[1]
        return super().put_if(key, value, expected)


class AcceptanceTest(unittest.TestCase):
    def test_non_atomic_conditional_creation_fails_acceptance(self):
        with tempfile.TemporaryDirectory() as temporary:
            store = NonAtomicCreateStore(temporary)
            with mock.patch(
                "tools.comparison.acceptance.open_store", return_value=store
            ):
                with self.assertRaisesRegex(RuntimeError, "8 of 8 simultaneous"):
                    check_publication_store(temporary)

    def test_driver_checks_parity_warm_cache_and_path_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model, scoring = fake_scoring(root / "profile")
            output = root / "acceptance"
            args = argparse.Namespace(
                executable=Path(llm_cc()),
                model=model,
                scoring_args=scoring,
                execution_host=None,
                store=None,
                store_options=None,
                output_dir=output,
                timeout_seconds=60,
                warm_budget_seconds=60,
            )
            with contextlib.redirect_stdout(io.StringIO()):
                run(args)
            evidence = read_json(output / "acceptance.json")
            self.assertEqual(evidence["unique_results"], 4)
            self.assertEqual(
                evidence["workers"],
                {
                    "one": 1,
                    "four": 4,
                    "reversed": 1,
                    "warm": 0,
                },
            )
            self.assertTrue(evidence["exact_result_parity"])
            self.assertTrue(evidence["rename_copy_category_move_reused"])
            self.assertEqual(evidence["publication_store"]["stored_counter"], 40)
            self.assertEqual(evidence["publication_store"]["successful_creates"], 1)
            self.assertEqual(
                evidence["publication_store"]["concurrent_create_attempts"], 8
            )
            self.assertEqual(
                read_json(output / "renamed/report/report.json")["status"], "complete"
            )
            self.assertTrue((output / "one-worker-0.stderr").is_file())


if __name__ == "__main__":
    unittest.main()
