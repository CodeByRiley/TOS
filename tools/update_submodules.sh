#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SUBMOD="userspace/bin/holyd"

cd "$REPO_ROOT"

# ensure submodule is present
git submodule update --init --recursive "$SUBMOD"

# change into submodule and update to remote tip of tracked branch
pushd "$SUBMOD" >/dev/null
BRANCH="$(git rev-parse --abbrev-ref @)"
echo "Updating $SUBMOD (branch: $BRANCH)"
git fetch origin
git pull --ff-only origin "$BRANCH" || {
  echo "Fast-forward failed; please resolve in $SUBMOD"
  popd >/dev/null
  exit 1
}
NEW_SHA="$(git rev-parse --short HEAD)"
popd >/dev/null

# build the submodule (adjust command if needed)
echo "Building $SUBMOD..."
make -C "$SUBMOD" || {
  echo "Build failed in $SUBMOD. Fix and re-run."
  exit 1
}

# optionally update superproject to point at new sha
read -p "Record new submodule SHA $NEW_SHA in superproject? [y/N] " ans
if [[ "$ans" == "y" || "$ans" == "Y" ]]; then
  git add "$SUBMOD"
  git commit -m "Update HolyD submodule to $NEW_SHA"
  echo "Committed update; push when ready."
else
  echo "Did not commit. You can inspect and commit manually."
fi
