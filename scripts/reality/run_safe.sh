#!/bin/bash
# run_safe.sh - runs one Phase 7 reality-check command under the machine-safety rules.
#
#   scripts/reality/run_safe.sh <label> <command> [args...]
#
# Before: AC power, free disk >= 80 GB, CPU speed limit 100 (waits otherwise), 1-min load
#         average <= REALITY_MAX_LOAD (default 4; this Mac idles near 2.7), checked after the cooldown, >= 120 s since the
#         last heavy run ended, budget left in the ledger.
# During: polls every 2 s; kills the command (SIGKILL) if: not on AC power, free disk < 50 GB,
#         command RSS > 4 GiB, swap used grew > 1 GiB, run time > 70 min. If the CPU speed limit
#         drops below 100 the command is paused (SIGSTOP) until it is back (the run is flagged).
#         caffeinate -i -w keeps the Mac awake while the command runs.
# After:  appends to the ledger: label, OS-counted bytes written (the command's last [final] or
#         [mon] line), exit code, peak RSS seen, swap growth, device MB read+written (iostat disk0,
#         whole machine), free disk before/after, thermal before/after.
#
# The command gets REALITY_MAX_WRITE = budget left (40e9 minus the ledger sum).
set -u
LABEL="$1"; shift
SCRATCH="${REALITY_SCRATCH:?set REALITY_SCRATCH to the scratch dir}"
LEDGER="$SCRATCH/reality_ledger.tsv"
LAST_END="$SCRATCH/reality_last_end"
LOGDIR="$SCRATCH/logs"; mkdir -p "$LOGDIR"
LOG="$LOGDIR/$(date +%Y%m%d-%H%M%S)-$LABEL.log"
BUDGET=40000000000
MIN_FREE_START=$((80*1000*1000*1000))
MIN_FREE_RUN=$((50*1000*1000*1000))
MAX_RSS_KB=$((4*1024*1024))
MAX_SWAP_GROWTH_MB=1024
MAX_SECS=$((70*60))
MAX_LOAD="${REALITY_MAX_LOAD:-4}"

say() { echo "[run_safe] $*" | tee -a "$LOG"; }
on_ac() { pmset -g batt | head -1 | grep -q "AC Power"; }
free_bytes() { df -k "$SCRATCH" | awk 'NR==2 {print $4*1024}'; }
swap_mb() { sysctl -n vm.swapusage | sed -E 's/.*used = ([0-9.]+)M.*/\1/' | cut -d. -f1; }
speed_limit() { local v; v=$(pmset -g therm | sed -nE 's/.*CPU_Speed_Limit[^0-9]*([0-9]+).*/\1/p' | tail -1); echo "${v:-100}"; }
dev_mb() { iostat -Id disk0 | awk 'NR==3 {print $3}'; }
ledger_sum() { [ -f "$LEDGER" ] && awk -F'\t' '{s+=$2} END {printf "%.0f", s}' "$LEDGER" || echo 0; }

: > "$LOG"
say "label=$LABEL cmd=$*"
on_ac || { say "ABORT: not on AC power ($(pmset -g batt | head -1))"; exit 10; }
FREE0=$(free_bytes)
[ "$FREE0" -ge "$MIN_FREE_START" ] || { say "ABORT: free disk $FREE0 < $MIN_FREE_START"; exit 11; }
USED=$(ledger_sum)
LEFT=$((BUDGET - USED))
[ "$LEFT" -gt 0 ] || { say "ABORT: write budget used up ($USED of $BUDGET)"; exit 13; }
if [ -f "$LAST_END" ]; then
  WAIT=$(( $(cat "$LAST_END") + 120 - $(date +%s) ))
  [ "$WAIT" -gt 0 ] && { say "cooldown: waiting ${WAIT}s since the last heavy run"; sleep "$WAIT"; }
fi
LOAD1=$(sysctl -n vm.loadavg | awk '{print $2}')
awk -v l="$LOAD1" -v m="$MAX_LOAD" 'BEGIN {exit !(l <= m)}' || {
  say "ABORT: load average $LOAD1 > $MAX_LOAD (something else is running)"
  ps -Ao pid,pcpu,comm -r | head -6 | tee -a "$LOG"; exit 12; }
