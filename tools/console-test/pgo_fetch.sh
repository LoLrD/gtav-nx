#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/common.sh"

MODE="${1:-dump}"
case "$MODE" in
  dump|--no-dump|--clean) ;;
  *) echo "usage: pgo_fetch.sh [--no-dump | --clean]" >&2; exit 2 ;;
esac
PGO_SD="$SD_DIR/pgo"
PGO_OUT="${PGO_OUT:-$ROOT/out/nx-pgo-use}"
WAIT_SECS="${PGO_WAIT_SECS:-900}"
BATCH=200

check_console

url_names() { sed -e 's/%/%25/g' -e 's/#/%23/g' -e 's/ /%20/g'; }

if [[ "$MODE" == "--clean" ]]; then
  files=()
  while IFS= read -r f; do [[ -n "$f" ]] && files+=("$f"); done < <(ftp_list "$PGO_SD")
  for ((i = 0; i < ${#files[@]}; i += BATCH)); do
    quotes=()
    for f in "${files[@]:i:BATCH}"; do quotes+=(-Q "*DELE $PGO_SD/$f"); done
    ftp_curl "$FTP_BASE" "${quotes[@]}" -o /dev/null
  done
  echo "ok: ${#files[@]} files removed from $PGO_SD"
  exit 0
fi

if [[ "$MODE" == "dump" ]]; then
  flag="$(mktemp)"
  ftp_put "$flag" "pgo/dump.flag"
  rm -f "$flag"
  echo "dump requested ($PGO_SD/dump.flag), waiting for the game (up to $WAIT_SECS s)"
  for ((t = 0; ; t += 5)); do
    listing="$(ftp_list "$PGO_SD")"
    grep -qxF dump.flag <<<"$listing" || break
    if ((t >= WAIT_SECS)); then
      ftp_del "$PGO_SD/dump.flag"
      echo "error: the game did not answer the dump request: is the PGO=GENERATE build running?" >&2
      exit 1
    fi
    sleep 5
  done
  echo "profile written after ${t} s"
fi

listing="$(ftp_list "$PGO_SD")"
names="$(grep '\.gcda$' <<<"$listing" || true)"
[[ -n "$names" ]] || { echo "error: no .gcda files in $PGO_SD" >&2; exit 1; }
files=()
while IFS= read -r f; do files+=("$f"); done <<<"$names"
encoded=()
while IFS= read -r f; do encoded+=("$f"); done < <(url_names <<<"$names")

fetch_dir="$PGO_OUT/pgo.fetch"
rm -rf "$fetch_dir"
mkdir -p "$fetch_dir"
for ((i = 0; i < ${#files[@]}; i += BATCH)); do
  args=()
  for ((j = i; j < i + BATCH && j < ${#files[@]}; j++)); do
    args+=("${FTP_BASE}${PGO_SD}/${encoded[j]}" -o "$fetch_dir/${files[j]}")
  done
  ftp_curl --globoff "${args[@]}"
  echo "fetched $((j)) / ${#files[@]}"
done
got=$(find "$fetch_dir" -name '*.gcda' -size +0 | wc -l | tr -d ' ')
((got == ${#files[@]})) || { echo "error: $got of ${#files[@]} .gcda files downloaded" >&2; exit 1; }

rm -rf "$PGO_OUT/pgo"
mv "$fetch_dir" "$PGO_OUT/pgo"
echo "ok: $got .gcda files ($(du -sh "$PGO_OUT/pgo" | cut -f1)) in $PGO_OUT/pgo"
for dir in "$PGO_OUT/CMakeFiles/gtav_recomp.dir" "$PGO_OUT/CMakeFiles/gtav.dir" "$PGO_OUT/plume"; do
  if [[ -d "$dir" ]]; then
    find "$dir" \( -name '*.obj' -o -name '*.gch' \) -delete
    echo "objects built with the previous profile removed from ${dir#"$ROOT"/}"
  fi
done
