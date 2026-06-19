#!/usr/bin/env sh
# Marketing-site shim for the AgentSphere installer. Hosted at
# https://tenbox.ai/install.sh; forwards to the canonical script published
# alongside each GitHub Release tag (or `latest`). Keeping the shim tiny
# means we don't have to redeploy the marketing site to ship installer
# changes -- only cut a new GitHub Release.
#
# Usage:
#   curl -fsSL https://tenbox.ai/install.sh | sudo sh
#
# Pin to a specific release:
#   curl -fsSL https://tenbox.ai/install.sh | sudo AGENTSPHERE_RELEASE_TAG=v0.4.0 sh

set -eu

AGENTSPHERE_RELEASE_TAG="${AGENTSPHERE_RELEASE_TAG:-latest}"
AGENTSPHERE_RELEASE_BASE="${AGENTSPHERE_RELEASE_BASE:-https://github.com/78/tenbox/releases}"

if [ "$AGENTSPHERE_RELEASE_TAG" = "latest" ]; then
    upstream="$AGENTSPHERE_RELEASE_BASE/latest/download/install-linux.sh"
else
    upstream="$AGENTSPHERE_RELEASE_BASE/download/$AGENTSPHERE_RELEASE_TAG/install-linux.sh"
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "tenbox install: curl is required" >&2
    exit 1
fi

# Re-export so the inner installer sees them too.
export AGENTSPHERE_RELEASE_TAG AGENTSPHERE_RELEASE_BASE
exec sh -c "$(curl -fsSL "$upstream")"
