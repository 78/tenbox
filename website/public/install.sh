#!/usr/bin/env sh
# Marketing-site shim for the TenBox installer. Hosted at
# https://tenbox.ai/install.sh; forwards to the canonical script published
# alongside each GitHub Release tag (or `latest`). Keeping the shim tiny
# means we don't have to redeploy the marketing site to ship installer
# changes -- only cut a new GitHub Release.
#
# Usage:
#   curl -fsSL https://tenbox.ai/install.sh | sh
#
# Pin to a specific release:
#   curl -fsSL https://tenbox.ai/install.sh | TENBOX_RELEASE_TAG=v0.4.0 sh

set -eu

TENBOX_RELEASE_TAG="${TENBOX_RELEASE_TAG:-latest}"
TENBOX_RELEASE_BASE="${TENBOX_RELEASE_BASE:-https://github.com/78/tenbox/releases}"

if [ "$TENBOX_RELEASE_TAG" = "latest" ]; then
    upstream="$TENBOX_RELEASE_BASE/latest/download/install-linux.sh"
else
    upstream="$TENBOX_RELEASE_BASE/download/$TENBOX_RELEASE_TAG/install-linux.sh"
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "tenbox install: curl is required" >&2
    exit 1
fi

# Re-export so the inner installer sees them too.
export TENBOX_RELEASE_TAG TENBOX_RELEASE_BASE
exec sh -c "$(curl -fsSL "$upstream")"
