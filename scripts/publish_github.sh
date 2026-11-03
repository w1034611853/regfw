#!/usr/bin/env bash
# Explicit user-run operation: create a PRIVATE repository on the authenticated
# GitHub account and push this complete source tree. Requires GitHub CLI.
set -euo pipefail
cd "$(dirname "$0")/.."
repo="${1:-regfw}"
if ! command -v gh >/dev/null 2>&1; then
    echo 'Missing GitHub CLI. Install gh and run gh auth login first.' >&2
    exit 2
fi
if ! gh auth status >/dev/null 2>&1; then
    echo 'GitHub not logged in. Run gh auth login first.' >&2
    exit 2
fi
if [[ ! -d .git ]]; then
    git init -b main
    git add .
    login=$(gh api user --jq .login)
    user_id=$(gh api user --jq .id)
    git -c user.name="$login" \
        -c user.email="${user_id}+${login}@users.noreply.github.com" \
        commit -m 'Release regfw 0.37.0'
fi
if origin=$(git remote get-url origin 2>/dev/null); then
    # Cloning the supplied Git bundle creates a local origin pointing to
    # *.bundle. It is safe to replace that *local bundle* remote.
    if [[ "$origin" == *.bundle ]]; then
        git remote remove origin
    else
        echo "origin already points to $origin; check the destination first." >&2
        exit 2
    fi
fi
# Private by default to avoid accidentally publishing proprietary device SDKs.
gh repo create "$repo" --private --source . --remote origin --push
printf 'GitHub repository created: '
gh repo view --json url --jq .url
