#!/bin/sh
set -u

die() { echo "$*" >&2; exit 1; }

require_linux() {
  os="$(uname -s 2>/dev/null || echo unknown)"
  [ "$os" = "Linux" ] || die "Agent tools require a Linux guest OS."
}

home_dir() {
  h="${HOME:-}"
  [ -n "$h" ] || h="$(getent passwd tenbox 2>/dev/null | cut -d: -f6 || true)"
  [ -n "$h" ] || h="/home/tenbox"
  printf '%s\n' "$h"
}

agent_rel() {
  case "$1" in
    hermes) echo ".hermes" ;;
    openclaw) echo ".openclaw" ;;
    *) die "Unsupported agent: $1" ;;
  esac
}

agent_cmd() {
  case "$1" in
    hermes) command -v hermes 2>/dev/null || true ;;
    openclaw) command -v openclaw 2>/dev/null || true ;;
    *) true ;;
  esac
}

agent_service() {
  case "$1" in
    hermes) echo "hermes-agent" ;;
    openclaw) echo "openclaw-agent" ;;
    *) die "Unsupported agent: $1" ;;
  esac
}

agent_excludes() {
  case "$1" in
    hermes) echo ".hermes/logs .hermes/tmp .hermes/cache .hermes/sessions .hermes/node_modules" ;;
    openclaw) echo ".openclaw/logs .openclaw/tmp .openclaw/cache .openclaw/sessions .openclaw/node_modules .openclaw/backup" ;;
    *) die "Unsupported agent: $1" ;;
  esac
}

validate_archive() {
  archive="$1" rel="$2"
  command -v python3 >/dev/null 2>&1 || die "python3 is required to validate the archive."
  python3 - "$archive" "$rel" <<'PY'
import os, sys, tarfile
archive, rel = sys.argv[1], sys.argv[2].rstrip('/') + '/'
try:
    tar = tarfile.open(archive, 'r:gz')
except Exception as exc:
    print(f'Invalid archive: {exc}', file=sys.stderr); sys.exit(2)
with tar:
    members = tar.getmembers()
    if not members:
        print('Archive is empty.', file=sys.stderr); sys.exit(2)
    for m in members:
        name = m.name; norm = os.path.normpath(name)
        if name.startswith('/') or norm == '..' or norm.startswith('../'):
            print(f'Archive contains unsafe path: {name}', file=sys.stderr); sys.exit(2)
        if not (norm == rel.rstrip('/') or norm.startswith(rel)):
            print(f'Archive contains unexpected path: {name}', file=sys.stderr); sys.exit(2)
        if m.issym() or m.islnk():
            print(f'Archive contains unsupported link: {name}', file=sys.stderr); sys.exit(2)
PY
}

write_manifest() {
  archive="$1" manifest="$2" agent="$3" scope="$4"
  command -v python3 >/dev/null 2>&1 || return 0
  python3 - "$archive" "$manifest" "$agent" "$scope" <<'PY' || true
import hashlib, json, os, sys, tarfile, time
archive, manifest, agent, scope = sys.argv[1:5]
sha = hashlib.sha256()
with open(archive, 'rb') as f:
    for chunk in iter(lambda: f.read(1024 * 1024), b''):
        sha.update(chunk)
try:
    with tarfile.open(archive, 'r:gz') as tar:
        count = sum(1 for m in tar.getmembers() if m.isfile())
except Exception:
    count = 0
data = {'agent': agent, 'scope': scope, 'archive': os.path.basename(archive), 'sha256': sha.hexdigest(), 'created_at': int(time.time()), 'file_count': count}
with open(manifest, 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)
PY
}

