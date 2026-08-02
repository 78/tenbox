#!/bin/sh
set -eu

script="${1:-}"
[ -n "$script" ] && [ -f "$script" ] || { echo "Usage: $0 PATH_TO_AGENT_TOOLS_SH" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 2; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

real_python="$(command -v python3)"
real_tar="$(command -v tar)"
real_mv="$(command -v mv)"
fakebin="$tmp/bin"
mkdir -p "$fakebin"
cat > "$fakebin/uname" <<'EOF'
#!/bin/sh
echo Linux
EOF
cat > "$fakebin/python3" <<EOF
#!/bin/sh
exec "$real_python" "\$@"
EOF
chmod +x "$fakebin/uname"
chmod +x "$fakebin/python3"

run_tool() {
  home_arg="$1"
  shift
  HOME="$home_arg" PATH="$fakebin:/usr/bin:/bin:/usr/sbin:/sbin" sh "$script" "$@"
}

assert_file_contains() {
  file="$1"
  needle="$2"
  grep -F -- "$needle" "$file" >/dev/null 2>&1 || {
    echo "Expected '$needle' in $file" >&2
    cat "$file" >&2 || true
    exit 1
  }
}

assert_file_not_contains() {
  file="$1"
  needle="$2"
  if grep -F -- "$needle" "$file" >/dev/null 2>&1; then
    echo "Did not expect '$needle' in $file" >&2
    cat "$file" >&2 || true
    exit 1
  fi
}

make_package() {
  agent="$1"
  profile_dir="$2"
  output="$3"
  work="$tmp/pkg-$(basename "$output")"
  rm -rf "$work"
  mkdir -p "$work"
  cat > "$work/manifest.json" <<EOF
{
  "format": "tenbox-agent-profile",
  "format_version": 2,
  "agent_type": "$agent",
  "export_scope": "test",
  "archive": "files.tar.gz"
}
EOF
  "$real_tar" -czf "$work/files.tar.gz" -C "$profile_dir" ".${agent}"
  "$real_tar" -czf "$output" -C "$work" manifest.json files.tar.gz
}

make_python_package() {
  agent="$1"
  output="$2"
  member_kind="$3"
  python3 - "$agent" "$output" "$member_kind" <<'PY'
import io, json, os, sys, tarfile, tempfile
agent, output, member_kind = sys.argv[1:4]
work = tempfile.mkdtemp()
manifest = {
    'format': 'tenbox-agent-profile',
    'format_version': 2,
    'agent_type': agent,
    'export_scope': 'test',
    'archive': 'files.tar.gz',
}
with open(os.path.join(work, 'manifest.json'), 'w', encoding='utf-8') as f:
    json.dump(manifest, f)
with tarfile.open(os.path.join(work, 'files.tar.gz'), 'w:gz') as tar:
    if member_kind == 'unsafe':
        data = b'bad'
        info = tarfile.TarInfo('../evil')
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))
    elif member_kind == 'link':
        info = tarfile.TarInfo(f'.{agent}/link')
        info.type = tarfile.SYMTYPE
        info.linkname = '/tmp/target'
        tar.addfile(info)
    elif member_kind == 'special':
        info = tarfile.TarInfo(f'.{agent}/device')
        info.type = tarfile.CHRTYPE
        info.devmajor = 1
        info.devminor = 3
        tar.addfile(info)
    else:
        raise SystemExit(f'unknown member kind: {member_kind}')
with tarfile.open(output, 'w:gz') as tar:
    tar.add(os.path.join(work, 'manifest.json'), arcname='manifest.json')
    tar.add(os.path.join(work, 'files.tar.gz'), arcname='files.tar.gz')
PY
}

home="$tmp/home"
mkdir -p "$home/.hermes"
printf '{"model":"tenbox"}\n' > "$home/.hermes/settings.json"
run_tool "$home" export-profile hermes "$tmp/hermes-profile.tgz" backup > "$tmp/export.out"
[ -f "$tmp/hermes-profile.tgz" ] || { echo "export did not create package" >&2; exit 1; }
[ -f "$tmp/hermes-profile.tgz.manifest.json" ] || { echo "export did not create manifest" >&2; exit 1; }

cat > "$fakebin/mv" <<EOF
#!/bin/sh
case "\$1" in
  *.tmp.*) exit 9 ;;
esac
exec "$real_mv" "\$@"
EOF
chmod +x "$fakebin/mv"
fallback_home="$tmp/fallback-home"
mkdir -p "$fallback_home/.hermes"
printf '{"fallback":true}\n' > "$fallback_home/.hermes/settings.json"
run_tool "$fallback_home" export-profile hermes "$tmp/fallback-profile.tgz" backup > "$tmp/fallback-export.out"
[ -f "$tmp/fallback-profile.tgz" ] || { echo "export finalize fallback did not create package" >&2; exit 1; }
set -- "$tmp"/fallback-profile.tgz.tmp.*
[ ! -e "$1" ] || { echo "export finalize fallback left a tmp file" >&2; exit 1; }
rm -f "$fakebin/mv"

