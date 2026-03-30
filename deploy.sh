#!/usr/bin/env bash
# deploy.sh — bump version, push, wait for CI, pull prebuilds
#
# Usage:
#   ./deploy.sh           — bump patch (0.2.2 → 0.2.3)
#   ./deploy.sh minor     — bump minor (0.2.2 → 0.3.0)
#   ./deploy.sh major     — bump major (0.2.2 → 1.0.0)
#   ./deploy.sh 1.2.3     — set exact version

set -euo pipefail

BUMP="${1:-patch}"
REPO="vanbaalon/wolfbook-btl"
WAIT_SECONDS=400   # 6-7 min ceiling; polls every 15 s

# ── 1. bump version ──────────────────────────────────────────────────────────
OLD_VER=$(node -p "require('./package.json').version")

bump_version() {
  local ver="$1" part="$2"
  IFS='.' read -r major minor patch <<< "$ver"
  case "$part" in
    major) echo "$((major+1)).0.0" ;;
    minor) echo "${major}.$((minor+1)).0" ;;
    patch) echo "${major}.${minor}.$((patch+1))" ;;
    *)     echo "$part" ;;   # exact version passed
  esac
}

NEW_VER=$(bump_version "$OLD_VER" "$BUMP")

# Update package.json in-place (no npm version, avoids git tag)
node -e "
const fs = require('fs');
const pkg = JSON.parse(fs.readFileSync('package.json','utf8'));
pkg.version = '$NEW_VER';
fs.writeFileSync('package.json', JSON.stringify(pkg, null, 2) + '\n');
"

echo "Version: $OLD_VER → $NEW_VER"

# ── 2. sync & commit & push ──────────────────────────────────────────────────
# Stash any pre-existing changes to ensure rebase success
git stash --message "deploy-stash"
echo "Syncing with remote..."
git pull --rebase || { echo "ERROR: git pull failed. Resolve conflicts before re-running."; git stash pop; exit 1; }
git stash pop || true # apply previously stashed changes if any

git add package.json
git commit -m "chore: bump version to $NEW_VER"
git push || { echo "ERROR: git push failed. Another user may have pushed; try pulling again."; exit 1; }
echo "Pushed to GitHub."

# ── 3. wait for CI ───────────────────────────────────────────────────────────
echo ""
echo "Waiting for CI (up to $((WAIT_SECONDS/60)) min)..."

# Check if gh CLI is available
if ! command -v gh &>/dev/null; then
  echo "  gh CLI not found — sleeping ${WAIT_SECONDS}s then pulling."
  sleep "$WAIT_SECONDS"
else
  # Get the run ID that was created by our push (wait for it to appear first)
  RUN_ID=""
  for i in $(seq 1 20); do
    sleep 10
    RUN_ID=$(gh run list --repo "$REPO" --branch main --limit 5 \
               --json databaseId,status,headSha \
               --jq '.[0].databaseId' 2>/dev/null || true)
    if [[ -n "$RUN_ID" ]]; then break; fi
  done

  if [[ -z "$RUN_ID" ]]; then
    echo "  Could not find a CI run — sleeping ${WAIT_SECONDS}s then pulling."
    sleep "$WAIT_SECONDS"
  else
    echo "  CI run #$RUN_ID started. Polling..."
    ELAPSED=0
    while [[ $ELAPSED -lt $WAIT_SECONDS ]]; do
      sleep 15
      ELAPSED=$((ELAPSED + 15))
      STATUS=$(gh run view "$RUN_ID" --repo "$REPO" \
                  --json status,conclusion \
                  --jq '"status=\(.status) conclusion=\(.conclusion)"' 2>/dev/null || true)
      echo "  [${ELAPSED}s] $STATUS"
      CONCL=$(gh run view "$RUN_ID" --repo "$REPO" \
                 --json conclusion --jq '.conclusion' 2>/dev/null || true)
      if [[ "$CONCL" == "success" ]]; then
        echo "  CI passed!"
        break
      elif [[ "$CONCL" == "failure" || "$CONCL" == "cancelled" ]]; then
        echo "  CI $CONCL. Check: https://github.com/$REPO/actions/runs/$RUN_ID"
        exit 1
      fi
    done
    if [[ "$CONCL" != "success" ]]; then
      echo "  Timed out waiting for CI. Attempting git pull anyway..."
    fi
  fi
fi

# Extra pause so the collect job can commit prebuilds back
echo "Waiting 30s for collect job to commit prebuilds..."
sleep 30

# ── 4. pull prebuilds ─────────────────────────────────────────────────────────
git pull --rebase
echo ""
echo "Done! Prebuilds in prebuilds/:"
ls -lh prebuilds/