export_profile() {
  agent="$1" output="$2" scope="${3:-backup}"
  home="$(home_dir)" rel="$(agent_rel "$agent")" src="$home/$rel"
  [ -d "$src" ] || die "$agent profile was not found at $src."
  work="$(dirname "$output")/.tenbox-profile-work.$$"
  tmp="${output}.tmp.$$"
  rm -rf "$work" "$tmp"
  mkdir -p "$work" || die "Failed to create profile work directory."
  exclude_args=""
  for path in $(agent_excludes "$agent"); do exclude_args="$exclude_args --exclude=$path"; done
  cat > "$work/manifest.json" <<EOF
{
  "format": "tenbox-agent-profile",
  "format_version": 2,
  "agent_type": "$agent",
  "export_scope": "$scope",
  "archive": "files.tar.gz"
}
EOF
  (cd "$home" && tar -czf "$work/files.tar.gz" $exclude_args "$rel") || { rm -rf "$work" "$tmp"; die "Failed to export $agent profile."; }
  (cd "$work" && tar -czf "$tmp" manifest.json files.tar.gz) || { rm -rf "$work" "$tmp"; die "Failed to package $agent profile."; }
  mv "$tmp" "$output"
  write_manifest "$output" "${output}.manifest.json" "$agent" "$scope"
  rm -rf "$work"
  echo "Exported $agent profile to $output."
}

import_profile() {
  agent="$1" input="$2"
  home="$(home_dir)" rel="$(agent_rel "$agent")" target="$home/$rel"
  [ -f "$input" ] || die "Backup file was not found: $input."
  work="$(dirname "$input")/.tenbox-profile-import.$$"
  rm -rf "$work"
  mkdir -p "$work" || die "Failed to create import work directory."
  tar -xzf "$input" -C "$work" || { rm -rf "$work"; die "Invalid profile package."; }
  [ -f "$work/manifest.json" ] || { rm -rf "$work"; die "Import package is missing manifest.json."; }
  [ -f "$work/files.tar.gz" ] || { rm -rf "$work"; die "Import package is missing files.tar.gz."; }
  command -v python3 >/dev/null 2>&1 || { rm -rf "$work"; die "python3 is required to validate the archive."; }
  pkg_agent="$(python3 - "$work/manifest.json" <<'PY' 2>/dev/null || true
import json, sys
with open(sys.argv[1], encoding='utf-8') as f:
    print(json.load(f).get('agent_type', ''))
PY
)"
  [ "$pkg_agent" = "$agent" ] || { rm -rf "$work"; die "Import package belongs to $pkg_agent, not $agent."; }
  validate_archive "$work/files.tar.gz" "$rel"
  if [ -e "$target" ]; then mv "$target" "$target.before-import-$(date +%Y%m%d-%H%M%S)" || die "Failed to preserve existing profile."; fi
  (cd "$home" && tar -xzf "$work/files.tar.gz") || { rm -rf "$work"; die "Failed to import $agent profile."; }
  rm -rf "$work"
  echo "Imported $agent profile from $input."
}

export_openclaw_source() {
  output="$1"
  home="$(home_dir)"
  src="$home/.openclaw"
  [ -d "$src" ] || die "OpenClaw profile was not found at $src."
  tmp="${output}.tmp.$$"
  rm -f "$tmp"
  exclude_args=""
  for path in $(agent_excludes openclaw); do exclude_args="$exclude_args --exclude=$path"; done
  (cd "$home" && tar -czf "$tmp" $exclude_args ".openclaw") || { rm -f "$tmp"; die "Failed to export OpenClaw source."; }
  mv "$tmp" "$output"
  echo "Exported OpenClaw source to $output."
}

health_status() {
  agent="$1" cmd="$(agent_cmd "$agent")" service="$(agent_service "$agent")"
  echo "Agent: $agent"
  [ -n "$cmd" ] && { echo "Command: $cmd"; "$cmd" --version 2>&1 | head -n 1 || true; } || echo "Command: not found"
  if command -v systemctl >/dev/null 2>&1; then
    state="$(systemctl --user is-active "$service" 2>/dev/null || true)"
    [ -n "$state" ] || state="$(systemctl is-active "$service" 2>/dev/null || true)"
    [ -n "$state" ] || state="unknown"
    echo "Service: $service ($state)"
  else
    echo "Service: systemctl not available"
  fi
  config_dir="$(home_dir)/$(agent_rel "$agent")"
  [ -d "$config_dir" ] && echo "Config: $config_dir" || echo "Config: missing ($config_dir)"
}

