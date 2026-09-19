#!/usr/bin/env python3
"""An installed header may only need what its target declares.

`install(DIRECTORY include/)` ships every header as part of what a consumer
gets from find_package. If one of them includes an external dependency the
target does not link, the consumer finds out at their own link step, with
undefined symbols and no indication of the cause. That is what
venue/include/flox-venue/tls_gateway.h did: it included <openssl/ssl.h> while
flox-venue linked no OpenSSL, and OpenSSL was found inside the tests block and
attached to one test by name.

Why this is a static check and not a link test: the header is entirely inline
functions. Including it emits nothing, so a translation unit that includes it
and links against the target alone succeeds whether or not the dependency is
declared -- the first version of this gate was exactly that test, and the
mutation that removes the declaration passed it green.

The set of external dependencies is derived from the headers, not listed here:
anything included with <> that is neither the standard library nor this
project's own is external, and its root has to appear in what the target
links.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

MODULES = [
    # (installed header root, the CMakeLists that declares that target)
    (ROOT / "venue/include", ROOT / "venue/CMakeLists.txt", "flox-venue"),
    (ROOT / "include", ROOT / "CMakeLists.txt", "${FLOX}"),
]

# Headers the install() rules leave out when their optional dependency is off.
# Derived from the same reason they are excluded: a header that is not shipped
# cannot hand anybody an undeclared dependency. Kept short and checked against
# the CMakeLists below, so it cannot quietly grow into a list of exemptions.
EXCLUDED_WHEN_OFF = {
    "flox/ml/onnx_inference.h": "FLOX_ENABLE_ONNX",
    "flox-venue/tls_gateway.h": "FLOX_VENUE_TLS",
}

# Includes that need no declaration: the standard library (no slash, no
# extension, or <c...>), and this project's own prefixes.
OWN_PREFIXES = ("flox/", "flox-venue/", "flox-connectors/")


def is_external(inc: str) -> bool:
    if inc.startswith(OWN_PREFIXES):
        return False
    if "/" not in inc and "." not in inc:
        return False  # <vector>, <string>, ...
    if "/" not in inc and inc.startswith("c") and "." not in inc:
        return False
    if re.fullmatch(r"c[a-z]+", inc):
        return False
    # <sys/socket.h>, <netinet/in.h> and friends are the platform, not a
    # package to link.
    if inc.split("/")[0] in {
        "sys", "netinet", "arpa", "net", "linux", "bits", "mach", "windows",
    }:
        return False
    if inc in {
        "unistd.h", "fcntl.h", "poll.h", "netdb.h", "dlfcn.h", "pthread.h",
        "sched.h", "signal.h", "time.h", "errno.h", "stdint.h", "stddef.h",
        "stdlib.h", "string.h", "stdio.h", "immintrin.h", "arm_neon.h",
        "x86intrin.h", "emmintrin.h", "winsock2.h", "ws2tcpip.h", "io.h",
        "intrin.h", "malloc.h", "numa.h", "sysinfoapi.h", "processthreadsapi.h",
        "windows.h", "memoryapi.h", "synchapi.h", "profileapi.h",
    }:
        return False
    return True


def root_of(inc: str) -> str:
    return inc.split("/")[0].removesuffix(".h").removesuffix(".hpp")


def main() -> int:
    failures = []
    for include_dir, cmake, target in MODULES:
        if not include_dir.is_dir() or not cmake.exists():
            continue
        declared = " ".join(
            line
            for line in cmake.read_text(encoding="utf-8").splitlines()
            if "target_link_libraries" in line or "PUBLIC" in line
        ).lower()
        cmake_text = cmake.read_text(encoding="utf-8")
        for header in sorted(include_dir.rglob("*.h")):
            rel = str(header.relative_to(include_dir))
            flag = EXCLUDED_WHEN_OFF.get(rel)
            if flag is not None:
                # It is only exempt if the install really does leave it out.
                if f"PATTERN \"{header.name}\" EXCLUDE" not in cmake_text:
                    failures.append(
                        f"{header.relative_to(ROOT)} is listed as excluded when "
                        f"{flag} is off, but {cmake.relative_to(ROOT)} installs it anyway"
                    )
                continue
            # Only UNCONDITIONAL includes count. A header that reaches for a
            # dependency behind #if has already said the dependency is
            # optional, and the build that turns the feature on is the build
            # that has to declare it. The defect this gate exists for --
            # tls_gateway.h and <openssl/ssl.h> -- was unconditional.
            depth = 0
            for line in header.read_text(encoding="utf-8", errors="replace").splitlines():
                stripped = line.lstrip()
                if re.match(r"#\s*(if|ifdef|ifndef)\b", stripped):
                    depth += 1
                    continue
                if re.match(r"#\s*endif\b", stripped):
                    depth = max(0, depth - 1)
                    continue
                if depth > 0:
                    continue
                m = re.match(r'#\s*include\s*<([^>]+)>', stripped)
                if not m:
                    continue
                inc = m.group(1)
                if not is_external(inc):
                    continue
                root = root_of(inc).lower()
                if root not in declared:
                    failures.append(
                        f"{header.relative_to(ROOT)} includes <{inc}> but "
                        f"{cmake.relative_to(ROOT)} does not link anything named "
                        f"{root!r} to {target}"
                    )
    if failures:
        print("[installed-headers] a shipped header needs what its target does not declare:")
        for f in sorted(set(failures)):
            print(f"  {f}")
        return 1
    print("[installed-headers] every external include of a shipped header is declared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
