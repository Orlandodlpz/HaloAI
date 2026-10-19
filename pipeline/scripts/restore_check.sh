#!/usr/bin/env bash
# Prove the newest server backup restores (B5). Run on the Mac, inside the repo:
#   pipeline/scripts/restore_check.sh
# Needs: the server's SSH key, the age private key (only on this Mac), and the local Docker
# Postgres running (cd pipeline && docker compose up -d).
#
# Fetches the newest encrypted backup, decrypts it here, restores it into a throwaway local
# database (haloai_restore, dropped first) and prints row counts per table next to the live
# server's. The decrypted dump lives in a private temp folder and is deleted at the end.
set -euo pipefail

HOST="${HALOAI_HOST:-ubuntu@api.haloai-cov.io}"
SSH_KEY="${HALOAI_SSH_KEY:-$HOME/.ssh/haloai_oracle}"
AGE_KEY="${HALOAI_BACKUP_KEY:-$HOME/Documents/haloai-secrets/haloai-backup.agekey}"
SSH=(ssh -n -i "$SSH_KEY" -o BatchMode=yes "$HOST")

cd "$(git rev-parse --show-toplevel)/pipeline"
WORK="$(mktemp -d)"
chmod 700 "$WORK"
trap 'rm -rf "$WORK"' EXIT

NEWEST="$("${SSH[@]}" 'sudo ls -1t /var/backups/haloai/ | grep "^haloai-.*\.dump\.age$" | head -1')"
[ -n "$NEWEST" ] || { echo "no backup on the server yet" >&2; exit 1; }
echo "newest backup: $NEWEST"
"${SSH[@]}" "sudo cat /var/backups/haloai/$NEWEST" > "$WORK/backup.age"
age --decrypt --identity "$AGE_KEY" "$WORK/backup.age" > "$WORK/backup.dump"
echo "decrypted: $(wc -c < "$WORK/backup.dump" | tr -d ' ') bytes"

psql_local() { docker compose exec -T db psql -U haloai -v ON_ERROR_STOP=1 "$@"; }
psql_local -d haloai -qc "DROP DATABASE IF EXISTS haloai_restore" -c "CREATE DATABASE haloai_restore"
docker compose exec -T db pg_restore -U haloai -d haloai_restore --no-owner --no-acl --exit-on-error < "$WORK/backup.dump"

COUNT_SQL="SELECT string_agg(t || '=' || n, ' ' ORDER BY t) FROM (
  SELECT table_name AS t, (xpath('/row/c/text()', query_to_xml(format('SELECT count(*) AS c FROM %I', table_name), false, true, '')))[1]::text AS n
  FROM information_schema.tables WHERE table_schema = 'public' AND table_type = 'BASE TABLE') x"
RESTORED="$(psql_local -d haloai_restore -At -c "$COUNT_SQL")"
LIVE="$("${SSH[@]}" "sudo -u postgres psql -d haloai -At -c \"$COUNT_SQL\"")"

echo
printf "%-22s %10s %10s\n" table restored live
MISSING=0
for pair in $LIVE; do
  t="${pair%%=*}"; live="${pair#*=}"
  restored="$(tr ' ' '\n' <<< "$RESTORED" | grep "^$t=" | cut -d= -f2 || true)"
  printf "%-22s %10s %10s\n" "$t" "${restored:-MISSING}" "$live"
  [ -n "$restored" ] || MISSING=1
done
psql_local -d haloai -qc "DROP DATABASE haloai_restore"
echo
if [ "$MISSING" = 1 ]; then echo "RESTORE CHECK FAILED: tables missing from the backup" >&2; exit 1; fi
echo "restore check passed (live counts can be higher: requests made since the backup)"
