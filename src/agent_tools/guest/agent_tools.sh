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
    hermes) echo ".hermes/logs .hermes/tmp .hermes/cache .hermes/sessions .hermes/node_modules .hermes/node_modules/* */node_modules */node_modules/*" ;;
    openclaw) echo ".openclaw/logs .openclaw/tmp .openclaw/cache .openclaw/sessions .openclaw/node_modules .openclaw/node_modules/* */node_modules */node_modules/* .openclaw/browser/*/user-data/Singleton* .openclaw/backup" ;;
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
        if not (m.isfile() or m.isdir()):
            print(f'Archive contains unsupported entry type: {name}', file=sys.stderr); sys.exit(2)
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

finalize_file() {
  tmp="$1" output="$2"
  mv "$tmp" "$output" 2>/dev/null && return 0
  cp "$tmp" "$output" 2>/dev/null && rm -f "$tmp"
}

create_live_archive() {
  archive="$1"
  shift
  err="${archive}.stderr.$$"
  "$@" 2>"$err"
  rc=$?
  if [ "$rc" -eq 0 ]; then
    rm -f "$err"
    return 0
  fi
  if [ "$rc" -eq 1 ] && [ -s "$archive" ] && awk '
    /file changed as we read it/ || /File removed before we read it/ { seen=1; next }
    /^[[:space:]]*$/ { next }
    { bad=1 }
    END { exit (seen && !bad) ? 0 : 1 }
  ' "$err"; then
    cat "$err" >&2
    rm -f "$err"
    return 0
  fi
  cat "$err" >&2
  rm -f "$err"
  return "$rc"
}

