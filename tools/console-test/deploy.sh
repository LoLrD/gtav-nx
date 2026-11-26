#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"
check_console
NRO="$NX_OUT/gtav.nro"
SPIRV="${GTAV_SPIRV:-$ROOT/out/spirv}"

ftp_put "$NRO" "gtav-nx.nro"
ftp_put "$ROOT/gtav/gtav_switch.toml" "gtav.toml"
ftp_put "$ROOT/tools/console-test/bench.txt" "bench.txt"
mkdir -p "$ROOT/out/console-test/deployed"
cp -f "$NX_OUT/gtav" "$ROOT/out/console-test/deployed/gtav.elf"
echo "nro + config uploaded"

if [[ "${1:-}" == "--spirv" ]]; then
  manifest="$DEPLOYED_DIR/spirv.md5"
  existing=$(ftp_list "$SD_DIR/spirv" | sort)
  current=$(cd "$SPIRV" && ls | sort | xargs md5 -r | sort -k2)
  todo=()
  while read -r sum f; do
    if ! grep -qxF "$f" <<<"$existing" || ! grep -qxF "$sum $f" "$manifest" 2>/dev/null; then todo+=("$f"); fi
  done <<<"$current"
  echo "spirv: ${#todo[@]} of $(wc -l <<<"$current" | tr -d ' ') to upload"
  for ((i = 0; i < ${#todo[@]}; i += 200)); do
    batch=("${todo[@]:i:200}")
    list=$(printf "$SPIRV/%s," "${batch[@]}")
    ftp_curl --ftp-create-dirs -T "{${list%,}}" "${FTP_BASE}${SD_DIR}/spirv/" > /dev/null
    echo "spirv: $((i + ${#batch[@]})) / ${#todo[@]}"
  done
  printf '%s\n' "$current" > "$manifest"
fi
