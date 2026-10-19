#!/usr/bin/env bash
# Nightly backup of the HaloAI database (B5), run by haloai-backup.timer as root.
#
# Dump (pg_dump custom format), encrypt with age before it touches the disk (only the public key
# is on the server; the private key is on Orlando's Mac), keep the newest 7 in /var/backups/haloai,
# and upload this one to Oracle Object Storage through a write-only pre-authenticated URL: the
# server can add backups there but can never read or delete them.
#
# Secrets in /opt/haloai/env/haloai.env: BACKUP_AGE_RECIPIENT (public key), BACKUP_PAR_URL.
# Run now: sudo systemctl start haloai-backup && journalctl -u haloai-backup -n 20
# Restore check (on the Mac): pipeline/scripts/restore_check.sh
set -euo pipefail

ENV="${HALOAI_ENV:-/opt/haloai/env/haloai.env}"  # overridable only for testing the script
DIR="${HALOAI_BACKUP_DIR:-/var/backups/haloai}"
KEEP=7

setting() { grep "^$1=" "$ENV" | cut -d= -f2- || true; }
RECIPIENT="$(setting BACKUP_AGE_RECIPIENT)"
PAR_URL="$(setting BACKUP_PAR_URL)"
if [ -z "$RECIPIENT" ]; then
  echo "backup FAILED: BACKUP_AGE_RECIPIENT is not set, refusing to write an unencrypted dump" >&2
  exit 1
fi

install -d -m 700 "$DIR"
NAME="haloai-$(date -u +%Y%m%d-%H%M%SZ).dump.age"
PART="$DIR/.$NAME.part"
trap 'rm -f "$PART"' EXIT

# pipefail: a failed pg_dump fails the backup instead of leaving a short file that looks fine.
sudo -u postgres pg_dump --format=custom haloai | age --encrypt --recipient "$RECIPIENT" > "$PART"
chmod 600 "$PART"
mv "$PART" "$DIR/$NAME"
SIZE="$(stat -c %s "$DIR/$NAME")"

# Keep the newest $KEEP on the server.
ls -1t "$DIR"/haloai-*.dump.age | tail -n +$((KEEP + 1)) | xargs -r rm -f

if [ -z "$PAR_URL" ]; then
  echo "backup $NAME ($SIZE bytes) kept on the server only: BACKUP_PAR_URL is not set" >&2
  exit 1
fi
curl --fail --silent --show-error --retry 3 --max-time 120 --upload-file "$DIR/$NAME" "${PAR_URL}${NAME}"
echo "backup $NAME ($SIZE bytes) kept on the server and uploaded to Object Storage"
