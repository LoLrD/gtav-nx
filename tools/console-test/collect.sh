#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

NAME="${1:?usage: collect.sh NAME}"
check_console
RUN_DIR="$OUT_ROOT/$NAME"
mkdir -p "$RUN_DIR"
cp "$DEPLOYED_DIR/gtav.elf" "$RUN_DIR/"
if ftp_get "$SD_DIR/logs/gtav.log" "$RUN_DIR/gtav.log" 2>/dev/null; then
  ftp_del "$SD_DIR/logs/gtav.log"
else
  echo "warning: no gtav.log on the console" >&2
fi
for name in rex_crash.log rex_stderr.log rex_profile.log; do
  if ftp_get "$SD_DIR/logs/rex/$name" "$RUN_DIR/$name" 2>/dev/null; then
    ftp_del "$SD_DIR/logs/rex/$name"
  else
    rm -f "$RUN_DIR/$name"
  fi
done
if [[ -f "$RUN_DIR/gtav.log" ]] && grep -q "SWEEP" "$RUN_DIR/gtav.log"; then
  python3 "$CT_DIR/sweep_table.py" "$RUN_DIR/gtav.log"
fi