while [ "$(speed_limit)" -lt 100 ]; do say "CPU speed limit $(speed_limit) < 100: waiting"; sleep 30; done
THERM0=$(pmset -g therm | tr '\n' ' ')
SWAP0=$(swap_mb); DEV0=$(dev_mb)
say "before: free=$FREE0 swap_used_mb=$SWAP0 budget_used=$USED left=$LEFT load1=$LOAD1 therm=[$THERM0]"

START=$(date +%s)
REALITY_MAX_WRITE=$LEFT "$@" >> "$LOG" 2>&1 &
PID=$!
caffeinate -i -w "$PID" &
PEAK_RSS_KB=0; PEAK_SWAP=$SWAP0; KILLED=""; PAUSED=0; N=0
while kill -0 "$PID" 2>/dev/null; do
  sleep 2; N=$((N+1))
  RSS=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' '); RSS=${RSS:-0}
  [ "$RSS" -gt "$PEAK_RSS_KB" ] && PEAK_RSS_KB=$RSS
  SW=$(swap_mb); [ "$SW" -gt "$PEAK_SWAP" ] && PEAK_SWAP=$SW
  FR=$(free_bytes); EL=$(( $(date +%s) - START ))
  if [ "$RSS" -gt "$MAX_RSS_KB" ]; then KILLED="RSS ${RSS} KB > 4 GiB"
  elif [ $((SW - SWAP0)) -gt "$MAX_SWAP_GROWTH_MB" ]; then KILLED="swap grew $((SW - SWAP0)) MB"
  elif [ "$FR" -lt "$MIN_FREE_RUN" ]; then KILLED="free disk $FR < 50 GB"
  elif [ "$EL" -gt "$MAX_SECS" ]; then KILLED="run time ${EL}s > 70 min"
  elif [ $((N % 5)) -eq 0 ] && ! on_ac; then KILLED="switched to battery"
  fi
  if [ -n "$KILLED" ]; then kill -9 "$PID" 2>/dev/null; say "KILLED: $KILLED"; break; fi
  if [ $((N % 15)) -eq 0 ] && [ "$(speed_limit)" -lt 100 ]; then
    kill -STOP "$PID"; PAUSED=1; say "paused: CPU speed limit $(speed_limit)"
    while [ "$(speed_limit)" -lt 100 ]; do sleep 30; done
    kill -CONT "$PID"; say "resumed"
  fi
done
wait "$PID"; RC=$?
END=$(date +%s); echo "$END" > "$LAST_END"
WRITTEN=$(sed -nE 's/^\[final\] os_written=([0-9]+).*/\1/p' "$LOG" | tail -1)
[ -z "$WRITTEN" ] && WRITTEN=$(sed -nE 's/^\[mon\].* os_written=([0-9]+).*/\1/p' "$LOG" | tail -1)
[ -z "$WRITTEN" ] && WRITTEN=$(sed -nE 's/^\[Budget\] bytes written: ([0-9]+).*/\1/p' "$LOG" | tail -1)
WRITTEN=${WRITTEN:-0}
FREE1=$(free_bytes); DEV1=$(dev_mb); THERM1=$(pmset -g therm | tr '\n' ' ')
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$LABEL" "$WRITTEN" "$RC" "$((PEAK_RSS_KB*1024))" \
  "$((PEAK_SWAP - SWAP0))" "$(echo "$DEV1 - $DEV0" | bc)" "$FREE0" "$FREE1" "$((END-START))" >> "$LEDGER"
say "after: rc=$RC secs=$((END-START)) os_written=$WRITTEN peak_rss=$((PEAK_RSS_KB*1024)) swap_growth_mb=$((PEAK_SWAP - SWAP0)) device_mb_rw_whole_machine=$(echo "$DEV1 - $DEV0" | bc) free_before=$FREE0 free_after=$FREE1 paused=$PAUSED killed=[${KILLED}] budget_used=$(ledger_sum) of $BUDGET therm_after=[$THERM1]"
say "log: $LOG"
exit "$RC"
