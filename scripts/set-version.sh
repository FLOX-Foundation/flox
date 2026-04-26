#!/usr/bin/env bash
# Synchronise the project version across every artifact that publishes a version:
#   - root CMakeLists.txt   (flox C++ core)
#   - python/pyproject.toml (flox-py PyPI package)
#   - node/package.json     (@flox-foundation/flox npm package)
#
# Usage: scripts/set-version.sh <version>
#        scripts/set-version.sh 0.5.3
set -euo pipefail

VERSION="${1:-}"
if [[ -z "$VERSION" ]]; then
  echo "usage: $0 <version>  (e.g. 0.5.3)" >&2
  exit 1
fi

if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][A-Za-z0-9.]+)?$ ]]; then
  echo "error: version '$VERSION' does not look like a SemVer string" >&2
  exit 1
fi

# pwd -W on Git Bash for Windows returns D:/a/... instead of /d/a/...,
# which Node.js can then resolve correctly. Falls back to plain pwd elsewhere.
ROOT="$(cd "$(dirname "$0")/.." && (pwd -W 2>/dev/null || pwd))"

# 1. Root CMakeLists.txt — `project(flox VERSION X.Y.Z LANGUAGES CXX C)`
#    Use Perl for cross-platform in-place edit (BSD sed and GNU sed disagree on -i).
perl -i -pe "s/^(project\(flox VERSION )[^ )]+/\${1}${VERSION}/" "$ROOT/CMakeLists.txt"

# 2. python/pyproject.toml — `version = "X.Y.Z"`
perl -i -pe "s/^(version = \")[^\"]+(\")/\${1}${VERSION}\${2}/" "$ROOT/python/pyproject.toml"

# 3. node/package.json — JSON, edit via node so we don't break formatting on weird inputs
node -e "
  const fs = require('fs');
  const path = '$ROOT/node/package.json';
  const pkg = JSON.parse(fs.readFileSync(path, 'utf8'));
  pkg.version = '${VERSION}';
  fs.writeFileSync(path, JSON.stringify(pkg, null, 2) + '\n');
"

echo "Set version to ${VERSION} in:"
echo "  CMakeLists.txt  ($(grep -E '^project\(flox VERSION' "$ROOT/CMakeLists.txt"))"
echo "  pyproject.toml  ($(grep -E '^version = ' "$ROOT/python/pyproject.toml"))"
echo "  package.json    (\"version\": \"$(node -e "console.log(require('$ROOT/node/package.json').version)")\")"