test_model() {
  agent="$1"
  curl -fsS --max-time 5 http://10.0.2.3/v1/models >/dev/null 2>&1 && echo "Model proxy is available for $agent." || die "Model proxy is unavailable for $agent."
}

restart_agent() {
  agent="$1" service="$(agent_service "$agent")"
  if command -v systemctl >/dev/null 2>&1; then
    systemctl --user restart "$service" 2>/dev/null && { echo "Restarted user service $service."; return 0; }
    systemctl restart "$service" 2>/dev/null && { echo "Restarted system service $service."; return 0; }
  fi
  echo "No restartable service was found for $agent."
}

reset_config() {
  agent="$1" home="$(home_dir)" rel="$(agent_rel "$agent")" target="$home/$rel"
  [ -e "$target" ] || { echo "$agent config is already absent."; return 0; }
  moved="$target.reset-$(date +%Y%m%d-%H%M%S)"
  mv "$target" "$moved" || die "Failed to move $target."
  echo "Moved $target to $moved."
}

diagnostics() {
  agent="$1" output_dir="$2"
  mkdir -p "$output_dir" || die "Failed to create diagnostics directory."
  { echo "Agent: $agent"; echo "Generated at: $(date -Iseconds 2>/dev/null || date)"; echo; health_status "$agent"; echo; ps -ef 2>/dev/null | grep -E '(hermes|openclaw)' | grep -v grep || true; echo; df -h 2>/dev/null || true; } > "$output_dir/diagnostics.txt"
  if command -v journalctl >/dev/null 2>&1; then
    service="$(agent_service "$agent")"
    journalctl --user -u "$service" -n 200 --no-pager > "$output_dir/journal-user.log" 2>/dev/null || true
    journalctl -u "$service" -n 200 --no-pager > "$output_dir/journal-system.log" 2>/dev/null || true
  fi
  echo "Diagnostics were written to $output_dir."
}

restore_tenbox_model_config() {
  hermes_cmd="$(agent_cmd hermes)"; [ -n "$hermes_cmd" ] || return 0
  set +e
  "$hermes_cmd" config set models.tenbox-default.provider openai >/dev/null 2>&1
  "$hermes_cmd" config set models.tenbox-default.baseURL http://127.0.0.1:7192/v1 >/dev/null 2>&1
  "$hermes_cmd" config set models.tenbox-default.apiKey tenbox >/dev/null 2>&1
  "$hermes_cmd" config set models.tenbox-default.model gpt-4o >/dev/null 2>&1
  "$hermes_cmd" config set defaultModel tenbox-default >/dev/null 2>&1
  "$hermes_cmd" config set model tenbox-default >/dev/null 2>&1
  set -e
}

configure_channels() {
  source_root="$1" home="$(home_dir)"
  command -v python3 >/dev/null 2>&1 || return 0
  python3 - "$source_root" "$home/.hermes" <<'PY' || true
import json, os, shutil, sys
source, hermes = sys.argv[1], sys.argv[2]
openclaw = os.path.join(source, '.openclaw')
os.makedirs(hermes, exist_ok=True)
for name in ('feishu', 'lark', 'wechat', 'wecom'):
    src = os.path.join(openclaw, name)
    dst = os.path.join(hermes, name)
    if os.path.isdir(src) and not os.path.exists(dst): shutil.copytree(src, dst)
src_settings = os.path.join(openclaw, 'settings.json')
dst_settings = os.path.join(hermes, 'settings.json')
if not os.path.exists(src_settings): sys.exit(0)
try: source_data = json.load(open(src_settings, encoding='utf-8'))
except Exception: sys.exit(0)
try: target_data = json.load(open(dst_settings, encoding='utf-8'))
except Exception: target_data = {}
for key in ('mcpServers', 'hooks', 'statusLine', 'permissions'):
    if key in source_data and key not in target_data: target_data[key] = source_data[key]
json.dump(target_data, open(dst_settings, 'w', encoding='utf-8'), ensure_ascii=False, indent=2)
PY
}

