#!/usr/bin/env bash
# Build the flox_py wheel + install into a throwaway venv + assert
# every pure-Python module under `python/flox_py/` is importable.
#
# Catches the regression where scikit-build-core only collects what
# the CMake `python_module` install component declares — adding a new
# `.py` file to `python/flox_py/` without updating CMakeLists used to
# silently drop it from the wheel.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root/python"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

py="${PYTHON:-python3}"
echo "[wheel-gate] preparing build venv with $($py --version)..."
"$py" -m venv "$work/build-venv"
build_py="$work/build-venv/bin/python"
"$build_py" -m pip install --quiet --upgrade build pybind11 scikit-build-core
"$build_py" -m build --wheel --outdir "$work"

whl=$(ls "$work"/flox_py-*.whl | head -1)
if [[ -z "$whl" ]]; then
  echo "::error::wheel build produced no .whl in $work" >&2
  exit 1
fi
echo "[wheel-gate] built $(basename "$whl")"

# Discover modules from source. The wheel must include every one.
mapfile -t expected < <(find "$repo_root/python/flox_py" -maxdepth 1 -name '*.py' -not -name '__init__.py' -exec basename {} .py \; | sort)
echo "[wheel-gate] expecting modules: ${expected[*]}"

"$py" -m venv "$work/venv"
"$work/venv/bin/pip" install --quiet "$whl"

# Run the import smoke from /tmp so the source `python/flox_py/`
# directory cannot shadow the installed wheel via cwd resolution.
cd /tmp
import_lines=""
for mod in "${expected[@]}"; do
  import_lines="${import_lines}import flox_py.${mod}"$'\n'
done

if ! "$work/venv/bin/python" -c "$import_lines
print('[wheel-gate] all ${#expected[@]} modules imported OK')"; then
  echo "::error::wheel-gate failed — flox_py wheel does not ship every pure-Python module" >&2
  echo "Inspect with: unzip -l $whl | grep '\.py$'" >&2
  exit 1
fi
