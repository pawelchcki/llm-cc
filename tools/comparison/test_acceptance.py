"""The reusable acceptance driver runs real stages with the test scorer."""

import argparse
import contextlib
import io
from pathlib import Path
import tempfile
import unittest

from .acceptance import run
from .common import llm_cc, read_json
from .fixtures import fake_scoring


class AcceptanceTest(unittest.TestCase):
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
            self.assertEqual(
                read_json(output / "renamed/report/report.json")["status"], "complete"
            )
            self.assertTrue((output / "one-worker-0.stderr").is_file())


if __name__ == "__main__":
    unittest.main()
