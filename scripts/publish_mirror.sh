#!/usr/bin/env bash
# scripts/publish_mirror.sh -- cut the venue core out of this repository, with
# its history, and publish it to the read-only mirror (T013 step 3).
#
# What it does, in order:
#
#   1. Clones THIS repository's current branch into a scratch directory
#      (single branch, full history -- nothing from any other branch is ever
#      in the clone).
#   2. Runs `git filter-repo` there with exactly scripts/mirror_paths.py's
#      file list, so the clone's history is the history of those files and
#      nothing else. Commits that touched nothing on the list disappear;
#      the ones that remain keep their authors and dates, so the cut is
#      deterministic: the same source history cuts to the same SHAs every
#      time, and a publish after a merge is a fast-forward of the mirror.
#   3. Deletes every tag. The mirror has no tags, ever (the v* rule); a tag
#      that survived the cut would be a version somebody could pin to, so
#      the script refuses to go on if one is still there.
#   4. Lays mirror/overlay/ on top -- the mirror's own README, SECURITY.md,
#      CODEOWNERS and CI workflow, replacing this repository's ci.yml -- as
#      one commit dated like the source commit it describes, so that too is
#      deterministic.
#   5. Scans the cut history: gitleaks over every commit (when gitleaks is
#      installed; the mirror workflow installs it) with this repository's
#      .gitleaks.toml, and a denylist of words
#      that must not appear in the mirror at all, given with --denylist (a
#      file, one pattern per line, grep -E). The denylist is not in this
#      repository on purpose: the words on it are the words that must not
#      be here.
#   6. Prints the file list and the cut HEAD, and -- only with --push --
#      pushes ONE ref, refs/heads/main, with --force-with-lease against the
#      SHA the mirror had when the script looked. Never --force, never
#      --tags, never --mirror.
#
# Without --push this is a dry run: everything but the push happens, and
# the scratch directory is left where the last line says, for inspection.
#
# Usage:
#   scripts/publish_mirror.sh [--push] [--remote URL] [--denylist FILE]
#                             [--work DIR] [--list]
#
#   --remote   where to push; default git@github.com:FLOX-Foundation/venue-core.git.
#              A local bare repository path is the rehearsal the checklist asks for.
#   --denylist patterns the cut must not contain, anywhere in its history.
#   --work     scratch directory; default a fresh mktemp -d.
#   --list     print every file of the cut HEAD (the checklist's "show the
#              operator the whole list"); the count is printed regardless.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
remote="git@github.com:FLOX-Foundation/venue-core.git"
denylist=""
work=""
push=0
list=0
while [ $# -gt 0 ]; do
  case "$1" in
    --push) push=1 ;;
    --list) list=1 ;;
    --remote) remote="$2"; shift ;;
    --denylist) denylist="$2"; shift ;;
    --work) work="$2"; shift ;;
    -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
    *) echo "publish_mirror: unknown argument '$1'" >&2; exit 2 ;;
  esac
  shift
done

say() { echo "[mirror] $*"; }
die() { echo "[mirror] ERROR: $*" >&2; exit 1; }

command -v git-filter-repo >/dev/null 2>&1 || git filter-repo --version >/dev/null 2>&1 \
  || die "git filter-repo is not installed (pip install git-filter-repo)"
[ -z "$denylist" ] || [ -r "$denylist" ] || die "cannot read the denylist at $denylist"

cd "$repo_root"
branch="$(git rev-parse --abbrev-ref HEAD)"
source_head="$(git rev-parse HEAD)"
[ "$branch" != "HEAD" ] || die "detached HEAD: check out the branch to publish first"
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  die "the working tree has uncommitted changes; the cut is made from commits only"
fi

if [ -z "$work" ]; then
  work="$(mktemp -d "${TMPDIR:-/tmp}/venue-core-cut.XXXXXX")"
fi
mkdir -p "$work"
cut="$work/cut"
rm -rf "$cut"

# 1. A fresh single-branch clone with the whole history of that branch.
say "source: $branch @ $source_head"
git clone --quiet --no-local --single-branch --branch "$branch" "$repo_root" "$cut"

# 2. The cut. The list is the one scripts/lite_standalone_check.sh copies,
#    read from the SOURCE tree so the script that defines the mirror is the
#    one that is checked in, not one that may have been cut away.
list_file="$work/paths.txt"
python3 scripts/mirror_paths.py > "$list_file"
n_paths=$(wc -l < "$list_file" | tr -d ' ')
[ "$n_paths" -gt 0 ] || die "mirror_paths.py listed nothing"
say "mirror_paths.py: $n_paths files"
args=()
while IFS= read -r p; do
  [ -n "$p" ] && args+=( --path "$p" )
