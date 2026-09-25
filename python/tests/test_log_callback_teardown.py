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


def _run_raw(script):
    """Run a script that decides for itself when to import flox_py.

    The import is what registers the binding's atexit teardown, and
    atexit runs LIFO, so anything registered *before* the import runs
    *after* that teardown. That ordering is the only way to observe the
    window these tests are about, so they cannot use _run().
    """
    return subprocess.run([sys.executable, "-c", script],
                          capture_output=True, text=True, timeout=120)


# Emits one engine log line at error level through the public surface: a
# strategy callback that raises is reported by the bridge guard
# (python/callback_guard.h) into the same engine log that
# set_log_callback redirects.
_EMIT_A_LOG_LINE = """
def emit_a_log_line():
    reg = flox_py.SymbolRegistry()
    sym = reg.add_symbol("test", "BTC", 0.01)
    runner = flox_py.Runner(reg, lambda sig: None, threaded=False)

    class S(flox_py.Strategy):
        def on_trade(self, ctx, trade):
            raise RuntimeError("LATE_LINE")

    runner.add_strategy(S(symbols=[sym]))
    runner.start()
    runner.on_trade(sym, 100.0, 1.0, True, 1)
    runner.stop()
"""


def test_a_log_line_emitted_after_teardown_does_not_reach_the_python_sink():
    """The sink is detached before the interpreter goes, and stays detached.

    Registering an atexit handler *before* importing flox_py puts it
    after the binding's own teardown in LIFO order, so by the time it
    runs the sink must already be gone: a log line emitted there has to
    land on the bundled console logger, not on a Python callable the
    interpreter is in the middle of taking apart.

    With no atexit registration nothing detaches, the sink is still live
    in that window, and SEEN grows -- the same live callable a C++
    thread could reach after finalisation.
    """
    done = _run_raw(f"""
import atexit, sys
sys.path.insert(0, {BUILD_DIR!r})

SEEN = []
{_EMIT_A_LOG_LINE}

def late():
    import flox_py
    before = len(SEEN)
    emit_a_log_line()
    print("LATE_REPORTS", len(SEEN) - before)
    sys.stdout.flush()

atexit.register(late)          # registered first, so it runs last
import flox_py                 # the binding registers its teardown second
flox_py.set_log_callback(lambda level, msg: SEEN.append(msg))
emit_a_log_line()
print("EARLY_REPORTS", len(SEEN))
""")

    assert done.returncode == 0, f"{done.stdout}{done.stderr}"
    assert "EARLY_REPORTS 1" in done.stdout, (
        "the sink never received the report while the interpreter was up, "
        f"so the late check below proves nothing\n{done.stdout}{done.stderr}")
    assert "LATE_REPORTS 0" in done.stdout, (
        "a log line emitted after the binding's atexit teardown still reached "
        f"the Python sink\n{done.stdout}{done.stderr}")


def test_a_sink_installed_after_teardown_does_not_crash_on_exit():
    """The slot must outlive the interpreter, not be destroyed with it.

    A callable installed after the atexit teardown has already run has no
    detach left to save it, so it is still in the slot at finalisation.
    The binding keeps the slot in a deliberately leaked heap py::object
    for exactly this: a function-static py::object runs its destructor
    during static destruction, after the interpreter is gone, and takes
    the process down with SIGSEGV (exit -11).
    """
    done = _run_raw(f"""
import atexit, sys
sys.path.insert(0, {BUILD_DIR!r})

def late():
    import flox_py
    # After the binding's own teardown: nothing will detach this one.
    flox_py.set_log_callback(lambda level, msg: None)
    print("reinstalled")
    sys.stdout.flush()

atexit.register(late)
import flox_py
flox_py.set_log_callback(lambda level, msg: None)
print("installed")
""")

    assert done.returncode == 0, (
        f"exit was {done.returncode} with a sink installed after teardown\n"
        f"{done.stdout}{done.stderr}")
    assert "installed" in done.stdout and "reinstalled" in done.stdout, (
        f"{done.stdout}{done.stderr}")


_RAISING_SINK_SCRIPT = """
def sink(level, msg):
    raise RuntimeError("SINK_BLEW_UP")

flox.set_log_callback(sink)

reg = flox.SymbolRegistry()
sym = reg.add_symbol("test", "BTC", 0.01)
signals = []
runner = flox.Runner(reg, lambda s: signals.append(s), threaded=False)

class S(flox.Strategy):
    def on_trade(self, ctx, trade):
        self.emit_market_buy(sym, 1.0)

class Gate(flox.RiskManager):
    def allow(self, sig):
        raise RuntimeError("GATE_BLEW_UP")

runner.add_strategy(S(symbols=[sym]))
runner.set_risk_manager(Gate())
runner.start()
runner.on_trade(sym, 100.0, 1.0, True, 1_000)
runner.stop()
flox.set_log_callback(None)
print("SIGNALS", len(signals))
"""


def test_a_log_sink_that_raises_still_leaves_the_description_behind():
    """A broken sink costs the message its destination, not its existence.

    callback_guard.h reports behind a thread-local flag, because the sink
    is a Python callable too: without it, a sink that raises reports
    itself through the same path, without bound. And a report that could
    not be delivered goes to stderr rather than disappearing.

    Both are pinned here. The description of the *original* failure --
    the gate, not the sink -- has to be on stderr, the gate still has to
    deny, the process has to exit cleanly, and the output has to stay
    small: unbounded recursion shows up as a crash, a timeout, or
    megabytes of stderr.
    """
    done = _run(_RAISING_SINK_SCRIPT)

    assert done.returncode == 0, (
        f"exit was {done.returncode} with a log sink that raises\n"
        f"{done.stdout}{done.stderr}")
    assert "SIGNALS 0" in done.stdout, (
        f"the gate still had to deny\n{done.stdout}{done.stderr}")
    assert "GATE_BLEW_UP" in done.stderr, (
        "the original description was lost when the sink raised; it has to "
        f"fall back to stderr\n{done.stderr}")
    assert "SINK_BLEW_UP" in done.stderr, (
        f"the sink's own failure went unreported\n{done.stderr}")
    assert len(done.stderr) < 20_000, (
        f"{len(done.stderr)} bytes of stderr -- the report recursed through "
        f"the raising sink instead of stopping at the first bounce\n"
        f"{done.stderr[:2000]}")
