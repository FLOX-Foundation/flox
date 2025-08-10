# Flox Engine
# Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
#
# Copyright (c) 2025 FLOX Foundation
# Licensed under the MIT License. See LICENSE file in the project root for full
# license information.

#!/usr/bin/env python3
import os, json, struct, socket, traceback
from hyperliquid.utils.signing import sign_l1_action
from eth_account import Account

SOCK = "/dev/shm/hl_sign.sock"

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
    try:
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(SOCK)
        os.chmod(SOCK, 0o600)
        srv.listen(128)

        while True:
            fd, _ = srv.accept()
            try:
                req = recv_msg(fd)
                resp = handle(req)
                send_msg(fd, resp)
            except Exception:
                tb = traceback.format_exc().encode()
                send_msg(fd, json.dumps({"error": tb.decode()}).encode())
            finally:
                fd.close()
    finally:
        try: os.unlink(SOCK)
        except FileNotFoundError: pass

if __name__ == "__main__":
    main()