done < "$list_file"
( cd "$cut" && git filter-repo --quiet "${args[@]}" )
( cd "$cut" && git checkout --quiet "$branch" 2>/dev/null || true )

# 3. No tags, ever.
( cd "$cut" && git tag -l | while IFS= read -r t; do [ -n "$t" ] && git tag -d "$t" >/dev/null; done )
remaining=$(cd "$cut" && git tag -l | wc -l | tr -d ' ')
[ "$remaining" -eq 0 ] || die "$remaining tag(s) survived the cut; the mirror carries no tags"
other_refs=$(cd "$cut" && git for-each-ref --format='%(refname)' | grep -v "^refs/heads/$branch$" || true)
[ -z "$other_refs" ] || die "refs other than refs/heads/$branch in the cut:
$other_refs"

# 4. The overlay, as one commit dated like the source it describes.
overlay="$repo_root/mirror/overlay"
[ -d "$overlay" ] || die "no overlay at $overlay"
( cd "$overlay" && find . -type f | sed 's|^\./||' ) | while IFS= read -r f; do
  mkdir -p "$cut/$(dirname "$f")"
  cp "$overlay/$f" "$cut/$f"
done
source_date="$(git log -1 --format=%cI "$source_head")"
(
  cd "$cut"
  git add -A
  GIT_AUTHOR_NAME="venue-core mirror" GIT_AUTHOR_EMAIL="mirror@users.noreply.github.com" \
  GIT_COMMITTER_NAME="venue-core mirror" GIT_COMMITTER_EMAIL="mirror@users.noreply.github.com" \
  GIT_AUTHOR_DATE="$source_date" GIT_COMMITTER_DATE="$source_date" \
  git commit --quiet --allow-empty -m "mirror: overlay for FLOX-Foundation/flox@${source_head:0:12}

Read-only mirror of the venue core. Contributions go to FLOX-Foundation/flox."
)
cut_head="$(cd "$cut" && git rev-parse HEAD)"
n_files=$(cd "$cut" && git ls-files | wc -l | tr -d ' ')
n_commits=$(cd "$cut" && git rev-list --count HEAD)
say "cut: $cut_head, $n_commits commits, $n_files files at HEAD"

# 5. Scans. Every commit, not just HEAD: what was once in the history is
#    published with it.
if command -v gitleaks >/dev/null 2>&1; then
  say "gitleaks over the cut history"
  if ! ( cd "$cut" && gitleaks git --no-banner --redact --exit-code 1 --config "$repo_root/.gitleaks.toml" . ); then
    die "gitleaks found something in the cut history; nothing is published"
  fi
else
  say "WARNING: gitleaks is not installed; the secret scan did not run"
fi
if [ -n "$denylist" ]; then
  say "denylist over the cut history"
  hits="$(cd "$cut" && git log -p --all --format='commit %H' | grep -E -n -i -f "$denylist" | head -20 || true)"
  if [ -n "$hits" ]; then
    echo "$hits" | sed 's/^/[mirror]   /' >&2
    die "the denylist matched in the cut history; nothing is published"
  fi
  tree_hits="$(cd "$cut" && git grep -E -n -i -f "$denylist" HEAD -- . | head -20 || true)"
  if [ -n "$tree_hits" ]; then
    echo "$tree_hits" | sed 's/^/[mirror]   /' >&2
    die "the denylist matched in the cut tree; nothing is published"
  fi
else
  say "WARNING: no --denylist; the word scan did not run"
fi
for forbidden in backtest aggregator connectors/bitget connectors/bybit connectors/binance bindings python/ node/; do
  if ( cd "$cut" && git ls-files | grep -q "^$forbidden" ); then
    die "the cut carries '$forbidden', which the mirror must not"
  fi
done

# 6. Show, then push one ref.
if [ "$list" -eq 1 ]; then
  ( cd "$cut" && git ls-files )
fi
if [ "$push" -eq 0 ]; then
  say "dry run: would push $cut_head to $remote refs/heads/main"
  say "the cut is at $cut"
  exit 0
fi
expected="$(git ls-remote "$remote" refs/heads/main 2>/dev/null | cut -f1 || true)"
if [ -n "$expected" ]; then
  say "mirror main is at $expected; pushing with --force-with-lease against it"
  ( cd "$cut" && git push --no-tags "--force-with-lease=refs/heads/main:$expected" "$remote" "HEAD:refs/heads/main" )
else
  say "mirror has no main yet; first publication"
  ( cd "$cut" && git push --no-tags "$remote" "HEAD:refs/heads/main" )
fi
say "published $cut_head to $remote"
