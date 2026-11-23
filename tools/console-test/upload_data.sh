#!/usr/bin/env bash
set -uo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"
DATA="${GTAV_DATA:-$HOME/gtav-data}"
check_console

remote_size() {
  ftp_curl --head "${FTP_BASE}${SD_DIR}/$1" 2>/dev/null | tr -d '\r' | awk -F': ' 'tolower($1) == "content-length" {print $2}'
}

files=()
while IFS= read -r f; do files+=("$f"); done < <(
  cd "$DATA" && { find game update -type f; find user/0000000000000000 -type f; } | sort)

total=${#files[@]}
i=0
failed=0
start=$(date +%s)
for rel in "${files[@]}"; do
  i=$((i + 1))
  local_size=$(stat -f %z "$DATA/$rel")
  dest="data/$rel"
  if [[ "$(remote_size "$dest")" == "$local_size" ]]; then
    echo "[$i/$total] skip $rel"
    continue
  fi
  t0=$(date +%s)
  if ftp_put "$DATA/$rel" "$dest"; then
    dt=$(( $(date +%s) - t0 ))
    echo "[$i/$total] ok $rel $((local_size / 1048576)) MB in ${dt}s ($(( local_size / 1048576 / (dt > 0 ? dt : 1) )) MB/s)"
  else
    echo "[$i/$total] FAILED $rel"
    failed=$((failed + 1))
  fi
done
echo "done in $(( $(date +%s) - start ))s, failed $failed"
