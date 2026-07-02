"""FLOX Python bindings.

The compiled pybind11 extension lives at `flox_py._flox_py` and is
re-exported here so users can `import flox_py` and access the full
public surface (indicators, engine, backtest, replay, etc.) at the
top level. Type information is shipped via `flox_py/__init__.pyi`
and the PEP 561 marker `flox_py/py.typed`.
"""
# The `_flox_py/` directory next to this file holds only typing stubs
# (*.pyi, no __init__.py). When the compiled extension was built for a
# different Python version its ABI tag does not match, the import system
# falls back to that stub directory as an empty namespace package, and
# every later attribute access fails with a confusing AttributeError.
# Detect the fallback (namespace packages have no __file__) and fail
# loudly with the actual cause instead.
from . import _flox_py as _native
if getattr(_native, "__file__", None) is None:
    import sys as _sys
    raise ImportError(
        "flox_py: the compiled extension `flox_py._flox_py` is not "
        f"importable under Python {_sys.version_info.major}."
        f"{_sys.version_info.minor}; the typing-stub directory was "
        "resolved instead. The native module was most likely built for "
        "a different Python version. Rebuild the bindings with this "
        "interpreter, or run with the interpreter they were built for."
    )
del _native

from ._flox_py import *  # noqa: F401,F403  # also brings in the `targets` submodule

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
