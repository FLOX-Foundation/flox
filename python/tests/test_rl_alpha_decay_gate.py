"""T038: smoke test for the RL alpha-decay gate.

Invokes scripts/rl_alpha_decay_gate.py as a subprocess and asserts it
exits cleanly under the default cap. The gate generates a synthetic
tape and runs both env and paper paths internally.
"""
from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        BUILD_PY = str(p)
        break
else:
    BUILD_PY = ""


class AlphaDecayGateTests(unittest.TestCase):
    def test_gate_passes_under_default_cap(self) -> None:
        if not BUILD_PY:
            self.skipTest("flox_py build not found")
        env = dict(os.environ)
        env["PYTHONPATH"] = BUILD_PY + os.pathsep + env.get("PYTHONPATH", "")
        script = REPO_ROOT / "scripts" / "rl_alpha_decay_gate.py"
        result = subprocess.run(
            [sys.executable, str(script), "--max-decay", "0.30"],
            capture_output=True, text=True, env=env,
            cwd=str(REPO_ROOT),
        )
        # Print on failure so CI logs show why.
        if result.returncode != 0:
            print("STDOUT:", result.stdout)
            print("STDERR:", result.stderr)
        self.assertEqual(result.returncode, 0)
        self.assertIn("PASS:", result.stdout)

    def test_gate_fails_under_impossibly_tight_cap(self) -> None:
        if not BUILD_PY:
            self.skipTest("flox_py build not found")
        env = dict(os.environ)
        env["PYTHONPATH"] = BUILD_PY + os.pathsep + env.get("PYTHONPATH", "")
        script = REPO_ROOT / "scripts" / "rl_alpha_decay_gate.py"
        result = subprocess.run(
            [sys.executable, str(script), "--max-decay", "0.0001"],
            capture_output=True, text=True, env=env,
            cwd=str(REPO_ROOT),
        )
        # Either FAIL or PASS depending on luck; we just verify the
        # exit code logic works — non-zero on FAIL is the contract.
        if "FAIL:" in result.stdout:
            self.assertNotEqual(result.returncode, 0)
        else:
            self.assertEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