cat > "$fakebin/tar" <<EOF
#!/bin/sh
has_hermes=0
for arg in "\$@"; do
  [ "\$arg" = ".hermes" ] && has_hermes=1
done
if [ "\$1" = "-czf" ] && [ "\$has_hermes" -eq 1 ] && [ "\${LIVE_TAR_MODE:-}" = "churn" ]; then
  "$real_tar" "\$@"
  echo "tar: .hermes/kanban.db-wal: File removed before we read it" >&2
  echo "tar: .hermes: file changed as we read it" >&2
  exit 1
fi
if [ "\$1" = "-czf" ] && [ "\$has_hermes" -eq 1 ] && [ "\${LIVE_TAR_MODE:-}" = "mixed-error" ]; then
  "$real_tar" "\$@"
  echo "tar: .hermes: file changed as we read it" >&2
  echo "tar: .hermes/missing: Cannot stat: No such file or directory" >&2
  exit 1
fi
exec "$real_tar" "\$@"
EOF
chmod +x "$fakebin/tar"
churn_home="$tmp/churn-home"
mkdir -p "$churn_home/.hermes"
printf '{"churn":true}\n' > "$churn_home/.hermes/settings.json"
LIVE_TAR_MODE=churn run_tool "$churn_home" export-profile hermes "$tmp/churn-profile.tgz" backup > "$tmp/churn-export.out" 2> "$tmp/churn-export.err"
[ -f "$tmp/churn-profile.tgz" ] || { echo "live-churn export did not create package" >&2; exit 1; }
assert_file_contains "$tmp/churn-export.err" "File removed before we read it"
assert_file_contains "$tmp/churn-export.err" "file changed as we read it"
if LIVE_TAR_MODE=mixed-error run_tool "$churn_home" export-profile hermes "$tmp/mixed-error-profile.tgz" backup > "$tmp/mixed-error-export.out" 2>&1; then
  echo "mixed tar error unexpectedly succeeded" >&2
  exit 1
fi
assert_file_contains "$tmp/mixed-error-export.out" "Cannot stat"
assert_file_contains "$tmp/mixed-error-export.out" "Failed to export hermes profile"
rm -f "$fakebin/tar"

fresh_home="$tmp/fresh-home"
mkdir -p "$fresh_home"
run_tool "$fresh_home" import-profile hermes "$tmp/hermes-profile.tgz" > "$tmp/import.out"
[ -f "$fresh_home/.hermes/settings.json" ] || { echo "import did not restore settings" >&2; exit 1; }

if run_tool "$fresh_home" import-profile openclaw "$tmp/hermes-profile.tgz" > "$tmp/cross.out" 2>&1; then
  echo "cross-agent import unexpectedly succeeded" >&2
  exit 1
fi
assert_file_contains "$tmp/cross.out" "not openclaw"

make_python_package hermes "$tmp/unsafe.tgz" unsafe
if run_tool "$fresh_home" import-profile hermes "$tmp/unsafe.tgz" > "$tmp/unsafe.out" 2>&1; then
  echo "unsafe archive unexpectedly imported" >&2
  exit 1
fi
assert_file_contains "$tmp/unsafe.out" "unsafe path"

make_python_package hermes "$tmp/link.tgz" link
if run_tool "$fresh_home" import-profile hermes "$tmp/link.tgz" > "$tmp/link.out" 2>&1; then
  echo "link archive unexpectedly imported" >&2
  exit 1
fi
assert_file_contains "$tmp/link.out" "unsupported link"

make_python_package hermes "$tmp/special.tgz" special
if run_tool "$fresh_home" import-profile hermes "$tmp/special.tgz" > "$tmp/special.out" 2>&1; then
  echo "special archive unexpectedly imported" >&2
  exit 1
fi
assert_file_contains "$tmp/special.out" "unsupported entry type"

rollback_src="$tmp/rollback-src"
rollback_home="$tmp/rollback-home"
mkdir -p "$rollback_src/.hermes" "$rollback_home/.hermes"
printf 'new\n' > "$rollback_src/.hermes/settings.json"
printf 'old\n' > "$rollback_home/.hermes/settings.json"
make_package hermes "$rollback_src" "$tmp/rollback.tgz"
cat > "$fakebin/tar" <<EOF
#!/bin/sh
case " \$* " in
  *files.tar.gz*) exit 9 ;;
esac
exec "$real_tar" "\$@"
EOF
chmod +x "$fakebin/tar"
if run_tool "$rollback_home" import-profile hermes "$tmp/rollback.tgz" > "$tmp/rollback.out" 2>&1; then
  echo "forced extraction failure unexpectedly succeeded" >&2
  exit 1
fi
assert_file_contains "$tmp/rollback.out" "Failed to import hermes profile"
assert_file_contains "$rollback_home/.hermes/settings.json" "old"
rm -f "$fakebin/tar"

