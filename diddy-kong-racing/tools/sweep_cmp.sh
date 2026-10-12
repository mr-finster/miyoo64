#!/bin/sh
# sweep_cmp.sh A B: count frame dumps in A that differ from B (0 = bit-exact).
# "0/N differ" must mean N frames were really compared: exit 2 with an ERROR line
# when either side holds no frames or the two hold different frame sets (a run
# that died early dumps fewer frames; Smash's T11 printed "0 differing" on none).
na=$(ls "$1" 2>/dev/null | wc -l | tr -d ' '); nb=$(ls "$2" 2>/dev/null | wc -l | tr -d ' ')
if [ "$na" -eq 0 ] || [ "$nb" -eq 0 ]; then echo "ERROR: no frames to compare ($1: $na, $2: $nb)"; exit 2; fi
d=0; n=0
for f in "$1"/*; do n=$((n+1)); cmp -s "$f" "$2/$(basename "$f")" || { d=$((d+1)); echo "differs: $(basename "$f")"; }; done
for f in "$2"/*; do [ -e "$1/$(basename "$f")" ] || { d=$((d+1)); echo "only in $2: $(basename "$f")"; }; done
if [ "$na" -ne "$nb" ]; then echo "ERROR: $1 has $na frames, $2 has $nb ($d differ)"; exit 2; fi
echo "$d/$n differ"
