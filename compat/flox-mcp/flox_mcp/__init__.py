"""flox-mcp is renamed to lrvx-mcp.

This package keeps code written against ``flox_mcp`` running: it re-exports
everything ``lrvx_mcp`` exports, and ``flox_mcp.<sub>`` resolves to the
very same module object as ``lrvx_mcp.<sub>``. Install ``lrvx-mcp`` and
write ``import lrvx_mcp`` instead.
"""
import importlib
import importlib.abc
import importlib.util
import sys
import warnings

warnings.warn("flox-mcp is renamed to lrvx-mcp; install lrvx-mcp and "
              "import lrvx_mcp", DeprecationWarning, stacklevel=2)

import lrvx_mcp as _lrvx_mcp  # noqa: E402
from lrvx_mcp import *  # noqa: E402,F401,F403

__all__ = sorted(n for n in dir(_lrvx_mcp) if not n.startswith("_"))

try:
    __version__ = _lrvx_mcp.__version__
except AttributeError:
    __version__ = "unknown"


def __getattr__(name):
    return getattr(_lrvx_mcp, name)


class _Alias(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    """Serves ``flox_mcp.<sub>`` as the already-imported ``lrvx_mcp.<sub>``.

    Sits first on ``sys.meta_path`` so the path finder never loads a second
    copy of a lrvx_mcp module under the old name."""

    _prefix = __name__ + "."

    def find_spec(self, fullname, path=None, target=None):
        if not fullname.startswith(self._prefix):
            return None
        try:
            found = importlib.util.find_spec("lrvx_mcp." + fullname[len(self._prefix):])
        except ModuleNotFoundError:
            return None
        if found is None:
            return None
        return importlib.util.spec_from_loader(fullname, self)

    def create_module(self, spec):
        return importlib.import_module("lrvx_mcp." + spec.name[len(self._prefix):])

    def exec_module(self, module):
        pass


if not any(isinstance(f, _Alias) for f in sys.meta_path):
    sys.meta_path.insert(0, _Alias())
