"""FLOX Python bindings.

The compiled pybind11 extension lives at `flox_py._flox_py` and is
re-exported here so users can `import flox_py` and access the full
public surface (indicators, engine, backtest, replay, etc.) at the
top level. Type information is shipped via `flox_py/__init__.pyi`
and the PEP 561 marker `flox_py/py.typed`.
"""
# The `_flox_py/` directory next to this file holds only typing stubs
# (*.pyi, no __init__.py). When the compiled extension is absent — a
# source-tree import, or a build made for a different Python version
# whose ABI tag does not match — the import system falls back to that
# stub directory as an empty namespace package. Pure-Python surfaces
# (flox_py.cli, flox_py.lookahead, ...) are expected to work in that
# degraded mode, so the package still imports; but any access to a
# native name must raise an ImportError naming the actual cause rather
# than a bare AttributeError.
from . import _flox_py as _native
_NATIVE_AVAILABLE = getattr(_native, "__file__", None) is not None
del _native

if _NATIVE_AVAILABLE:
    from ._flox_py import *  # noqa: F401,F403  # also brings in the `targets` submodule
else:
    import sys as _sys
    _NATIVE_IMPORT_ERROR = (
        "flox_py: the compiled extension `flox_py._flox_py` is not "
        f"importable under Python {_sys.version_info.major}."
        f"{_sys.version_info.minor}; the typing-stub directory was "
        "resolved instead. Either this is a source-tree import (only "
        "pure-Python submodules like flox_py.cli work here), or the "
        "native module was built for a different Python version. "
        "Rebuild the bindings with this interpreter, or run with the "
        "interpreter they were built for."
    )
    del _sys

    def __getattr__(name):  # PEP 562: fires only for names not found above
        # `from flox_py import cli` resolves through this hook before
        # the import machinery falls back to a submodule import, so
        # pure-Python submodules must be served here; only names with
        # no submodule behind them are genuinely native and get the
        # explanatory error.
        import importlib
        try:
            return importlib.import_module(f".{name}", __name__)
        except ModuleNotFoundError:
            raise ImportError(
                f"{_NATIVE_IMPORT_ERROR} (while accessing flox_py.{name})"
            ) from None

# Pure-Python research surfaces that sit on top of the compiled bindings.
from .live_queue_calibrator import (  # noqa: F401
    CalibrationResult,
    LiveQueueCalibrator,
    TestOrderHelper,
)
from .custom_venue import (  # noqa: F401
    CustomVenue,
    assemble_custom_venue,
)
