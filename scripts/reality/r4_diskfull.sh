#!/bin/bash
# r4_diskfull.sh <scenario: vlog|index> - Phase 7 R4. Creates a 512 MB APFS disk image, mounts
# it, runs bench_crash fill inside it under run_safe.sh, then detaches and deletes the image.
# The store never touches the real disk. The image file's allocated size is added to the ledger
# as its own line (bytes hdiutil/diskimages-helper wrote to the real disk for the image).
set -u
SCEN="$1"
SCRATCH="${REALITY_SCRATCH:?set REALITY_SCRATCH}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMG="$SCRATCH/img/r4-$SCEN.dmg"; MNT="$SCRATCH/img/mnt-$SCEN"
mkdir -p "$SCRATCH/img" "$MNT"
[ -e "$IMG" ] && { echo "image $IMG exists; remove it first"; exit 1; }
# Wait for the start conditions run_safe.sh checks (AC, load, cooldown) before creating the image,
# so a refused start does not waste an image's worth of writes.
LAST="$SCRATCH/reality_last_end"
until pmset -g batt | head -1 | grep -q "AC Power" && \
      [ "$(sysctl -n vm.loadavg | awk '{print ($2<=3.0)}')" = 1 ] && \
      { [ ! -f "$LAST" ] || [ $(( $(date +%s) - $(cat "$LAST") )) -ge 125 ]; }; do sleep 10; done
hdiutil create -size 512m -fs APFS -volname "RFULL$SCEN" -type UDIF "$IMG" >/dev/null || exit 1
hdiutil attach -nobrowse -mountpoint "$MNT" "$IMG" >/dev/null || { rm -f "$IMG"; exit 1; }
echo "[r4] image $IMG mounted at $MNT: $(df -k "$MNT" | awk 'NR==2 {print "size_kb="$2" free_kb="$4}')"
"$ROOT/scripts/reality/run_safe.sh" "r4-$SCEN" "$ROOT/build/bench/bench_crash" fill --dir "$MNT/store" \
  --scenario "$SCEN" --filler "$MNT/filler" --leave-mb 8 --free-check-path "$SCRATCH" \
  --max-write 1500000000
RC=$?
hdiutil detach "$MNT" >/dev/null || hdiutil detach -force "$MNT" >/dev/null
ALLOC=$(( $(du -k "$IMG" | awk '{print $1}') * 1024 ))
printf 'r4-image-%s\t%s\t0\t0\t0\t0\t0\t0\t0\n' "$SCEN" "$ALLOC" >> "$SCRATCH/reality_ledger.tsv"
echo "[r4] image allocated bytes (added to the ledger): $ALLOC"
rm -f "$IMG"; rmdir "$MNT" 2>/dev/null
echo "[r4] image deleted; real disk free: $(df -k "$SCRATCH" | awk 'NR==2 {print $4*1024}')"
exit $RC
