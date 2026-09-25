"""python/tests/test_hl_signerd_socket.py

The Hyperliquid signing daemon (``connectors/utils/hl_signerd.py``) receives
a raw private key in every request, so the socket it listens on is the whole
of its access control. Two properties are pinned here:

* the socket node is private to its owner from the moment it exists.
  ``bind()`` creates it with ``0777 & ~umask``; the daemon used to chmod it
  afterwards, which leaves a window where the node is already on the
  filesystem with whatever the ambient umask allowed. The umask is narrowed
  around ``bind()`` instead, which is what the chmod-suppressed case below
  checks -- with the fix the mode is right without the chmod, without it the
  socket is born world-writable.
* nothing in the daemon speaks TCP. Loopback authenticates neither end.

Run from repo root:
    python3 -m pytest python/tests/test_hl_signerd_socket.py
"""
from __future__ import annotations

import importlib.util
import os
import shutil
import socket
import stat
import struct
import tempfile
from pathlib import Path

import pytest

DAEMON = Path(__file__).resolve().parents[2] / "connectors" / "utils" / "hl_signerd.py"


def _load():
    spec = importlib.util.spec_from_file_location("hl_signerd_under_test", DAEMON)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def daemon():
    return _load()


@pytest.fixture
def sock_dir():
    # pytest's tmp_path is long enough on macOS to overflow sun_path (104
    # bytes there), which every AF_UNIX bind below would trip over.
    directory = tempfile.mkdtemp(prefix="flox-sd-")
    try:
        yield Path(directory)
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def test_socket_is_private_without_the_chmod(daemon, sock_dir, monkeypatch):
    monkeypatch.setattr(daemon.os, "chmod", lambda *a, **kw: None)
    old_umask = os.umask(0o000)
    try:
        srv = daemon.create_listener(str(sock_dir / "sign.sock"))
    finally:
        os.umask(old_umask)

    try:
        mode = stat.S_IMODE(os.stat(sock_dir / "sign.sock").st_mode)
        assert mode == 0o600, "bind() created a socket other local users can open"
    finally:
        srv.close()


def test_listener_is_a_unix_socket(daemon, sock_dir):
    srv = daemon.create_listener(str(sock_dir / "sign.sock"))
    try:
        assert srv.family == socket.AF_UNIX
        assert stat.S_ISSOCK(os.stat(sock_dir / "sign.sock").st_mode)
    finally:
        srv.close()


def test_stale_socket_file_is_replaced(daemon, sock_dir):
    path = sock_dir / "sign.sock"
    path.write_text("left over from a crash")
    srv = daemon.create_listener(str(path))
    try:
        assert stat.S_ISSOCK(os.stat(path).st_mode)
    finally:
        srv.close()


def test_request_length_is_bounded(daemon):
    left, right = socket.socketpair()
    try:
        right.sendall(struct.pack("!I", daemon.MAX_REQUEST + 1))
        with pytest.raises(ValueError):
            daemon.recv_msg(left)
    finally:
        left.close()
        right.close()


def test_daemon_never_binds_tcp():
    source = DAEMON.read_text()
    for token in ("AF_INET", "AF_INET6", "SOCK_DGRAM", "19847"):
        assert token not in source, f"{token} has no place in a private-key signer"
