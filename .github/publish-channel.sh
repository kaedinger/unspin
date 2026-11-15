#!/usr/bin/env bash
# publish-channel.sh - put a .plg on the `channel` branch, where installed betas look for updates
# (https://raw.githubusercontent.com/kaedinger/unspin/channel/unspin.plg).
# Run by CI. Releases are immutable, so the rolling beta .plg can't live in a release.
#
# Usage: .github/publish-channel.sh <plg file> <message>

set -euo pipefail

PLG="$(realpath "$1")"
MSG="$2"
WT="${RUNNER_TEMP:-/tmp}/channel"

# a worktree shares the checkout's credentials; the branch holds just this one file
git worktree add -q --detach "$WT"
cd "$WT"
git checkout -q --orphan channel-publish
git rm -rqf . || true
cp "$PLG" unspin.plg
git add unspin.plg
git -c user.name="github-actions[bot]" -c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
    commit -qm "$MSG"
git push -qf origin HEAD:channel
echo "Beta channel now serves: $(grep -o '<!ENTITY version *"[^"]*"' unspin.plg)"
