# Agent Data Backups

TenBox.app exports, imports, backs up, and restores Hermes/OpenClaw Agent data
without requiring images to preinstall TenBox-specific scripts.

The macOS manager creates a temporary shared folder, then sends a short shell
command through the existing VM console channel. The command uses standard guest
tools such as `tar` and `gzip`.

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
