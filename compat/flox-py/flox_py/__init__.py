"""flox-py is renamed to lrvx.

This package keeps code written against ``flox_py`` running: it re-exports
everything ``lrvx`` exports, and ``flox_py.<sub>`` resolves to the very
same module object as ``lrvx.<sub>``. Install ``lrvx`` and write
``import lrvx`` instead.
"""
import importlib
import importlib.abc
import importlib.util
import sys
import warnings

warnings.warn("flox-py is renamed to lrvx; install lrvx and import lrvx",
              DeprecationWarning, stacklevel=2)

import lrvx as _lrvx  # noqa: E402
from lrvx import *  # noqa: E402,F401,F403

# The exception the native module raises, under the name FLOX code catches.
FloxError = _lrvx.LrvxError

__all__ = sorted({n for n in dir(_lrvx) if not n.startswith("_")} | {"FloxError"})

try:
    __version__ = _lrvx.__version__
except AttributeError:
    try:
        from importlib.metadata import version as _dist_version
        __version__ = _dist_version("lrvx")
    except Exception:
        __version__ = "unknown"


def __getattr__(name):
    return getattr(_lrvx, name)


class _Alias(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    """Serves ``flox_py.<sub>`` as the already-imported ``lrvx.<sub>``.

    Sits first on ``sys.meta_path`` so the path finder never loads a second
    copy of a lrvx module under the old name."""

    _prefix = __name__ + "."

    def find_spec(self, fullname, path=None, target=None):
        if not fullname.startswith(self._prefix):
            return None
        try:
            found = importlib.util.find_spec("lrvx." + fullname[len(self._prefix):])
        except ModuleNotFoundError:
            return None
        if found is None:
            return None
        return importlib.util.spec_from_loader(fullname, self)

    def create_module(self, spec):
        return importlib.import_module("lrvx." + spec.name[len(self._prefix):])

    def exec_module(self, module):
        pass


if not any(isinstance(f, _Alias) for f in sys.meta_path):
    sys.meta_path.insert(0, _Alias())
