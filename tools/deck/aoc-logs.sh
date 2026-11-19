#!/bin/bash
# Age of Calamity Recomp logs on the Steam Deck: lists and sums up the sessions played, follows the running session and
# bundles everything to send. Lives in the game folder (tools/build.py dist); runs in Konsole or over SSH.
#
#   ./aoc-logs.sh                    summary of the last session (same as "summary 1")
#   ./aoc-logs.sh list [N]           one line per session, the N newest (default 15): date, minutes, fps, APU watts
#                                    (mean and peak), battery, highest CPU and GPU temperature, errors
#   ./aoc-logs.sh summary [K] [--rows]  summary of the K-th newest session (1 = the last); --rows lists its "sys:"
#                                    lines, one every 10 s
#   ./aoc-logs.sh live               follows the running session (sys:, fps:, frametime: lines, errors); Ctrl+C quits
#   ./aoc-logs.sh system             the machine right now: TDP, watts, temperatures, fan, clocks, battery
#   ./aoc-logs.sh detailed on|off    turns the performance trace (AOC_TRACE=perf) on or off for the next sessions: GPU
#                                    per frame, pool shadows, write watching; cheap, but the log grows
#   ./aoc-logs.sh performance [K]    the performance log of the K-th newest session (logs/performance/, one row a
#                                    second): fps, stutters and their cause, power, what happened around each "Mark
#                                    this moment" of the quick menu (tools/perf_view.py)
#   ./aoc-logs.sh diag [K]           state recorder reports (freeze, black or dark screen) of the K-th session, read by
#                                    tools/diag_view.py
#   ./aoc-logs.sh bundle             logs/bundle-<date>.tar.gz with the sessions, aoc.ini, the machine state and the
#                                    pictures of the two newest reports (logs/diag/)
#
# Sessions: logs/aoc.log is the last one; aoc.sh keeps each earlier session compressed in logs/sessions/ when the game
# starts (the 60 newest). Summaries come from tools/logsum.py (tools/deck/logsum.py in the repository).
here="$(cd "$(dirname "$0")" && pwd)"
L="$here/logs"
S="$L/sessions"
PY="$here/tools/logsum.py"
INI="$here/aoc.ini"

# The sessions, newest first: logs/aoc.log, then logs/sessions/*.log.gz by name (the name is the start time).
sessions() {
    [ -f "$L/aoc.log" ] && echo "$L/aoc.log"
    ls -1 "$S"/*.log.gz 2>/dev/null | sort -r
}

need_python() {
    command -v python3 >/dev/null || { echo "python3 not found"; exit 1; }
    [ -f "$PY" ] || { echo "missing $PY"; exit 1; }
}

# One sysfs value from the hwmon whose name is $1 ($2 = file); empty when absent.
hw() {
    for d in /sys/class/hwmon/hwmon*; do
        [ "$(cat "$d/name" 2>/dev/null)" = "$1" ] && cat "$d/$2" 2>/dev/null && return
    done
}

machine() {
    w() { [ -n "$1" ] && awk -v v="$1" -v d="$2" 'BEGIN { printf "%.1f", v / d }' || printf -- '-'; }
    echo "date: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "system: $(. /etc/os-release 2>/dev/null; echo "$PRETTY_NAME $VERSION_ID") · kernel $(uname -r)"
    echo "APU: $(w "$(hw amdgpu power1_average)" 1000000) W now; limit (TDP) $(w "$(hw amdgpu power1_cap)" 1000000) W" \
         "(max $(w "$(hw amdgpu power1_cap_max)" 1000000) W)"
    echo "temperatures: CPU $(w "$(hw k10temp temp1_input)" 1000) °C, GPU $(w "$(hw amdgpu temp1_input)" 1000) °C;" \
         "fan $(hw steamdeck_hwmon fan1_input) rpm"
    echo "clocks: GPU $(w "$(hw amdgpu freq1_input)" 1000000) MHz; CPU" \
         "$(for c in /sys/devices/system/cpu/cpu[0-7]/cpufreq/scaling_cur_freq; do w "$(cat "$c")" 1000; printf ' '; done) MHz;" \
         "governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
    echo "GPU busy: $(cat /sys/class/drm/card*/device/gpu_busy_percent 2>/dev/null | head -1) %;" \
         "performance level: $(cat /sys/class/drm/card*/device/power_dpm_force_performance_level 2>/dev/null | head -1)"
    for b in /sys/class/power_supply/*; do
        [ "$(cat "$b/type" 2>/dev/null)" = Battery ] || continue
        v=$(cat "$b/voltage_now" 2>/dev/null); i=$(cat "$b/current_now" 2>/dev/null)
        p=$( [ -n "$v" ] && [ -n "$i" ] && awk -v v="$v" -v i="$i" 'BEGIN { printf "%.1f", v * i / 1e12 }' )
        echo "battery: $(cat "$b/capacity")% $(cat "$b/status"), ${p:--} W"
    done
    echo "memory: $(free -m | awk '/^Mem:/ { print $3 " of " $2 " MiB in use" }')"
    pgrep -a aoc | grep -v aoc-logs | sed 's/^/process: /'
}

cmd=${1:-summary}
case "$cmd" in
list)
    need_python
    n=${2:-15}
    files=$(sessions | head -n "$n")
    [ -n "$files" ] || { echo "no session in $L"; exit 0; }
    # oldest first, so the table reads top to bottom in time
    python3 "$PY" --list $(echo "$files" | tac)
    ;;
summary)
    need_python
    shift
    k=1
    case "$1" in [0-9]*) k=$1; shift ;; esac
    f=$(sessions | sed -n "${k}p")
    [ -n "$f" ] || { echo "there is no session $k (./aoc-logs.sh list)"; exit 1; }
    echo "== $f"
    python3 "$PY" "$f" "$@"
    ;;
live)
    [ -f "$L/aoc.log" ] || { echo "no logs/aoc.log"; exit 1; }
    tail -n 20 -F "$L/aoc.log" | grep --line-buffered -E '^\[host\] (sys:|fps:|frametime:|resolution:)|FATAL|hang:|device lost'
    ;;
system)
    machine
    ;;
detailed)
    case "$2" in
    on)
        if grep -qiE '^[[:space:]]*trace[[:space:]]*=' "$INI" 2>/dev/null; then
            sed -i -E 's/^[[:space:]]*[Tt][Rr][Aa][Cc][Ee][[:space:]]*=.*/trace = perf/' "$INI"
        else
            printf '\n[diagnostics]\ntrace = perf\n' >> "$INI"
        fi
        echo "performance trace on for the next sessions (aoc.ini: trace = perf)"
        ;;
    off)
        sed -i -E '/^[[:space:]]*[Tt][Rr][Aa][Cc][Ee][[:space:]]*=/d' "$INI" 2>/dev/null
        echo "performance trace off (only the sys: lines and the frametime summary)"
        ;;
    *)
        grep -iE '^[[:space:]]*trace[[:space:]]*=' "$INI" 2>/dev/null || echo "off"
        ;;
    esac
    ;;
