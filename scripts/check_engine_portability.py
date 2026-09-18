#!/usr/bin/env python3
"""The venue engine must not reach for POSIX behind the perimeter's back.

`FLOX_VENUE_PERIMETER=OFF` is supposed to leave a module that builds where
POSIX sockets do not. Whether it does was, until this script, answered only
by a Windows CI job eight minutes into a run -- and answered one header at a
time, because the compiler stops at the first one.

This walks what each engine test actually includes, follows the chain through
flox and flox-venue headers, and reports every POSIX include that is not
inside a platform conditional. It answers the same question in about a
second, and it answers all of it at once.

A POSIX include under `#ifdef __linux__` (or any platform test) is fine: the
point is not to forbid POSIX, it is to forbid reaching for it unconditionally
in code that claims to be portable.
"""
import os
import re
import subprocess
import sys

ROOTS = ["include", "venue/include"]
INCLUDE = re.compile(r'#include\s+[<"]((?:flox|flox-venue)/[^">]+)[>"]')
POSIX = re.compile(
    r"#include\s+<(unistd|sys/socket|netinet/in|netinet/tcp|arpa/inet|poll|sys/wait|"
    r"fcntl|sys/mman|sys/time|netdb|termios|dirent)\.h>"
)
GUARD_OPEN = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b")
GUARD_END = re.compile(r"^\s*#\s*endif\b")
PLATFORM = re.compile(r"_WIN32|__linux__|__APPLE__|__unix__|POSIX|__has_include")

# Headers the perimeter owns. A test that includes one of these is excluded
# from the engine-only build, so its POSIX use is not this script's business.
PERIMETER = {
    "socket_acceptor.h", "tcp_gateway.h", "ws_gateway.h", "tls_gateway.h",
    "udp_multicast.h", "session.h", "session_registry.h", "cancel_on_disconnect.h",
    "fix_session.h", "md_distribution.h", "md_recovery.h", "control_server.h",
    "control_api.h", "metrics_server.h", "rest_json.h",
}


def resolve(inc):
    for root in ROOTS:
        path = os.path.join(root, inc)
        if os.path.isfile(path):
            return path
    return None


def unguarded(path):
    """POSIX includes in `path` that sit outside any platform conditional."""
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    except OSError:
        return []
    hits, stack = [], []
    for line in lines:
        if GUARD_OPEN.match(line):
            stack.append(bool(PLATFORM.search(line)))
        elif GUARD_END.match(line) and stack:
            stack.pop()
        m = POSIX.search(line)
        if m and not any(stack):
            hits.append(m.group(1))
    return hits


def walk(path, seen, chain):
    if path in seen:
        return []
    seen.add(path)
    found = [(path, h, chain) for h in unguarded(path)]
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return found
    for m in INCLUDE.finditer(text):
        target = resolve(m.group(1))
        if target:
            found += walk(target, seen, chain + [os.path.basename(path)])
    return found


def is_perimeter_test(src):
    try:
        text = open(src, encoding="utf-8", errors="replace").read()
    except OSError:
        return False
    return any(f"flox-venue/{h}" in text for h in PERIMETER)


def main():
    tests = sorted(
        p for p in os.listdir("venue/tests")
        if p.startswith("test_") and p.endswith(".cpp")
    )
    engine = [os.path.join("venue/tests", t) for t in tests
              if not is_perimeter_test(os.path.join("venue/tests", t))]
    bad = {}
    for src in engine:
        hits = walk(src, set(), [])
        if hits:
            bad[src] = sorted({(os.path.basename(p), h, " -> ".join(c[1:] + [os.path.basename(p)]))
                               for p, h, c in hits})
    if not bad:
        print(f"[engine-portability] {len(engine)} engine tests reach no POSIX header "
              f"outside a platform conditional")
        return 0
    for src, hits in sorted(bad.items()):
        for header, inc, chain in hits:
            print(f"::error file={header}::<{inc}.h> is included unconditionally "
                  f"and reaches the engine-only build via {src}: {chain}")
    print(f"[engine-portability] {len(bad)} engine test(s) pull POSIX unconditionally. "
          f"Either guard the include, or -- if the header belongs to the perimeter -- "
          f"add it to PERIMETER here and to FLOX_VENUE_PERIMETER_HEADERS in "
          f"venue/CMakeLists.txt.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
