# Agent Data Profile Packages

TenBox Agent data export/import uses a versioned archive so migration,
backup, and restore flows can share one artifact shape without exposing
guest dotfile paths to non-technical users.

## Package Format

`tenbox-agent-profile.tar.zst` is a zstd-compressed tar archive:

```text
tenbox-agent-profile.tar.zst
├── manifest.json
├── files/
└── checksums.txt
```

`manifest.json` records:

- `format`: `tenbox-agent-profile`
- `format_version`: currently `1`
- `agent_type`: `hermes` or `openclaw`
- `tenbox_version`
- `created_at`
- `home`
- `source_path`
- `paths`
- `excluded`
- `checksums`

`checksums.txt` stores SHA-256 hashes for files under `files/`. It never
contains secret values directly.

## Supported Agents

Hermes profile:

- Includes `/home/tenbox/.hermes`
- Excludes `.hermes/logs`, `.hermes/image_cache`, `.hermes/audio_cache`
- Sensitive files such as API keys remain inside the package payload; logs
  and manifest output must not print their values.

OpenClaw profile:

- Includes `/home/tenbox/.openclaw`
- Excludes `.openclaw/cache`, `.openclaw/.cache`, `.openclaw/workspace/.cache`
- Sensitive files remain inside the package payload; manifest and logs must
  not print token values.

QwenPaw is intentionally not included in this version because `.qwenpaw.secret`
needs a separate sensitivity policy.

## Guest CLI

Inside Hermes and OpenClaw images:

```sh
tenbox-agent-profile export --agent hermes --output /mnt/shared/agent-data.tar.zst
tenbox-agent-profile import --agent hermes --input /mnt/shared/agent-data.tar.zst
```

The import path verifies the archive manifest and checksums, rejects cross-agent
imports, backs up existing data to `*.pre-import-*`, and then restores ownership
and permissions for the `tenbox` user.

## Automatic Agent Data Backups

Hermes and OpenClaw images also include `tenbox-agent-backup`, which reuses the
profile package format instead of creating a second backup format.

```sh
tenbox-agent-backup snapshot --agent hermes --vm-id <vm-id>
tenbox-agent-backup status --agent hermes --vm-id <vm-id>
tenbox-agent-backup restore --agent hermes --vm-id <vm-id>
```

By default backups are written under the first writable shared folder. With the
default TenBox share tag this is usually:

```text
/mnt/shared/shared/tenbox-agent-backups/<vm-id>/<agent>/
├── agent-data-YYYYMMDDHHMMSS.tar.zst
├── agent-data-YYYYMMDDHHMMSS.tar.zst.manifest.json
└── status.json
```

The default retention is the latest 5 packages per VM and Agent. The snapshot
path estimates source size before export and stops with a clear `space_low`
status when the host-side shared folder does not have enough free space. Restore
uses `tenbox-agent-profile import`, so existing Agent data is still protected by
the `*.pre-import-*` backup before replacement.

## Agent Health and Repair

`tenbox-agent-health` provides a deterministic health report and a small set of
operator actions:

```sh
tenbox-agent-health status --agent hermes
tenbox-agent-health restart --agent hermes
tenbox-agent-health test-model --agent hermes
tenbox-agent-health reset-config --agent hermes
tenbox-agent-health diagnostics --agent hermes
```

The status report separates Agent service state, gateway reachability, TenBox
LLM proxy reachability, browser availability, disk space, and the latest
redacted error summary. User-facing messages use "Agent" and "model
configuration" wording instead of requiring users to understand systemd, QEMU,
or dotfile paths.

Repair actions create an Agent data snapshot through `tenbox-agent-backup`
before changing service state or restoring default model configuration.
Diagnostics are exported to the same writable shared folder as backups with
common token patterns redacted.
