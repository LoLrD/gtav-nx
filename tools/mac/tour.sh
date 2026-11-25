#!/bin/zsh
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B=${BDIR:-$ROOT/build/mac}; D=${GTAV_DATA:-$HOME/gtav-data}
T=$ROOT/tools/mac
O=${OUT:-$ROOT/out/mac-runs}
mkdir -p $O
name=$1; loops=$2; shift 2
L=$B/$name.log; M=$O/$name.mem; CMD=$B/logs/native_cmd
mkdir -p $B/logs
rm -f $L $M $CMD
printf '0.00 0.12 1000 0 0 0 0 0 0\n1.90 0.10 0000 0 0 0 0 0 0\n' > $B/$name.pad
cd $B
GTAV_IGNORE_SETTINGS=1 ./gtav --game_data_root=$D/game --user_data_root=$D/user --update_data_root=$D/update \
  --gpu_plugin=xenos --headless=true --log_level=info --log_file=$L --gtav_native=on --gpu_skip_rendering=true \
  --gpu_skip_rendering_lag_ms=4 --vsync=false --gtav_native_vsync=false --gtav_native_hidden=true --fullscreen=false \
  --audio_mute=true --mnk_mode=true --input_script=$B/$name.pad --dlc_enable=true --gtav_native_stats=true \
  --gtav_debug_commands=true --gtav_native_buffer_budget_mb=96 \
  $(python3 -I $T/switch_flags.py ${TOML:-$ROOT/gtav/gtav_switch.toml}) "$@" > $O/$name.stdout 2>&1 &
PID=$!
echo $PID > $O/$name.pid
S=$(date +%s)
sample() { echo "$(( $(date +%s) - S )) s $1 rss $(ps -o rss= -p $PID) KB $(footprint -p $PID 2>/dev/null | grep -E 'phys_footprint:' | head -1)" >> $M; }
until grep -q "layout active" $L 2>/dev/null; do
  sleep 5
  kill -0 $PID 2>/dev/null || { echo "exited" >> $M; exit 1; }
  [ $(( $(date +%s) - S )) -gt 900 ] && { echo "timeout" >> $M; kill -9 $PID; exit 2; }
done
sample player
sleep 60
sample prologue
cmd() { echo "$1" > $CMD; sleep 2; }
cmd "freeroam"
spots=("195 -933 31" "-1037 -2737 20" "-1388 -586 30" "450 -1000 30" "-560 280 82" "1200 -3000 5" "2747 1500 24" "1693 3590 35" "-74 6427 31" "501 5604 797" "-2300 3300 32" "2500 4100 38" "-3000 100 11" "1100 -500 60")
for l in $(seq 1 $loops); do
  for p in "${spots[@]}"; do
    cmd "tp $p"
    sleep 6
    cmd "freeroam"
    sleep 14
    kill -0 $PID 2>/dev/null || { echo "exited" >> $M; exit 1; }
    sample "loop $l tp $p"
  done
done
sample end
kill $PID; sleep 3; kill -9 $PID 2>/dev/null
echo done >> $M