migrate_openclaw() {
  input="$1" report="$2" skill_conflict="$3" workspace_target="$4" mode="$5"
  validate_archive "$input" ".openclaw"
  tmp="${TMPDIR:-/tmp}/tenbox-openclaw-migration.$$"; rm -rf "$tmp"; mkdir -p "$tmp" || die "Failed to create temporary directory."
  (cd "$tmp" && tar -xzf "$input") || { rm -rf "$tmp"; die "Failed to unpack OpenClaw source."; }
  source_dir="$tmp/.openclaw"
  [ -d "$source_dir" ] || { rm -rf "$tmp"; die "Migration package is missing .openclaw."; }
  { echo "# OpenClaw to Hermes migration"; echo; echo "- Mode: $mode"; echo "- Generated at: $(date -Iseconds 2>/dev/null || date)"; echo; } > "$report"
  hermes_cmd="$(agent_cmd hermes)"
  if [ -n "$hermes_cmd" ]; then
    if [ "$mode" = "dry-run" ]; then yes_arg="--dry-run"; else yes_arg="--yes"; fi
    if [ -n "$workspace_target" ]; then
      "$hermes_cmd" --overwrite claw migrate --source "$source_dir" --skill-conflict "$skill_conflict" --workspace-target "$workspace_target" --migrate-secrets "$yes_arg" >> "$report" 2>&1 || { [ "$mode" = "dry-run" ] || die "Hermes migration failed. See $report."; }
    else
      "$hermes_cmd" --overwrite claw migrate --source "$source_dir" --skill-conflict "$skill_conflict" --migrate-secrets "$yes_arg" >> "$report" 2>&1 || { [ "$mode" = "dry-run" ] || die "Hermes migration failed. See $report."; }
    fi
  elif [ "$mode" != "dry-run" ]; then
    home="$(home_dir)"; [ -e "$home/.hermes" ] && mv "$home/.hermes" "$home/.hermes.before-openclaw-migration-$(date +%Y%m%d-%H%M%S)"
    cp -a "$source_dir" "$home/.hermes" || die "Failed to copy OpenClaw profile into Hermes profile."
  else
    echo "Hermes CLI was not found; only archive validation was completed." >> "$report"
  fi
  if [ "$mode" != "dry-run" ]; then configure_channels "$tmp"; restore_tenbox_model_config; echo "Migration completed." >> "$report"; fi
  echo "Migration report was written to $report."
  rm -rf "$tmp"
}

main() {
  require_linux
  cmd="${1:-}"; [ -n "$cmd" ] || die "Missing command."; shift
  case "$cmd" in
    export-profile) [ "$#" -ge 2 ] || die "Usage: export-profile AGENT OUTPUT [SCOPE]"; export_profile "$1" "$2" "${3:-backup}" ;;
    import-profile) [ "$#" -eq 2 ] || die "Usage: import-profile AGENT INPUT"; import_profile "$1" "$2" ;;
    health) [ "$#" -eq 1 ] || die "Usage: health AGENT"; health_status "$1" ;;
    test-model) [ "$#" -eq 1 ] || die "Usage: test-model AGENT"; test_model "$1" ;;
    restart) [ "$#" -eq 1 ] || die "Usage: restart AGENT"; restart_agent "$1" ;;
    reset-config) [ "$#" -eq 1 ] || die "Usage: reset-config AGENT"; reset_config "$1" ;;
    diagnostics) [ "$#" -eq 2 ] || die "Usage: diagnostics AGENT OUTPUT_DIR"; diagnostics "$1" "$2" ;;
    export-openclaw-source) [ "$#" -eq 1 ] || die "Usage: export-openclaw-source OUTPUT"; export_openclaw_source "$1" ;;
    migrate-openclaw-dry-run) [ "$#" -eq 4 ] || die "Usage: migrate-openclaw-dry-run INPUT REPORT SKILL_CONFLICT WORKSPACE_TARGET"; migrate_openclaw "$1" "$2" "$3" "$4" dry-run ;;
    migrate-openclaw-apply) [ "$#" -eq 4 ] || die "Usage: migrate-openclaw-apply INPUT REPORT SKILL_CONFLICT WORKSPACE_TARGET"; migrate_openclaw "$1" "$2" "$3" "$4" apply ;;
    *) die "Unknown command: $cmd" ;;
  esac
}

main "$@"
