#!/bin/bash
set -e

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

# shellcheck source=scripts/clang-format-version.sh
. scripts/clang-format-version.sh
if ! clang_format_version_check; then
  exit 2
fi

FILES=$(git ls-files '*.cpp' '*.h' | grep -v '^third_party/')
if [ -z "$FILES" ]; then
  echo "[check-format] No C++ source files found to check."
  exit 0
fi

echo "[check-format] Checking formatting with clang-format ${actual} on tracked files..."
if clang-format --dry-run --Werror $FILES; then
  echo "[check-format] All files are properly formatted."
else
  echo "[check-format] Found improperly formatted files."
  exit 1
fi
