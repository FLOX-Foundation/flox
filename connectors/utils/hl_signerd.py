# Flox Engine
# Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
#
# Copyright (c) 2025 FLOX Foundation
# Licensed under the MIT License. See LICENSE file in the project root for full
# license information.

#!/usr/bin/env python3
"""Hyperliquid signing daemon.

The request body carries a raw private key, so the only transport this
daemon offers is a Unix socket no other user on the box can open. It has
never listened on TCP and must not start: loopback authenticates neither
end, and whoever binds the port first harvests the key.
"""
import os, sys, json, struct, socket, traceback

# Imported lazily-ish: the listener setup below has to stay importable
# without the signing SDK installed so it can be tested on its own.
try:
    from hyperliquid.utils.signing import sign_l1_action
    from eth_account import Account
except ImportError as exc:  # pragma: no cover - exercised by main()
    sign_l1_action = None
    Account = None
    _IMPORT_ERROR = exc
else:
    _IMPORT_ERROR = None

DEFAULT_SOCK = "/dev/shm/hl_sign.sock"
SOCK_ENV = "FLOX_HL_SIGNER_SOCKET"

# A signing request is a few hundred bytes. The length header comes from the
# peer, so it is a request for an allocation, not a fact.
MAX_REQUEST = 1 << 20

def socket_path():
    return os.environ.get(SOCK_ENV) or DEFAULT_SOCK

def create_listener(path, backlog=128):
    """Bind and listen on `path`, private to this user for its whole life.

    bind() creates the socket node with 0777 & ~umask, so chmod-after-bind
    leaves a window in which the node exists with whatever the ambient umask
    allowed. The umask is narrowed around bind() instead, and the chmod stays
    as a belt-and-braces assertion; both run before listen(), so no client can
    connect until the mode is right.
    """
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    old_umask = os.umask(0o177)
    try:
        srv.bind(path)
    finally:
        os.umask(old_umask)
    os.chmod(path, 0o600)
    srv.listen(backlog)
    return srv

def recv_all(fd, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = fd.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return bytes(buf)

def recv_msg(fd):
    hdr = recv_all(fd, 4)
    (n,) = struct.unpack("!I", hdr)
    if n == 0 or n > MAX_REQUEST:
        raise ValueError("request length %d out of bounds" % n)
    return recv_all(fd, n)

def send_msg(fd, b):
    fd.sendall(struct.pack("!I", len(b)))
    fd.sendall(b)

def handle(req_bytes):
    req = json.loads(req_bytes.decode("utf-8"))
    pk = req["private_key"]
    if pk.startswith(("0x", "0X")): pk = pk[2:]
    wallet = Account.from_key(bytes.fromhex(pk))
    sig = sign_l1_action(
        wallet=wallet,
        action=json.loads(req["action_json"]),
        active_pool=req.get("active_pool"),
        nonce=int(req["nonce"]),
        expires_after=req.get("expires_after"),
        is_mainnet=bool(req.get("is_mainnet", True)),
    )
    r = (getattr(sig, "r", None) or sig["r"]).lower()
    s = (getattr(sig, "s", None) or sig["s"]).lower()
    v = int(getattr(sig, "v", None) or sig["v"])
    if not r.startswith("0x"): r = "0x"+r
    if not s.startswith("0x"): s = "0x"+s
    return json.dumps({"r": r, "s": s, "v": v}).encode("utf-8")

def main():
    if _IMPORT_ERROR is not None:
        raise SystemExit("hl_signerd needs the hyperliquid SDK and eth_account: %s" % _IMPORT_ERROR)

    path = socket_path()
    srv = create_listener(path)
    try:
        while True:
            fd, _ = srv.accept()
            try:
                req = recv_msg(fd)
                resp = handle(req)
                send_msg(fd, resp)
            except Exception:
                # The traceback goes to the operator, not down the socket: the
                # client bounds the reply it will accept, and a stack trace is
                # both larger than that bound and more than the caller needs.
                traceback.print_exc(file=sys.stderr)
                send_msg(fd, json.dumps({"error": "signing failed"}).encode())
            finally:
                fd.close()
    finally:
        try: os.unlink(path)
        except FileNotFoundError: pass

if __name__ == "__main__":
    main()