create_openclaw_source_archive() {
  archive="$1" home="$2"
  command -v python3 >/dev/null 2>&1 || die "python3 is required to export OpenClaw source."
  python3 - "$archive" "$home" <<'PY'
import os, stat, sys, tarfile

archive, home = sys.argv[1:3]
root_name = '.openclaw'
root = os.path.join(home, root_name)
if not os.path.isdir(root):
    print(f'OpenClaw profile was not found at {root}.', file=sys.stderr)
    sys.exit(1)

def arcname(path):
    return os.path.relpath(path, home).replace(os.sep, '/')

def excluded(name):
    parts = name.split('/')
    if len(parts) >= 2 and parts[0] == root_name and parts[1] in {'logs', 'tmp', 'cache', 'sessions', 'backup'}:
        return True
    if 'node_modules' in parts:
        return True
    if len(parts) >= 5 and parts[0] == root_name and parts[1] == 'browser' and 'user-data' in parts and os.path.basename(name).startswith('Singleton'):
        return True
    return False

def add_file(tar, source, name):
    try:
        info = tar.gettarinfo(source, arcname=name)
        if not info.isfile():
            return
        with open(source, 'rb') as f:
            tar.addfile(info, f)
    except FileNotFoundError:
        print(f'tar: {name}: File removed before we read it', file=sys.stderr)
    except OSError as exc:
        print(f'tar: {name}: {exc}', file=sys.stderr)

def add_path(tar, source, name, seen):
    if excluded(name):
        return
    try:
        st = os.lstat(source)
    except FileNotFoundError:
        print(f'tar: {name}: File removed before we read it', file=sys.stderr)
        return
    if stat.S_ISLNK(st.st_mode):
        try:
            real = os.path.realpath(source)
            target_st = os.stat(real)
        except FileNotFoundError:
            print(f'tar: {name}: File removed before we read it', file=sys.stderr)
            return
        key = (target_st.st_dev, target_st.st_ino)
        if key in seen:
            return
        if stat.S_ISDIR(target_st.st_mode):
            add_dir(tar, real, name, seen | {key})
        elif stat.S_ISREG(target_st.st_mode):
            add_file(tar, real, name)
        return
    if stat.S_ISDIR(st.st_mode):
        add_dir(tar, source, name, seen)
    elif stat.S_ISREG(st.st_mode):
        add_file(tar, source, name)

def add_dir(tar, source, name, seen):
    if excluded(name):
        return
    try:
        info = tar.gettarinfo(source, arcname=name)
        info.type = tarfile.DIRTYPE
        tar.addfile(info)
        entries = sorted(os.listdir(source))
    except FileNotFoundError:
        print(f'tar: {name}: File removed before we read it', file=sys.stderr)
        return
    except OSError as exc:
        print(f'tar: {name}: {exc}', file=sys.stderr)
        return
    for entry in entries:
        child = os.path.join(source, entry)
        add_path(tar, child, f'{name}/{entry}', seen)

with tarfile.open(archive, 'w:gz') as tar:
    add_path(tar, root, root_name, set())
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
  (cd "$home" && create_live_archive "$work/files.tar.gz" tar -czf "$work/files.tar.gz" $exclude_args "$rel") || { rm -rf "$work" "$tmp"; die "Failed to export $agent profile."; }
  (cd "$work" && tar -czf "$tmp" manifest.json files.tar.gz) || { rm -rf "$work" "$tmp"; die "Failed to package $agent profile."; }
  finalize_file "$tmp" "$output" || { rm -rf "$work" "$tmp"; die "Failed to finalize $agent profile export."; }
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
  validate_archive "$work/files.tar.gz" "$rel" || { rm -rf "$work"; die "Invalid profile archive."; }
  backup=""
  if [ -e "$target" ]; then
    backup="$target.before-import-$(date +%Y%m%d-%H%M%S)"
    mv "$target" "$backup" || { rm -rf "$work"; die "Failed to preserve existing profile."; }
  fi
  if ! (cd "$home" && tar -xzf "$work/files.tar.gz"); then
    rm -rf "$target"
    if [ -n "$backup" ] && [ -e "$backup" ]; then mv "$backup" "$target" || true; fi
    rm -rf "$work"
    die "Failed to import $agent profile."
  fi
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
  create_openclaw_source_archive "$tmp" "$home" || { rm -f "$tmp"; die "Failed to export OpenClaw source."; }
  validate_archive "$tmp" ".openclaw" || { rm -f "$tmp"; die "Invalid OpenClaw source archive."; }
  finalize_file "$tmp" "$output" || { rm -f "$tmp"; die "Failed to finalize OpenClaw source export."; }
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
  validate_archive "$input" ".openclaw" || die "Invalid OpenClaw source archive."
  tmp="$(dirname "$input")/.tenbox-openclaw-migration.$$"; rm -rf "$tmp"; mkdir -p "$tmp" || die "Failed to create temporary directory."
  (cd "$tmp" && tar -xmzf "$input") || { rm -rf "$tmp"; die "Failed to unpack OpenClaw source."; }
  source_dir="$tmp/.openclaw"
  [ -d "$source_dir" ] || { rm -rf "$tmp"; die "Migration package is missing .openclaw."; }
  { echo "# OpenClaw to Hermes migration"; echo; echo "- Mode: $mode"; echo "- Generated at: $(date -Iseconds 2>/dev/null || date)"; echo; } > "$report"
  hermes_cmd="$(agent_cmd hermes)"
  [ -n "$hermes_cmd" ] || die "Target VM is missing the Hermes command."
  if [ "$mode" = "dry-run" ]; then
    mode_arg="--dry-run"
    fail_message="Hermes migration dry run failed. See $report."
  else
    mode_arg="--yes"
    fail_message="Hermes migration failed. See $report."
  fi
  if [ -n "$workspace_target" ]; then
    "$hermes_cmd" claw migrate --overwrite --source "$source_dir" --skill-conflict "$skill_conflict" --workspace-target "$workspace_target" --migrate-secrets "$mode_arg" >> "$report" 2>&1 || die "$fail_message"
  else
    "$hermes_cmd" claw migrate --overwrite --source "$source_dir" --skill-conflict "$skill_conflict" --migrate-secrets "$mode_arg" >> "$report" 2>&1 || die "$fail_message"
  fi
  if grep -F "Refusing to apply" "$report" >/dev/null 2>&1; then
    rm -rf "$tmp"
    die "$fail_message"
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
