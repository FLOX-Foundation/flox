#!/bin/bash
set -e

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

FILES=$(git ls-files '*.cpp' '*.h')
if [ -z "$FILES" ]; then
  echo "[check-format] No C++ source files found to check."
  exit 0
fi

echo "[check-format] Checking formatting on tracked files..."
if clang-format --dry-run --Werror $FILES; then
  echo "[check-format] All files are properly formatted."
else
  echo "[check-format] Found improperly formatted files."
  exit 1
fi
