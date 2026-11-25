#!/bin/zsh
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B=${BDIR:-$ROOT/build/mac}; D=${GTAV_DATA:-$HOME/gtav-data}
T=$ROOT/tools/mac
O=${OUT:-$ROOT/out/mac-runs}
mkdir -p $O
name=$1; plan=$2; shift 2
L=$O/$name.log; CMD=$B/logs/native_cmd; PAD=$O/$name.pad
mkdir -p $B/logs
rm -f $L $CMD
printf '0.00 0.12 1000 0 0 0 0 0 0\n1.90 0.10 0000 0 0 0 0 0 0\n' > $PAD
cd $B
[ -n "$SHOTS" ] && mkdir -p $O/shots_$name
env GTAV_IGNORE_SETTINGS=1 ${SHOTS:+GTAV_NATIVE_SHOT_DIR=$O/shots_$name} ${SHOTS:+GTAV_SHOT_FRAMES=$SHOTS} ./gtav \
  --game_data_root=$D/game --user_data_root=$D/user --update_data_root=$D/update \
  --gpu_plugin=xenos --headless=true --log_level=info --log_file=$L --gtav_native=on --gpu_skip_rendering=true \
  --gpu_skip_rendering_lag_ms=4 --vsync=false --gtav_native_vsync=false --gtav_native_hidden=true --fullscreen=false \
  --audio_mute=true --mnk_mode=true --input_script=$PAD --dlc_enable=true --gtav_native_stats=true \
  --gtav_debug_commands=true \
  $(python3 -I $T/switch_flags.py ${TOML:-$ROOT/gtav/gtav_switch.toml}) \
  --gtav_native_pass_gpu_times=true "$@" > $O/$name.stdout 2>&1 &
PID=$!
echo $PID > $O/$name.pid
S=$(date +%s)
until grep -q "layout active" $L 2>/dev/null; do
  sleep 5
  kill -0 $PID 2>/dev/null || { echo "exited before player"; exit 1; }
  [ $(( $(date +%s) - S )) -gt 900 ] && { echo "timeout"; kill -9 $PID; exit 2; }
done
until grep -E "replay frame [0-9]+: [0-9]+ drawn" $L | awk '{for(i=1;i<=NF;i++) if($i=="drawn,") n=$(i-1)} END{exit !(n>=300)}'; do
  sleep 5
  kill -0 $PID 2>/dev/null || { echo "exited before scene"; exit 1; }
  [ $(( $(date +%s) - S )) -gt 900 ] && { echo "timeout"; kill -9 $PID; exit 2; }
done
echo "scene at $(( $(date +%s) - S )) s"
mark() { echo "MARK $1" >> $O/$name.marks; echo "$(date +%T) $1 line $(wc -l < $L)" >> $O/$name.marks; }
cmd() { echo "$1" > $CMD; sleep 2; }
pad() { printf "$1\n" > $PAD; }
: > $O/$name.marks
source $plan
kill $PID; sleep 3; kill -9 $PID 2>/dev/null
echo done