performance)
    command -v python3 >/dev/null || { echo "python3 not found"; exit 1; }
    f=$(ls -1 "$L"/performance/*.csv 2>/dev/null | sort -r | sed -n "${2:-1}p")
    [ -n "$f" ] || { echo "no performance log ${2:-1} in $L/performance"; exit 1; }
    python3 "$here/tools/perf_view.py" "$f" "${@:3}"
    ;;
diag)
    command -v python3 >/dev/null || { echo "python3 not found"; exit 1; }
    [ -f "$here/tools/diag_view.py" ] || { echo "missing $here/tools/diag_view.py"; exit 1; }
    f=$(sessions | sed -n "${2:-1}p")
    [ -n "$f" ] || { echo "no session ${2:-1}"; exit 1; }
    case "$f" in
    *.gz) t=$(mktemp); gzip -dc "$f" > "$t"; python3 "$here/tools/diag_view.py" "$t" --threads; rm -f "$t" ;;
    *) python3 "$here/tools/diag_view.py" "$f" --threads ;;
    esac
    ;;
bundle)
    ts=$(date +%Y%m%d-%H%M%S)
    tmp=$(mktemp -d)
    machine > "$tmp/machine.txt" 2>&1
    cp "$INI" "$tmp/" 2>/dev/null
    mkdir -p "$tmp/logs"
    sessions | head -n 10 | while read -r f; do cp "$f" "$tmp/logs/"; done
    # the performance logs of the five newest sessions (one CSV row a second, small)
    ls -1 "$L"/performance/*.csv 2>/dev/null | sort -r | head -n 5 | while read -r f; do
        mkdir -p "$tmp/logs/performance" && cp "$f" "$tmp/logs/performance/"
    done
    # the picture copies of the two newest recorder reports (320x180, small); not hits/, marks/ or dump/ (large)
    ls -1d "$L"/diag/2*-* 2>/dev/null | sort -r | head -n 2 | while read -r d; do
        mkdir -p "$tmp/logs/diag" && cp -r "$d" "$tmp/logs/diag/"
    done
    out="$L/bundle-$ts.tar.gz"
    tar -czf "$out" -C "$tmp" .
    rm -rf "$tmp"
    echo "$out ($(du -h "$out" | cut -f1))"
    ;;
*)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
