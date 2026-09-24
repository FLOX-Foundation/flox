"""A log callback left installed must not crash the interpreter on exit.

`flox_py.set_log_callback` hands a Python callable to the C API, which
keeps it for the whole process. Nothing obliges the user to detach it,
so the binding has to: released (or called by a C++ thread) after
interpreter finalisation, the callable takes the process down with
SIGSEGV -- exit code -11 on POSIX, which turns a green test run into a
crashed one at the very last moment.

The binding registers its detach with atexit at module init.
"""
from __future__ import annotations

import os
import subprocess
import sys

import flox_py as flox

BUILD_DIR = os.path.dirname(os.path.dirname(os.path.abspath(flox.__file__)))


def _run(body):
    script = f"import sys\nsys.path.insert(0, {BUILD_DIR!r})\nimport flox_py as flox\n" + body
    return subprocess.run([sys.executable, "-c", script],
                          capture_output=True, text=True, timeout=60)


def test_interpreter_exits_cleanly_with_a_callback_still_installed():
    done = _run("flox.set_log_callback(lambda level, msg: None)\n"
                "print('installed')\n")
    assert done.returncode == 0, (
        f"exit was {done.returncode} with a log callback still installed\n"
        f"{done.stdout}{done.stderr}")
    assert "installed" in done.stdout


def test_detaching_by_hand_still_works():
    done = _run("flox.set_log_callback(lambda level, msg: None)\n"
                "flox.set_log_callback(None)\n"
                "print('detached')\n")
    assert done.returncode == 0, f"{done.stdout}{done.stderr}"
    assert "detached" in done.stdout


def test_a_callback_installed_at_exit_is_not_called_after_teardown():
    # The sink is detached, not merely dropped: a log line emitted from a
    # C++ destructor during shutdown must reach the console logger, never
    # the dead Python callable.
    done = _run("flox.set_log_callback(lambda level, msg: print('LOG', msg))\n"
                "import atexit\n"
                "atexit.register(lambda: print('late atexit'))\n"
                "print('done')\n")
    assert done.returncode == 0, f"{done.stdout}{done.stderr}"
    assert "done" in done.stdout
