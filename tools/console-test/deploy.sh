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
  existing=$(ftp_list "$SD_DIR/spirv" | sort)
  local_files=()
  while IFS= read -r f; do local_files+=("$f"); done < <(cd "$SPIRV" && ls | sort)
  todo=()
  for f in "${local_files[@]}"; do
    grep -qxF "$f" <<<"$existing" || todo+=("$f")
  done
  echo "spirv: ${#todo[@]} of ${#local_files[@]} to upload"
  for ((i = 0; i < ${#todo[@]}; i += 200)); do
    batch=("${todo[@]:i:200}")
    list=$(printf "$SPIRV/%s," "${batch[@]}")
    ftp_curl --ftp-create-dirs -T "{${list%,}}" "${FTP_BASE}${SD_DIR}/spirv/" > /dev/null
    echo "spirv: $((i + ${#batch[@]})) / ${#todo[@]}"
  done
fi