openclaw_home="$tmp/openclaw-home"
mkdir -p "$openclaw_home/.openclaw/feishu" "$openclaw_home/.openclaw/openclaw-weixin/node_modules/gtoken/node_modules/.bin" "$openclaw_home/.openclaw/browser/openclaw/user-data" "$openclaw_home/.openclaw/skills" "$tmp/linked-skill"
printf 'token\n' > "$openclaw_home/.openclaw/feishu/config"
printf 'skill\n' > "$tmp/linked-skill/SKILL.md"
ln -s ../uuid "$openclaw_home/.openclaw/openclaw-weixin/node_modules/gtoken/node_modules/.bin/uuid"
ln -s /tmp/chrome-lock "$openclaw_home/.openclaw/browser/openclaw/user-data/SingletonLock"
ln -s "$tmp/linked-skill" "$openclaw_home/.openclaw/skills/lark-minutes"
cat > "$openclaw_home/.openclaw/settings.json" <<'EOF'
{
  "mcpServers": {
    "demo": {
      "command": "demo"
    }
  },
  "permissions": {
    "allow": ["demo"]
  }
}
EOF
run_tool "$openclaw_home" export-openclaw-source "$tmp/openclaw-source.tgz" > "$tmp/export-openclaw.out"
"$real_tar" -tzf "$tmp/openclaw-source.tgz" | grep -F "node_modules" >/dev/null 2>&1 && {
  echo "OpenClaw source export included node_modules" >&2
  exit 1
}
"$real_tar" -tzf "$tmp/openclaw-source.tgz" | grep -F "SingletonLock" >/dev/null 2>&1 && {
  echo "OpenClaw source export included browser runtime locks" >&2
  exit 1
}
"$real_tar" -tzf "$tmp/openclaw-source.tgz" | grep -F ".openclaw/skills/lark-minutes/SKILL.md" >/dev/null 2>&1 || {
  echo "OpenClaw source export did not dereference skill links" >&2
  exit 1
}
cat > "$fakebin/tar" <<EOF
#!/bin/sh
case "\$1" in
  -*x*)
    case "\$1" in
      *m*) ;;
      *) echo "migration extraction did not disable mtime restore" >&2; exit 9 ;;
    esac
    ;;
esac
exec "$real_tar" "\$@"
EOF
chmod +x "$fakebin/tar"
if run_tool "$openclaw_home" migrate-openclaw-dry-run "$tmp/openclaw-source.tgz" "$tmp/migration-report.md" merge skip > "$tmp/migrate.out" 2>&1; then
  echo "migration dry run unexpectedly succeeded without hermes" >&2
  exit 1
fi
assert_file_contains "$tmp/migrate.out" "missing the Hermes command"

export HERMES_LOG="$tmp/hermes.log"
cat > "$fakebin/hermes" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$HERMES_LOG"
[ "${1:-}" = "--overwrite" ] && { echo "global overwrite rejected" >&2; exit 64; }
if [ "${HERMES_REFUSE_APPLY:-}" = "1" ]; then
  case " $* " in
    *" --yes "*) echo "Refusing to apply"; exit 0 ;;
  esac
fi
case "$*" in
  *"claw migrate "*) echo "fake migration completed"; exit 0 ;;
  config\ set*) exit 0 ;;
esac
exit 0
EOF
chmod +x "$fakebin/hermes"

TMPDIR=/dev/null run_tool "$openclaw_home" migrate-openclaw-dry-run "$tmp/openclaw-source.tgz" "$tmp/migration-report-ok.md" merge skip > "$tmp/migrate-ok.out"
assert_file_contains "$tmp/migration-report-ok.md" "fake migration completed"
assert_file_contains "$HERMES_LOG" "claw migrate"
assert_file_contains "$HERMES_LOG" "claw migrate --overwrite"
assert_file_contains "$HERMES_LOG" "--dry-run"
assert_file_contains "$HERMES_LOG" "--skill-conflict merge"
assert_file_contains "$HERMES_LOG" "--workspace-target skip"

if HERMES_REFUSE_APPLY=1 TMPDIR=/dev/null run_tool "$openclaw_home" migrate-openclaw-apply "$tmp/openclaw-source.tgz" "$tmp/migration-refuse.md" overwrite preserve > "$tmp/migrate-refuse.out" 2>&1; then
  echo "migration apply unexpectedly succeeded after refusal report" >&2
  exit 1
fi
unset HERMES_REFUSE_APPLY
assert_file_contains "$tmp/migration-refuse.md" "Refusing to apply"
assert_file_contains "$tmp/migrate-refuse.out" "Hermes migration failed"

TMPDIR=/dev/null run_tool "$openclaw_home" migrate-openclaw-apply "$tmp/openclaw-source.tgz" "$tmp/migration-apply.md" overwrite preserve > "$tmp/migrate-apply.out"
assert_file_contains "$tmp/migration-apply.md" "Migration completed."
assert_file_contains "$HERMES_LOG" "--yes"
assert_file_contains "$HERMES_LOG" "--skill-conflict overwrite"
assert_file_contains "$HERMES_LOG" "--workspace-target preserve"
[ -f "$openclaw_home/.hermes/feishu/config" ] || { echo "apply did not copy channel config" >&2; exit 1; }
assert_file_contains "$openclaw_home/.hermes/settings.json" "mcpServers"
rm -f "$fakebin/tar"
