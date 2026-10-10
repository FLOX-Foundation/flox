"""The deprecated flox-py and flox-mcp packages keep old code running.

scripts/build_compat_shims.sh builds both shims; each is installed into a
scratch venv next to the package it forwards to, and code written against
the old import name has to work there and say it is deprecated.

The lrvx wheel is the one named by LRVX_WHEEL when that is set. Otherwise
a wheel is packed from the in-tree build (build/python/lrvx: the pure-Python
package plus the compiled extension, which is what the published wheel
ships), so the test needs no C++ rebuild. lrvx-mcp is packed from
mcp/lrvx_mcp the same way. Both install with --no-deps; numpy, the one
runtime dependency the checks touch, comes from this interpreter.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import subprocess
import sys
import venv
import zipfile
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]


def _version(pyproject: Path) -> str:
    return re.search(r'^version = "([^"]+)"', pyproject.read_text(), re.M).group(1)


def _pack_wheel(package_dir: Path, dist: str, version: str, out: Path) -> Path:
    info = f"{dist}-{version}.dist-info"
    wheel = out / f"{dist}-{version}-py3-none-any.whl"
    records = []
    with zipfile.ZipFile(wheel, "w", zipfile.ZIP_DEFLATED) as zf:
        def add(name: str, data: bytes) -> None:
            zf.writestr(name, data)
            digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest())
            records.append(f"{name},sha256={digest.rstrip(b'=').decode()},{len(data)}")

        for f in sorted(package_dir.rglob("*")):
            if f.is_file() and "__pycache__" not in f.parts:
                add(f"{package_dir.name}/{f.relative_to(package_dir).as_posix()}", f.read_bytes())
        add(f"{info}/METADATA",
            f"Metadata-Version: 2.1\nName: {dist}\nVersion: {version}\n".encode())
        add(f"{info}/WHEEL",
            b"Wheel-Version: 1.0\nGenerator: test_compat_shims\n"
            b"Root-Is-Purelib: false\nTag: py3-none-any\n")
        records.append(f"{info}/RECORD,,")
        zf.writestr(f"{info}/RECORD", "\n".join(records) + "\n")
    return wheel


def _lrvx_wheel(out: Path) -> Path:
    given = os.environ.get("LRVX_WHEEL")
    if given:
        return Path(given)
    for cand in ("build/python", "build-py312/python"):
        pkg = REPO_ROOT / cand / "lrvx"
        if any(pkg.glob("_lrvx*.so")) or any(pkg.glob("_lrvx*.pyd")):
            return _pack_wheel(pkg, "lrvx", _version(REPO_ROOT / "pyproject.toml"), out)
    pytest.skip("no compiled lrvx under build/python and LRVX_WHEEL is not set")


@pytest.fixture(scope="module")
def env(tmp_path_factory):
    work = tmp_path_factory.mktemp("compat-shims")
    shims = work / "shims"
    subprocess.run(["bash", str(REPO_ROOT / "scripts" / "build_compat_shims.sh"), str(shims)],
                   check=True, capture_output=True, text=True,
                   env=dict(os.environ, PYTHON=sys.executable))
    wheels = work / "wheels"
    wheels.mkdir()
    lrvx_mcp = _pack_wheel(REPO_ROOT / "mcp" / "lrvx_mcp", "lrvx_mcp",
                             _version(REPO_ROOT / "mcp" / "pyproject.toml"), wheels)

    root = work / "venv"
    venv.EnvBuilder(with_pip=True).create(root)
    py = root / "bin" / "python"
    site = Path(subprocess.run(
        [str(py), "-c", "import sysconfig; print(sysconfig.get_paths()['purelib'])"],
        check=True, capture_output=True, text=True).stdout.strip())
    import numpy
    (site / "_test_numpy.pth").write_text(str(Path(numpy.__file__).resolve().parents[1]) + "\n")
    subprocess.run(
        [str(py), "-m", "pip", "install", "--quiet", "--no-deps", "--no-index",
         str(_lrvx_wheel(wheels)), str(lrvx_mcp),
         *map(str, sorted(shims.glob("*.whl")))],
        check=True, capture_output=True, text=True)
    return {"py": py, "bin": root / "bin", "shims": shims, "cwd": work}


def _run(env, code: str) -> dict:
    out = subprocess.run([str(env["py"]), "-c", code], check=True, capture_output=True,
                         text=True, cwd=env["cwd"])
    return json.loads(out.stdout.strip().splitlines()[-1])


_FLOX_PY = r'''
import json, sys, warnings
with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter("always")
    import flox_py
    import flox_py
import lrvx, lrvx.cli, lrvx.archives.binance
import flox_py.cli, flox_py.archives.binance
from flox_py import SymbolRegistry
from flox_py.tape import inspect_tape
sid = flox_py.SymbolRegistry().add_symbol("bybit", "BTCUSDT", 0.01)
print(json.dumps({
    "warnings": [(w.category.__name__, str(w.message)) for w in caught],
    "same_class": SymbolRegistry is lrvx.SymbolRegistry,
    "same_cli": flox_py.cli is lrvx.cli and sys.modules["flox_py.cli"] is lrvx.cli,
    "same_nested": flox_py.archives.binance is lrvx.archives.binance,
    "same_function": inspect_tape is lrvx.tape.inspect_tape,
    "in_all": "SymbolRegistry" in flox_py.__all__ and "FloxError" in flox_py.__all__,
    "flox_error": flox_py.FloxError is lrvx.LrvxError,
    "version": flox_py.__version__,
    "symbol_added": sid is not None,
    "lrvx_installed": "site-packages" in lrvx.__file__,
}))
'''


def test_flox_py_reexports_lrvx_and_warns_once(env):
    got = _run(env, _FLOX_PY)
    assert got["warnings"] == [
        ["DeprecationWarning", "flox-py is renamed to lrvx; install lrvx and import lrvx"]]
    assert got["same_class"] and got["same_cli"] and got["same_nested"] and got["same_function"]
    assert got["in_all"] and got["symbol_added"] and got["lrvx_installed"]
    assert got["flox_error"]
    assert got["version"] == _version(REPO_ROOT / "pyproject.toml")


def test_flox_command_says_it_is_renamed_and_runs_lrvx(env):
    out = subprocess.run([str(env["bin"] / "flox"), "--help"], capture_output=True,
                         text=True, cwd=env["cwd"])
    assert out.returncode == 0, out.stderr
    assert "flox-py is renamed to lrvx" in out.stderr
    assert "new" in out.stdout and "tape" in out.stdout


def test_flox_mcp_reexports_lrvx_mcp_and_warns(env):
    got = _run(env, r'''
import json, warnings
with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter("always")
    import flox_mcp
import lrvx_mcp, lrvx_mcp.tools.lookup
import flox_mcp.tools.lookup
print(json.dumps({
    "warnings": [w.category.__name__ for w in caught],
    "same_module": flox_mcp.tools.lookup is lrvx_mcp.tools.lookup,
    "version": flox_mcp.__version__ == lrvx_mcp.__version__,
}))
''')
    assert got["warnings"] == ["DeprecationWarning"]
    assert got["same_module"] and got["version"]
    # `init --print` is the one verb that needs no MCP SDK; it writes nothing.
    out = subprocess.run([str(env["bin"] / "flox-mcp"), "init", "--print"],
                         capture_output=True, text=True, cwd=env["cwd"])
    assert out.returncode == 0, out.stderr
    assert "flox-mcp is renamed to lrvx-mcp" in out.stderr
    assert "mcpServers" in json.loads(out.stdout)


def test_shims_pin_the_version_this_tree_builds(env):
    for wheel, dist, pyproject in (
            ("flox_py-*.whl", "lrvx", REPO_ROOT / "pyproject.toml"),
            ("flox_mcp-*.whl", "lrvx-mcp", REPO_ROOT / "mcp" / "pyproject.toml")):
        path = next(env["shims"].glob(wheel))
        with zipfile.ZipFile(path) as zf:
            meta = zf.read(next(n for n in zf.namelist() if n.endswith("METADATA"))).decode()
        assert f"Requires-Dist: {dist}=={_version(pyproject)}" in meta
        assert any(env["shims"].glob(wheel.replace("-*.whl", "-*.tar.gz")))
