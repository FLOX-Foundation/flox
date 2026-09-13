# shellcheck shell=sh
#
# Shared clang-format version pin. Sourced by scripts/check-format.sh and
# scripts/pre-commit (and read by .github/workflows/ci.yml) so there is
# exactly one place this version lives.
#
# Pinned to the PyPI `clang-format` wheel, which ships prebuilt,
# byte-identical binaries per version across macOS and Linux:
# `pip install "clang-format==${CLANG_FORMAT_REQUIRED_VERSION}"`. CI
# installs the exact same wheel instead of whatever the distro currently
# ships, so the baseline can't drift out from under this file without
# both changing together in the same commit.
#
# The version matters because clang-format's output is NOT stable across
# major releases. 18 and 21 disagree on spacing around placement new
# (`::new(ptr)` vs `::new (ptr)`), among other rules. A mismatched local
# install makes formatting checks meaningless: green here, red in CI, on
# a file the contributor never touched -- and worse, scripts/pre-commit's
# `clang-format -i` will silently rewrite files into the wrong style on
# every commit, since it trusts whatever binary is on PATH.
CLANG_FORMAT_REQUIRED_VERSION="18.1.8"
CLANG_FORMAT_REQUIRED_MAJOR="${CLANG_FORMAT_REQUIRED_VERSION%%.*}"

# Prints a diagnostic and returns non-zero when the clang-format on PATH
# is missing or is not CLANG_FORMAT_REQUIRED_MAJOR. Callers decide what to
# do with a non-zero return (fail the check, refuse to auto-format, ...).
clang_format_version_check() {
  if ! command -v clang-format >/dev/null 2>&1; then
    echo "[clang-format] not found on PATH." >&2
    echo "[clang-format] Install the pinned version: pip install \"clang-format==${CLANG_FORMAT_REQUIRED_VERSION}\"" >&2
    return 2
  fi

  actual=$(clang-format --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
  actual_major="${actual%%.*}"

  if [ "$actual_major" != "$CLANG_FORMAT_REQUIRED_MAJOR" ]; then
    echo "[clang-format] version mismatch: found ${actual:-unknown}, need major version ${CLANG_FORMAT_REQUIRED_MAJOR} (pinned: ${CLANG_FORMAT_REQUIRED_VERSION})." >&2
    echo "[clang-format] Different major versions format code differently (e.g. placement new: '::new(ptr)' vs '::new (ptr)')," >&2
    echo "[clang-format] so a mismatched local install passes here and fails in CI -- or, in the pre-commit hook, silently rewrites files into the wrong style on every commit." >&2
    echo "[clang-format] Install the pinned version: pip install \"clang-format==${CLANG_FORMAT_REQUIRED_VERSION}\"" >&2
    return 2
  fi

  return 0
}
