"""FLOX Python bindings.

The compiled pybind11 extension lives at `flox_py._flox_py` and is
re-exported here so users can `import flox_py` and access the full
public surface (indicators, engine, backtest, replay, etc.) at the
top level. Type information is shipped via `flox_py/__init__.pyi`
and the PEP 561 marker `flox_py/py.typed`.
"""
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
