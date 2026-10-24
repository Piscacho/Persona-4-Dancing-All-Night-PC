#!/usr/bin/env bash
# tools/restart.sh [run.sh args]: restarts the game with frame stats, P4D_PAD_FILE and
# P4D_TOGGLE_FILE (out/pad, out/toggles) and enters the level (title: cross, cross).
cd -- "$(dirname -- "$0")/.."
T=tools
pkill -x p4d-probe; sleep 2; pkill -9 -x p4d-probe; sleep 1
: > out/pad; echo ${BASE_MASK:-0} > out/toggles
P4D_PAD_FILE=$PWD/out/pad P4D_FRAME_STATS=1 P4D_TOGGLE_FILE=$PWD/out/toggles P4D_FPS_LIMIT=0 \
    ${CPUS:+taskset -c $CPUS} setsid stdbuf -oL -eL bash run.sh "$@" > out/session.log 2>&1 < /dev/null &
# Title menu: the pad is opened and frame stats report a light scene for a while.
for i in $(seq 1 180); do
    sleep 1
    [[ $(grep -c '^Frame stats' out/session.log) -ge 3 ]] && grep -q 'pad opened' out/session.log && break
done
sleep ${TITLE_WAIT:-6}
$T/press.sh cross; sleep 6; $T/press.sh cross
for i in $(seq 1 90); do
    sleep 2
    last=$(grep '^Frame stats' out/session.log | tail -1 | sed -E 's/.* ([0-9]+) draws\/frame.*/\1/')
    [[ -n $last && $last -gt 600 ]] && break
done
sleep 10
grep '^Frame stats' out/session.log | tail -1
