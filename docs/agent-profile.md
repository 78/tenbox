# Agent Data Tools

TenBox.app provides Agent data export/import, backup/restore, and health actions
without requiring Hermes/OpenClaw images to preinstall TenBox-specific scripts.

The macOS manager creates a temporary shared folder, then sends a short shell
command through the existing VM console channel. The command uses standard guest
tools such as `tar`, `gzip`, `systemctl`, `curl`, and `journalctl`.

## Profile package

The exported package is a gzip tar archive:

```text
<vm>-<agent>-profile.tar.gz
├── manifest.json
└── files.tar.gz
```

`manifest.json` contains:

- `format`: `tenbox-agent-profile`
- `format_version`: `2`
- `agent_type`: `hermes` or `openclaw`
- `archive`: `files.tar.gz`

`files.tar.gz` contains the Agent data directory relative to the guest home:

- Hermes: `.hermes`
- OpenClaw: `.openclaw`

Excluded paths:

- Hermes: `.hermes/logs`, `.hermes/image_cache`, `.hermes/audio_cache`
- OpenClaw: `.openclaw/cache`, `.openclaw/.cache`, `.openclaw/workspace/.cache`

Import rejects packages whose `agent_type` does not match the selected Agent.
Before replacing existing data, it renames the current directory to
`*.pre-import-YYYYMMDDHHMMSS`.

## Backups

Manual backups are created by TenBox.app in:

```text
~/Library/Application Support/TenBox/AgentBackups/<vm-id>/<agent>/
```

Backups use the same profile package format and keep the newest five packages.
Restore uses the newest package for the selected VM and Agent.

## Health actions

TenBox.app can run these actions while the VM is running:

- health status
- restart Agent
- test model proxy
- reset Agent config
- export diagnostics

Restart and reset create a backup first, using the same host-managed backup
directory. Diagnostics are exported to the host backup directory through the
temporary shared folder.
